#include "research_mesh.h"
#include "sparse_face_exchange.h"
#include <cstdlib>
#include <cstring>
#include <limits>
namespace nglib {
#include <nglib.h>
}
namespace mesh_research {
ResearchOptions &options() { static ResearchOptions value; return value; }
}
namespace {
std::vector<mesh_research::CoarseCell> coarse_graph(nglib::Ng_Mesh *mesh) {
    const int ne=nglib::Ng_GetNE(mesh);
    std::vector<mesh_research::CoarseCell> graph(ne);
    std::map<int,std::pair<int,int>> first_face;
    std::set<int> paired;
    bool update=true;
    for(int i=0;i<ne;++i) {
        int verts[4],domain,fids[4],orient[4];
        nglib::Ng_GetVolumeElement(mesh,i+1,verts,domain);
        std::copy_n(verts,4,graph[i].vertices.begin());
        nglib::My_Ng_GetElement_Faces(mesh,i+1,fids,orient,update);update=false;
        double x[4][3];
        for(int j=0;j<4;++j) nglib::Ng_GetPoint(mesh,verts[j],x[j]);
        double a[3],b[3],c[3];
        for(int j=0;j<3;++j){a[j]=x[1][j]-x[0][j];b[j]=x[2][j]-x[0][j];c[j]=x[3][j]-x[0][j];}
        graph[i].volume=std::abs(a[0]*(b[1]*c[2]-b[2]*c[1])-
                                a[1]*(b[0]*c[2]-b[2]*c[0])+a[2]*(b[0]*c[1]-b[1]*c[0]))/6.0;
        if(graph[i].volume<=0) throw std::runtime_error("degenerate coarse tetrahedron");
        double edge2=0;
        for(int u=0;u<4;++u) for(int v=u+1;v<4;++v) for(int d=0;d<3;++d)
            edge2+=(x[u][d]-x[v][d])*(x[u][d]-x[v][d]);
        const double quality=12*std::pow(3*graph[i].volume,2.0/3.0)/edge2;
        graph[i].shape_volume=graph[i].volume*std::max(0.0,1.0/quality-1.0);
        graph[i].shape2_volume=graph[i].shape_volume*std::max(0.0,1.0/quality-1.0);
        for(int k=0;k<4;++k) {
            int fv[3];double y[3][3];
            nglib::My_Ng_GetFace_Vertices(mesh,fids[k],fv);
            std::copy_n(fv,3,graph[i].face_vertices[k].begin());
            for(int j=0;j<3;++j) nglib::Ng_GetPoint(mesh,fv[j],y[j]);
            for(int j=0;j<3;++j){a[j]=y[1][j]-y[0][j];b[j]=y[2][j]-y[0][j];}
            c[0]=a[1]*b[2]-a[2]*b[1];c[1]=a[2]*b[0]-a[0]*b[2];c[2]=a[0]*b[1]-a[1]*b[0];
            graph[i].face_area[k]=0.5*std::sqrt(c[0]*c[0]+c[1]*c[1]+c[2]*c[2]);
            if(graph[i].face_area[k]<=0) throw std::runtime_error("degenerate coarse face");
            const double denom=4*graph[i].face_area[k];
            graph[i].normal_moment[k]={c[0]*c[0]/denom,c[1]*c[1]/denom,c[2]*c[2]/denom,
                c[0]*c[1]/denom,c[0]*c[2]/denom,c[1]*c[2]/denom};
            double edges2=0;
            for(int u=0;u<3;++u) for(int v=u+1;v<3;++v) for(int d=0;d<3;++d)
                edges2+=(y[u][d]-y[v][d])*(y[u][d]-y[v][d]);
            graph[i].face_shape[k]=std::max(0.0,edges2/(4*std::sqrt(3.0)*graph[i].face_area[k])-1.0);
            if(paired.count(fids[k])) throw std::runtime_error("non-manifold coarse mesh");
            auto entry=first_face.emplace(fids[k],std::make_pair(i,k));
            if(!entry.second) {
                const auto prev=entry.first->second;
                graph[i].neighbor[k]=prev.first;graph[prev.first].neighbor[prev.second]=i;
                paired.insert(fids[k]);
            }
        }
    }
    return graph;
}
bool same_face(const xdMeshFaceInfo &a,const xdMeshFaceInfo &b) {
    if(a.outw!=b.outw || a.procids[0]!=b.procids[0] || a.procids[1]!=b.procids[1]) return false;
    const int sides=a.procids[1]>=0?2:1;
    for(int j=0;j<sides;++j) {
        if(a.domainidx[j]!=b.domainidx[j]) return false;
        for(int k=0;k<3;++k) if(a.svrtx[j][k]!=b.svrtx[j][k]) return false;
    }
    return true;
}
}
idx_t *PartitionResearchMesh(void *raw,int parts) {
    using namespace mesh_research;
    auto *mesh=static_cast<nglib::Ng_Mesh *>(raw);
    const int ne=nglib::Ng_GetNE(mesh);
    int rank;MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    auto *labels=static_cast<idx_t *>(std::malloc(static_cast<std::size_t>(ne)*sizeof(idx_t)));
    if(!labels || ne<parts || ne<=0) protocol_error(MPI_COMM_WORLD,"more partitions than coarse cells");
    std::vector<double> all_stats;
    auto &profile=scaling::Profiler::instance();
    constexpr int stat_count=9+phase_features+phase_count;
    if(rank==0) {
        try {
            std::vector<int> part(ne,0);
            if(parts>1) {
                scaling::StageScope time("metis_seed","compute");
                idx_t *initial=PartitionMesh(mesh,parts);
                for(int i=0;i<ne;++i) part[i]=static_cast<int>(initial[i]);
                std::free(initial);
            }
            std::vector<CoarseCell> graph;
            {
                scaling::StageScope time("partition_cost_setup","compute");
                graph=coarse_graph(mesh);
            }
            BalanceResult result;
            {
                scaling::StageScope time("partition_cost_balance","compute");
                result=balance_partition(graph,part,parts,options().cost,options().balance());
            }
            all_stats.resize(static_cast<std::size_t>(parts)*stat_count);
            for(int p=0;p<parts;++p) {
                const auto after=cost_features(result.after[p],options().cost);
                double *s=all_stats.data()+p*stat_count;
                s[0]=predicted_cost(result.before[p],options().cost);
                s[1]=predicted_cost(result.after[p],options().cost);
                s[2]=result.after[p].cells;s[3]=result.after[p].faces;s[4]=result.after[p].volume;
                for(int k=0;k<4;++k)s[5+k]=after[k];
                const auto x=phase_cost_features(result.after[p],options().cost);
                const auto y=predicted_phases(result.after[p],options().cost);
                for(int k=0;k<phase_features;++k)s[9+k]=x[k];
                for(int k=0;k<phase_count;++k)s[9+phase_features+k]=y[k];
            }
            for(int i=0;i<ne;++i)labels[i]=part[i];
            profile.set_metric("partition_cut_before",result.cut_before);
            profile.set_metric("partition_cut_after",result.cut_after);
            profile.set_metric("partition_moves",result.accepted_moves);
            profile.set_metric("partition_proposed_moves",result.proposed_moves);
            profile.set_metric("partition_adopted",result.adopted?1:0);
            profile.set_metric("closure_total_before",result.closure_before);
            profile.set_metric("closure_total_after",result.closure_after);
            profile.set_metric("closure_max_before",result.closure_max_before);
            profile.set_metric("closure_max_after",result.closure_max_after);
            profile.set_metric("candidate_closure_total",result.candidate_closure);
            profile.set_metric("candidate_closure_max",result.candidate_closure_max);
            profile.set_metric("seed_max_seconds",result.seed_max_seconds);
            profile.set_metric("candidate_max_seconds",result.candidate_max_seconds);
            profile.set_metric("partition_rejection_flags",result.rejection_flags);
            profile.set_metric("seed_lower_seconds",result.seed_lower_seconds);
            profile.set_metric("candidate_upper_seconds",result.candidate_upper_seconds);
            profile.set_metric("correction_seconds",result.correction_seconds);
            profile.set_metric("net_gain_lower_seconds",result.net_gain_lower_seconds);
            profile.set_metric("search_timed_out",result.search_timed_out?1:0);
            profile.set_metric("search_skipped",result.search_skipped?1:0);
        } catch(const std::exception &e) {protocol_error(MPI_COMM_WORLD,e.what());}
    }
    // Common to all four ablations: one reproducible seed/label assignment.
    // Its entire cost is inside the measured core interval.
    double local_stats[stat_count];
    if(profile.split_collectives()) {
        scaling::StageScope wait("partition_pre_collective_wait","synchronization");
        check_mpi(MPI_Barrier(MPI_COMM_WORLD),MPI_COMM_WORLD);
    }
    {
        scaling::StageScope time("partition_distribution","communication");
        const MPI_Datatype type=sizeof(idx_t)==4?MPI_INT32_T:MPI_INT64_T;
        check_mpi(MPI_Bcast(labels,ne,type,0,MPI_COMM_WORLD),MPI_COMM_WORLD);
        check_mpi(MPI_Scatter(all_stats.data(),stat_count,MPI_DOUBLE,local_stats,stat_count,MPI_DOUBLE,0,MPI_COMM_WORLD),MPI_COMM_WORLD);
    }
    const std::uint64_t label_bytes=static_cast<std::uint64_t>(ne)*sizeof(idx_t);
    profile.add_communication("partition_distribution",rank==0?parts-1:0,rank?1:0,
        rank==0?(parts-1)*(label_bytes+stat_count*sizeof(double)):0,rank?label_bytes+stat_count*sizeof(double):0);
    const char *names[]={"predicted_cost_before","predicted_cost_after","assigned_coarse_cells",
        "assigned_boundary_faces","assigned_volume","cost_feature_surface","cost_feature_remesh",
        "cost_feature_refine","cost_feature_numbering"};
    for(int k=0;k<9;++k)profile.set_metric(names[k],local_stats[k]);
    for(int k=0;k<phase_features;++k)profile.set_metric("phase_feature_"+std::to_string(k),local_stats[9+k]);
    for(int k=0;k<phase_count;++k)profile.set_metric("phase_prediction_"+std::to_string(k),local_stats[9+phase_features+k]);
    return labels;
}
void SparseGatherFaceMap(std::map<int,xdMeshFaceInfo> &facemap,
                        fid_xdMeshFaceInfo *records,int count,int first_element) {
    using namespace mesh_research;
    std::vector<FaceRecord> local(count);
    for(int i=0;i<count;++i) {
        const auto &src=records[i];auto &dst=local[i];
        dst.fill(0);dst[0]=src.fid;dst[1]=first_element+i/4;
        dst[2]=src.mfi.procids[0];dst[3]=-1;
        std::copy_n(src.mfi.svrtx[0],3,dst.begin()+4);
        dst[10]=src.mfi.domainidx[0];dst[12]=src.mfi.outw;
    }
    MPI_Comm comm;
    {
        scaling::StageScope time("face_sparse_context","communication");
        check_mpi(MPI_Comm_dup(MPI_COMM_WORLD,&comm),MPI_COMM_WORLD);
    }
    auto result=exchange_face_closure(local,comm);
    for(const auto &entry:result) {
        const auto &r=entry.second;xdMeshFaceInfo f{};
        f.procids[0]=r[2];f.procids[1]=r[3];f.domainidx[0]=r[10];f.domainidx[1]=r[11];
        for(int k=0;k<3;++k){f.svrtx[0][k]=r[4+k];f.svrtx[1][k]=r[7+k];}
        f.outw=static_cast<short>(r[12]);facemap.emplace(entry.first,f);
    }
    check_mpi(MPI_Comm_free(&comm),MPI_COMM_WORLD);
    scaling::Profiler::instance().set_metric("face_closure_records",facemap.size());
}
void VerifyFaceClosure(const std::map<int,xdMeshFaceInfo> &sparse,
                      const std::map<int,xdMeshFaceInfo> &global,int rank) {
    using namespace mesh_research;
    std::map<int,int> vertices;
    for(const auto &entry:global) {
        const auto &f=entry.second;
        for(int j=0;j<2;++j)if(f.procids[j]==rank)
            for(int v:f.svrtx[j])vertices.emplace(v,static_cast<int>(vertices.size())+1);
    }
    std::size_t expected=0;
    for(const auto &entry:global) {
        const auto &f=entry.second;
        bool needed=false;
        for(int v:f.svrtx[0])if(vertices.count(v))needed=true;
        if(!needed)continue;
        ++expected;auto it=sparse.find(entry.first);
        if(it==sparse.end() || !same_face(f,it->second))
            protocol_error(MPI_COMM_WORLD,"sparse closure differs from global reference");
    }
    if(expected!=sparse.size())protocol_error(MPI_COMM_WORLD,"unexpected closure entries");
    std::map<Barycvrtx,std::list<int>,CompBarycvrtx> a,b;
    auto copy_global=global,copy_sparse=sparse;
    computeadj(rank,copy_global,vertices,a);computeadj(rank,copy_sparse,vertices,b);
    if(a.size()!=b.size())protocol_error(MPI_COMM_WORLD,"adjacency key count mismatch");
    auto ia=a.begin();auto ib=b.begin();
    for(;ia!=a.end();++ia,++ib) {
        if(std::memcmp(ia->first.gvrtx,ib->first.gvrtx,3*sizeof(int)))
            protocol_error(MPI_COMM_WORLD,"adjacency key mismatch");
        ia->second.sort();ib->second.sort();
        if(ia->second!=ib->second)protocol_error(MPI_COMM_WORLD,"shared vertex/edge/face holders mismatch");
    }
    if(rank==0)std::printf("[VERIFY] sparse face closure and adjacency match global reference\n");
}
