#include "partition_cost.h"
#include "partition_sampling.h"
#include <cassert>
#include <iostream>
#include <random>
#include <sstream>
using namespace mesh_research;
static double max_work(const std::vector<PartCost> &s,const CostConfig &cfg) {
    double m=0;for(const auto&p:s)m=std::max(m,predicted_cost(p,cfg));return m;
}
static void close(double a,double b) {assert(std::abs(a-b)<1e-8*std::max(1.0,std::abs(a)));}
int main() {
    // 第三次迭代：重排可复现、归属签名忽略标签置换、轮换为双射。
    const auto identity=sampling_cell_order(26411,-1,true);
    assert(identity==sampling_cell_order(26411,17,false));
    const auto order=sampling_cell_order(26411,17,true);
    assert(order==sampling_cell_order(26411,17,true));
    assert(order!=identity && order!=sampling_cell_order(26411,41,true));
    auto sorted=order;std::sort(sorted.begin(),sorted.end());assert(sorted==identity);
    assert(canonical_partition({0,0,1,1,2,2},3)==canonical_partition({2,2,0,0,1,1},3));
    assert(canonical_partition({0,0,1,1,2,2},3)!=canonical_partition({0,1,0,1,2,2},3));
    for(int count:{1,2,3,64,8192}) for(int shift:{0,count/2}) {
        std::set<int> physical;
        for(int p=0;p<count;++p) {
            int rank=physical_partition_rank(p,count,shift);physical.insert(rank);
            assert((rank-shift+count)%count==p);
        }
        assert(static_cast<int>(physical.size())==count);
    }
    bool empty_rejected=false;
    try {canonical_partition({0,0,0},2);} catch(const std::runtime_error &) {empty_rejected=true;}
    assert(empty_rejected);
    CostConfig cfg;cfg.levels=2;cfg.refines=2;cfg.sweeps=10;
    std::mt19937 rng(71);
    for(int repeat=0;repeat<100;++repeat) {
        std::vector<CoarseCell> cells(24);
        std::vector<int> part(24);
        for(int i=0;i<24;++i) {
            cells[i].volume=repeat==0?(i<12?10:1):1+rng()%20;
            cells[i].face_area.fill(1.0);
            cells[i].face_shape.fill(0.25);
            cells[i].shape_volume=cells[i].volume*0.5;
            cells[i].shape2_volume=cells[i].volume*0.25;
            for(int f=0;f<4;++f)cells[i].normal_moment[f]={0.5,0.3,0.2,0.1,0.05,-0.03};
            cells[i].vertices={i+1,i+2,i+3,i+4};
            cells[i].face_vertices={{{i+1,i+2,i+3},{i+2,i+3,i+4},{i+1,i+2,i+4},{i+1,i+3,i+4}}};
            if(i>0)cells[i].neighbor[0]=i-1;
            if(i<23)cells[i].neighbor[1]=i+1;
            part[i]=i/12;
        }
        const auto original=part;
        auto result=balance_partition(cells,part,2,cfg);
        auto actual=partition_stats(cells,part,2);
        assert(result.cut_after<=result.cut_before*(1+cfg.cut_growth));
        assert(result.cut_after==cut_faces(cells,part));
        assert(max_work(actual,cfg)<=max_work(result.before,cfg)*(1+1e-12));
        int transitions=0;
        for(int i=1;i<24;++i)if(part[i]!=part[i-1])++transitions;
        assert(transitions==1);
        for(int p=0;p<2;++p) {
            assert(actual[p].cells>0 && actual[p].cells==result.after[p].cells);
            assert(actual[p].faces==result.after[p].faces);
            close(actual[p].volume,result.after[p].volume);close(actual[p].area,result.after[p].area);
            close(actual[p].inverse_size,result.after[p].inverse_size);
            close(actual[p].shape_volume,result.after[p].shape_volume);
            close(actual[p].boundary_shape,result.after[p].boundary_shape);
            close(actual[p].area2,result.after[p].area2);close(actual[p].volume2,result.after[p].volume2);
            close(actual[p].shape2_volume,result.after[p].shape2_volume);
            close(actual[p].boundary_shape2,result.after[p].boundary_shape2);
            for(int k=0;k<6;++k)close(actual[p].normal_moment[k],result.after[p].normal_moment[k]);
            auto x=phase_cost_features(actual[p],cfg),y=phase_cost_features(result.after[p],cfg);
            for(int k=0;k<phase_features;++k)close(x[k],y[k]);
            assert(actual[p].physical_faces==result.after[p].physical_faces);
        }
        if(repeat==0){assert(result.accepted_moves>0);std::cout<<"heterogeneous fixture: proxy maximum "
            <<max_work(result.before,cfg)<<" -> "<<max_work(result.after,cfg)<<"\n";}
        auto replay=original;auto repeated=balance_partition(cells,replay,2,cfg);
        assert(replay==part && repeated.accepted_moves==result.accepted_moves);
        if(repeat==0) {
            CostConfig seconds_cfg=cfg;
            seconds_cfg.model.enabled=true;seconds_cfg.model.levels=cfg.levels;
            seconds_cfg.model.refines=cfg.refines;seconds_cfg.model.held_out_ranks=2;
            seconds_cfg.model.coefficients[1][2]=1;
            seconds_cfg.min_gain_seconds=0;seconds_cfg.min_gain_fraction=0;
            seconds_cfg.closure_growth=1;
            auto candidate=original;
            auto selected=balance_partition(cells,candidate,2,seconds_cfg);
            assert(selected.adopted && selected.accepted_moves>0);
            assert(max_work(selected.after,seconds_cfg)<max_work(selected.before,seconds_cfg));
            auto closure=dependency_volume(cells,candidate,2);
            assert(closure.first==selected.closure_after && closure.second==selected.closure_max_after);
            // Reject a compute improvement that increases the worst dependency
            // closure, even when the cut-face budget alone would accept it.
            seconds_cfg.closure_growth=0;candidate=original;
            auto constrained=balance_partition(cells,candidate,2,seconds_cfg);
            assert(!constrained.adopted && constrained.proposed_moves>0 && candidate==original);
            assert(constrained.rejection_flags&4);
            seconds_cfg.closure_growth=1;seconds_cfg.min_gain_seconds=1e100;
            candidate=original;auto rejected=balance_partition(cells,candidate,2,seconds_cfg);
            assert(!rejected.adopted && rejected.accepted_moves==0 && candidate==original);
            assert(rejected.rejection_flags&1);
            // Identical mean area and volume, different boundary size tails.
            PartCost mixed=selected.before[0],uniform=mixed;
            mixed.inverse_size*=2;
            auto a=phase_cost_features(uniform,cfg),b=phase_cost_features(mixed,cfg);
            close(a[2],b[2]);close(2*a[3],b[3]);
            // Equal first moments can have different size/shape second moments.
            mixed=uniform;mixed.area2*=2;mixed.boundary_shape2*=2;
            b=phase_cost_features(mixed,cfg);
            close(a[2],b[2]);assert(b[8]>a[8]);close(b[10],2*a[10]);
            seconds_cfg.min_gain_seconds=0;seconds_cfg.model.guarded=true;
            seconds_cfg.model.maximum.fill(1e100);seconds_cfg.search_seconds=100;
            candidate=original;
            auto safe=balance_partition(cells,candidate,2,seconds_cfg);
            assert(safe.adopted && safe.net_gain_lower_seconds>0 && safe.correction_seconds>=0);
            // A nominally profitable partition is rejected by measured error.
            seconds_cfg.model.under_error=100;candidate=original;
            auto uncertain=balance_partition(cells,candidate,2,seconds_cfg);
            assert(!uncertain.adopted && candidate==original && (uncertain.rejection_flags&16));
            assert(uncertain.proposed_moves>0);
            // Unsupported seed is rejected before costly topology/search work.
            seconds_cfg.model.maximum[0]=0.5;candidate=original;
            auto outside=balance_partition(cells,candidate,2,seconds_cfg);
            assert(outside.search_skipped && (outside.rejection_flags&32) && !outside.proposed_moves);
            seconds_cfg.model.maximum[0]=1;seconds_cfg.search_seconds=0;
            auto timed=balance_partition(cells,candidate,2,seconds_cfg);
            assert(timed.search_skipped && timed.search_timed_out && candidate==original);
            // Large overprediction makes even the theoretical best candidate
            // unable to clear the gain test; avoid spending the search budget.
            seconds_cfg.search_seconds=100;seconds_cfg.model.over_error=1;
            auto futile=balance_partition(cells,candidate,2,seconds_cfg);
            assert(futile.search_skipped && (futile.rejection_flags&16));
        }
    }
    // Removing an articulation cell would split its donor component.
    std::vector<CoarseCell> chain(3);std::vector<int> part(3,0);
    chain[0].neighbor[0]=1;chain[1].neighbor[0]=0;chain[1].neighbor[1]=2;chain[2].neighbor[0]=1;
    assert(!can_remove(1,chain,part));assert(can_remove(0,chain,part));
    // Attaching a tetrahedron by one face must not create a vertex-only pinch
    // with another component of the target partition.
    std::vector<CoarseCell> pinch(4);
    pinch[0].vertices={1,2,3,4};pinch[1].vertices={1,2,3,5};
    pinch[2].vertices={2,3,4,6};pinch[3].vertices={1,7,8,9};
    std::map<std::array<int,3>,std::pair<int,int>> faces;
    std::map<int,std::vector<int>> incidence;
    const int side[4][3]={{0,1,2},{0,1,3},{0,2,3},{1,2,3}};
    for(int i=0;i<4;++i) {
        for(int v:pinch[i].vertices)incidence[v].push_back(i);
        for(int k=0;k<4;++k) {
            auto &f=pinch[i].face_vertices[k];
            for(int j=0;j<3;++j)f[j]=pinch[i].vertices[side[k][j]];
            auto key=f;std::sort(key.begin(),key.end());
            auto at=faces.emplace(key,std::make_pair(i,k));
            if(!at.second){auto old=at.first->second;pinch[i].neighbor[k]=old.first;pinch[old.first].neighbor[old.second]=i;}
        }
    }
    assert(can_remove(0,pinch,{0,0,1,1}));
    assert(!manifold_after_move(0,1,pinch,{0,0,1,1},incidence));
    bool rejected=false;try {cfg.weights={0,0,0,0};balance_partition(chain,part,1,cfg);}catch(...){rejected=true;}
    assert(rejected);
    for(const std::string body:{"bad 2 2 2 8 4", "mesh_phase_v2 2 2 2 8 4 -1", "mesh_phase_v2 2 2 2 8 4 nan"}) {
        std::istringstream in(body);bool bad=false;
        try {PhaseModel model;model.read(in);}catch(const std::runtime_error &){bad=true;}
        assert(bad);
    }
    std::ostringstream valid;valid<<"mesh_phase_v3 2 2 2 14 4\n";
    for(int k=0;k<56;++k)valid<<"1 ";
    valid<<"\n0.2 0.3\n";
    for(int k=0;k<14;++k)valid<<"0 1000000000000\n";
    PhaseModel parsed;std::istringstream input(valid.str());parsed.read(input);
    assert(parsed.guarded && parsed.enabled);close(parsed.lower(10),8);close(parsed.upper(10),13);
    std::istringstream extra(valid.str()+"nan");
    try {parsed.read(extra);assert(false);}catch(const std::runtime_error &){}
    std::cout<<"PASS: cost descent, cut budget, connectivity, boundary manifold, conservation, determinism, invalid weights\n";
}
