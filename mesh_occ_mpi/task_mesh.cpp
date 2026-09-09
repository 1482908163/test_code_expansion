#include "task_mesh.h"
#include "mesh_mpi_types.h"
#include "task_schedule.h"
#include "scaling_profiler.h"
#include "sparse_face_exchange.h"
#include <climits>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
namespace nglib {
#include <nglib.h>
}
namespace {
using FaceKey=std::array<int,3>;
struct TaskMesh {
    int task;
    nglib::Ng_Mesh *mesh;
    std::map<int,int> g2l;
    std::map<Barycentric,int,CompBarycentric> bary;
    std::list<xdFace> faces;
    explicit TaskMesh(int t):task(t),mesh(nglib::Ng_NewMesh()){}
    ~TaskMesh(){nglib::Ng_DeleteMesh(mesh);}
};
FaceKey key(const xdFace &face) {
    std::set<int> vertices;
    for(const auto &b:face.barycv)for(int v:b.gvrtx)if(v>0)vertices.insert(v);
    if(vertices.size()!=3)throw std::runtime_error("task face lost its coarse parent");
    FaceKey result;std::copy(vertices.begin(),vertices.end(),result.begin());return result;
}
std::uint64_t fingerprint(nglib::Ng_Mesh *mesh) {
    std::uint64_t h=1469598103934665603ULL;
    auto add=[&](const void *data,std::size_t n){const auto *b=static_cast<const unsigned char *>(data);for(std::size_t i=0;i<n;++i){h^=b[i];h*=1099511628211ULL;}};
    for(int i=1;i<=nglib::Ng_GetNP(mesh);++i){double x[3];nglib::Ng_GetPoint(mesh,i,x);add(x,sizeof(x));}
    for(int i=1;i<=nglib::Ng_GetNE(mesh);++i){int v[4],domain;nglib::Ng_GetVolumeElement(mesh,i,v,domain);add(v,sizeof(v));add(&domain,sizeof(domain));}
    return h;
}
void append(TaskMesh &task,nglib::Ng_Mesh *merged,const std::map<FaceKey,xdMeshFaceInfo> &parents,
            const std::vector<int> &owner,int rank,int coarse_fd,
            std::map<int,int> &g2l,std::map<Barycentric,int,CompBarycentric> &bary,std::list<xdFace> &faces) {
    std::vector<int> local(nglib::Ng_GetNP(task.mesh)+1,0);
    std::map<int,Barycentric> reverse;for(const auto &b:task.bary)reverse.emplace(b.second,b.first);
    for(int i=1;i<=nglib::Ng_GetNP(task.mesh);++i) {
        const auto b=reverse.find(i);double x[3];nglib::Ng_GetPoint(task.mesh,i,x);
        if(b!=reverse.end()) {
            const auto found=bary.find(b->second);
            if(found!=bary.end()) {
                double y[3];nglib::Ng_GetPoint(merged,found->second,y);
                for(int j=0;j<3;++j)if(std::abs(x[j]-y[j])>1e-10*std::max({1.,std::abs(x[j]),std::abs(y[j])}))
                    throw std::runtime_error("inconsistent task interface coordinates");
                local[i]=found->second;continue;
            }
        }
        require_local_mesh_capacity(static_cast<GlobalCount>(nglib::Ng_GetNP(merged))+1,MPI_COMM_WORLD);
        nglib::Ng_AddPoint(merged,x,local[i]);
        if(b!=reverse.end())bary.emplace(b->second,local[i]);
    }
    for(const auto &v:task.g2l) {
        auto added=g2l.emplace(v.first,local[v.second]);
        if(!added.second && added.first->second!=local[v.second])throw std::runtime_error("duplicate coarse task vertex");
    }
    std::map<std::pair<int,int>,int> descriptors;
    for(const auto &original:task.faces) {
        const auto &parent=parents.at(key(original));const int other=parent.procids[1];
        if(other>=0 && owner[parent.procids[0]]==rank && owner[other]==rank) {
            if(parent.domainidx[0]==parent.domainidx[1])continue;
            if(task.task!=parent.procids[0])continue;
        }
        xdFace f=original;for(int &v:f.lsvrtx)v=local.at(v);
        if(f.geoboundary>coarse_fd) {
            const int outside=(other>=0 && owner[parent.procids[0]]==rank && owner[other]==rank)?parent.domainidx[1]:0;
            const auto descriptor_key=std::make_pair(f.geoboundary,outside);
            auto found=descriptors.find(descriptor_key);
            if(found==descriptors.end()) {
                int x[4];nglib::My_Ng_GetFaceDescriptor(task.mesh,f.geoboundary,x);
                const int next=nglib::Ng_GetNFD(merged)+1;
                if(next>SHRT_MAX)throw std::runtime_error("too many local task face descriptors");
                const int index=nglib::My_Ng_AddFaceDescriptor(merged,next,x[1],outside,x[3]);
                found=descriptors.emplace(descriptor_key,index).first;
            }
            f.geoboundary=static_cast<short>(found->second);
        }
        nglib::Ng_AddSurfaceElementwithIndex(merged,nglib::NG_TRIG,f.lsvrtx,f.geoboundary);faces.push_back(f);
    }
    require_local_mesh_capacity(static_cast<GlobalCount>(nglib::Ng_GetNE(merged))+nglib::Ng_GetNE(task.mesh),MPI_COMM_WORLD);
    for(int i=1;i<=nglib::Ng_GetNE(task.mesh);++i) {
        int v[4],domain;nglib::Ng_GetVolumeElement(task.mesh,i,v,domain);for(int &j:v)j=local.at(j);
        nglib::Ng_AddVolumeElement(merged,nglib::NG_TET,v,domain);
    }
}
}
double GenerateScheduledTasks(void *coarse_raw,void *merged_raw,int tasks,int levels,int maxbarycoord,
    std::map<int,xdMeshFaceInfo> &facemap,std::map<int,int> &g2l,
    std::map<Barycentric,int,CompBarycentric> &bary,std::list<xdFace> &faces) {
    using namespace mesh_research;
    auto *coarse=static_cast<nglib::Ng_Mesh *>(coarse_raw),*merged=static_cast<nglib::Ng_Mesh *>(merged_raw);
    int rank,p;MPI_Comm_rank(MPI_COMM_WORLD,&rank);MPI_Comm_size(MPI_COMM_WORLD,&p);
    auto &profile=scaling::Profiler::instance();const int ne=nglib::Ng_GetNE(coarse);
    if(p<2 || tasks<p-1 || tasks>ne)protocol_error(MPI_COMM_WORLD,"任务数须在进程数减一与粗单元数之间");
    double local_mesh_seconds=0;
    try {
        std::vector<int> labels(ne),home(tasks),owner(tasks),node(p),cell_counts(tasks);std::vector<double> weight(tasks,1.);
        {
            scaling::StageScope stage("metis_partition","compute");
            if(rank==0) {
                idx_t *part=PartitionMesh(coarse,tasks);
                for(int i=0;i<ne;++i)labels[i]=static_cast<int>(part[i]);
                std::free(part);
            }
            check_mpi(MPI_Bcast(labels.data(),ne,MPI_INT,0,MPI_COMM_WORLD),MPI_COMM_WORLD);
        }
        MPI_Comm shared;check_mpi(MPI_Comm_split_type(MPI_COMM_WORLD,MPI_COMM_TYPE_SHARED,rank,MPI_INFO_NULL,&shared),MPI_COMM_WORLD);
        int leader=rank;check_mpi(MPI_Allreduce(MPI_IN_PLACE,&leader,1,MPI_INT,MPI_MIN,shared),shared);
        check_mpi(MPI_Allgather(&leader,1,MPI_INT,node.data(),1,MPI_INT,MPI_COMM_WORLD),MPI_COMM_WORLD);
        check_mpi(MPI_Comm_free(&shared),MPI_COMM_WORLD);
        for(int label:labels) {
            if(label<0 || label>=tasks)throw std::runtime_error("invalid task label");
            ++cell_counts[label];
        }
        if(std::find(cell_counts.begin(),cell_counts.end(),0)!=cell_counts.end())
            throw std::runtime_error("empty closed task in METIS partition");
        for(int t=0;t<tasks;++t)home[t]=1+static_cast<int>(static_cast<long long>(t)*(p-1)/tasks);
        std::map<int,xdMeshFaceInfo> all;std::map<FaceKey,xdMeshFaceInfo> parents;
        std::vector<std::map<int,xdMeshFaceInfo>> task_faces(tasks);
        std::vector<std::map<int,int>> dependencies(tasks);
        {
            scaling::StageScope stage("task_geometry_setup","compute");bool update=true;
            for(int i=1;i<=ne;++i) {
                int fids[4],orient[4],v[4],domain;
                nglib::My_Ng_GetElement_Faces(coarse,i,fids,orient,update);update=false;
                nglib::Ng_GetVolumeElement(coarse,i,v,domain);
                const int t=labels[i-1];if(t<0 || t>=tasks)throw std::runtime_error("invalid task partition");
                for(int f=0;f<4;++f) {
                    fid_xdMeshFaceInfo record;ExtractSurfaceMesh(coarse,fids[f],t,v,&record,0,domain);
                    auto inserted=all.emplace(fids[f],record.mfi);
                    if(!inserted.second) {
                        auto &a=inserted.first->second;if(a.procids[1]!=-1)throw std::runtime_error("nonmanifold coarse task face");
                        a.procids[1]=t;a.domainidx[1]=domain;std::copy_n(record.mfi.svrtx[0],3,a.svrtx[1]);
                    }
                }
            }
            for(const auto &entry:all) {
                const auto &f=entry.second;int a=f.procids[0],b=f.procids[1];
                if(a==b && f.domainidx[0]==f.domainidx[1])continue;
                task_faces[a].insert(entry);weight[a]+=1;
                if(b>=0 && b!=a){task_faces[b].insert(entry);weight[b]+=1;dependencies[a][b]++;dependencies[b][a]++;}
                FaceKey k;std::copy_n(f.svrtx[0],3,k.begin());std::sort(k.begin(),k.end());parents.emplace(k,f);
            }
        }
        std::vector<std::unique_ptr<TaskMesh>> local;
        std::vector<std::uint64_t> hashes(tasks);std::vector<GlobalCount> counts(tasks);
        MPI_Comm queue;check_mpi(MPI_Comm_dup(MPI_COMM_WORLD,&queue),MPI_COMM_WORLD);
        constexpr int request_tag=10,reply_tag=11;
        if(rank==0) {
            scaling::StageScope service("task_scheduler_service","scheduling");
            TaskSchedule schedule(home,node,dependencies,weight,options().task_cut_growth);
            int active=p-1,completed=0;std::vector<int> running(p,-1);
            while(active) {
                double done[6];MPI_Status status;
                check_mpi(MPI_Recv(done,6,MPI_DOUBLE,MPI_ANY_SOURCE,request_tag,queue,&status),queue);
                const int worker=status.MPI_SOURCE,t=static_cast<int>(done[0]);
                if(t!=running[worker])throw std::runtime_error("task completion does not match assignment");
                if(t>=0) {
                    if(done[1]<0 || !std::isfinite(done[1]) || done[2]<=0)throw std::runtime_error("invalid completed task");
                    hashes[t]=(static_cast<std::uint64_t>(done[4])<<32)|static_cast<std::uint64_t>(done[5]);
                    counts[t]=static_cast<GlobalCount>(done[2]);++completed;
                }
                const int next=schedule.claim(worker,options().balance());running[worker]=next;
                check_mpi(MPI_Send(&next,1,MPI_INT,worker,reply_tag,queue),queue);
                if(next<0)--active;
            }
            if(!schedule.empty() || completed!=tasks)throw std::runtime_error("unfinished task queue");
            owner=schedule.owners();profile.set_metric("task_cross_node_faces_before",schedule.initial_cut());
            profile.set_metric("task_cross_node_faces_after",schedule.cut());
            profile.set_metric("task_cross_node_faces_limit",schedule.limit());
            int moved=0;for(int t=0;t<tasks;++t)moved+=node[owner[t]]!=node[home[t]];
            profile.set_metric("task_moved_between_nodes",moved);
            profile.add_communication("task_dispatch",tasks+p-1,tasks+p-1,
                static_cast<std::uint64_t>(tasks+p-1)*sizeof(int),static_cast<std::uint64_t>(tasks+p-1)*6*sizeof(double));
        } else {
            double done[6]={-1,0,0,0,0,0};int t;
            for(;;) {
                {
                    scaling::StageScope dispatch("task_dispatch","communication");
                    check_mpi(MPI_Send(done,6,MPI_DOUBLE,0,request_tag,queue),queue);
                    check_mpi(MPI_Recv(&t,1,MPI_INT,0,reply_tag,queue,MPI_STATUS_IGNORE),queue);
                }
                profile.add_communication("task_dispatch",1,1,6*sizeof(double),sizeof(int));
                if(t<0)break;
                const double start=MPI_Wtime();std::unique_ptr<TaskMesh> task(new TaskMesh(t));NewSubmesh(coarse,task->mesh);
                std::map<IntPair,int,IntPairCompare> edges;
                {scaling::StageScope stage("part_face_create","compute");PartFaceCreate(coarse,t,task_faces[t],maxbarycoord,task->mesh,task->g2l,task->bary,task->faces);}
                {scaling::StageScope stage("surface_refine","compute");Refine(task->mesh,levels,t,task->faces,task->bary,edges);}
                const double mesh_start=MPI_Wtime();
                {scaling::StageScope stage("local_volume_mesh","compute");nglib::Ng_Meshing_Parameters parameters;parameters.fineness=1;
                 if(nglib::Ng_GenerateVolumeMesh(task->mesh,&parameters)!=nglib::NG_OK)throw std::runtime_error("封闭子域体网格生成失败");}
                local_mesh_seconds+=MPI_Wtime()-mesh_start;
                const auto h=fingerprint(task->mesh);
                done[0]=t;done[1]=MPI_Wtime()-start;done[2]=nglib::Ng_GetNE(task->mesh);done[3]=nglib::Ng_GetNP(task->mesh);
                done[4]=static_cast<double>(h>>32);done[5]=static_cast<double>(h&0xffffffffULL);
                local.push_back(std::move(task));
            }
        }
        {scaling::StageScope wait("task_completion_wait","synchronization");
         check_mpi(MPI_Bcast(owner.data(),tasks,MPI_INT,0,queue),queue);
         check_mpi(MPI_Bcast(hashes.data(),tasks,MPI_UINT64_T,0,queue),queue);
         check_mpi(MPI_Bcast(counts.data(),tasks,MPI_INT64_T,0,queue),queue);}
        check_mpi(MPI_Comm_free(&queue),MPI_COMM_WORLD);
        profile.add_communication("task_metadata",rank==0?1:0,rank==0?0:1,
            rank==0?static_cast<std::uint64_t>(tasks)*20*(p-1):0,rank==0?0:static_cast<std::uint64_t>(tasks)*20);
        std::uint64_t signature=1469598103934665603ULL;GlobalCount total=0;
        for(int t=0;t<tasks;++t){signature=(signature^hashes[t])*1099511628211ULL;signature=(signature^static_cast<std::uint64_t>(counts[t]))*1099511628211ULL;total+=counts[t];}
        std::ostringstream sig;sig<<std::hex<<signature;profile.add_metadata("task_mesh_signature",sig.str());
        profile.set_metric("task_generated_elements_global",static_cast<double>(total));
        profile.set_metric("tasks_completed",local.size());profile.set_metric("task_count",tasks);
        std::vector<idx_t> physical(ne);for(int i=0;i<ne;++i)physical[i]=owner[labels[i]];
        {scaling::StageScope stage("face_pipeline_total","algorithm");ExtractPartitionSurfaceMesh(coarse,physical.data(),facemap,nullptr);}
        profile.mark_elapsed("face_complete_elapsed");
        // 覆盖逐任务 PartFaceCreate 的最后一次局部计数，报告最终物理分区的面。
        std::uint64_t physical_faces=0,partition_faces=0;
        for(const auto &entry:facemap) {
            const auto &f=entry.second;
            if(f.procids[0]!=rank && f.procids[1]!=rank)continue;
            if(f.procids[1]<0)++physical_faces;
            else if(f.procids[0]!=f.procids[1])++partition_faces;
        }
        profile.set_metric("physical_boundary_faces",physical_faces);
        profile.set_metric("partition_boundary_faces",partition_faces);
        profile.set_metric("facemap_entries",facemap.size());
        {scaling::StageScope stage("task_merge","compute");
         std::sort(local.begin(),local.end(),[](const std::unique_ptr<TaskMesh> &a,const std::unique_ptr<TaskMesh> &b){return a->task<b->task;});
         for(auto &task:local){append(*task,merged,parents,owner,rank,nglib::Ng_GetNFD(coarse),g2l,bary,faces);task.reset();}}
    } catch(const std::exception &e){protocol_error(MPI_COMM_WORLD,e.what());}
    return local_mesh_seconds;
}
