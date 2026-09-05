#include "partition_cost.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace mesh_research;
static double max_work(const std::vector<PartCost> &s,const CostConfig &cfg) {
    double m=0;for(const auto&p:s)m=std::max(m,predicted_cost(p,cfg));return m;
}
static void close(double a,double b) {assert(std::abs(a-b)<1e-8*std::max(1.0,std::abs(a)));}
int main() {
    CostConfig cfg;cfg.levels=2;cfg.refines=2;cfg.sweeps=10;
    std::mt19937 rng(71);
    for(int repeat=0;repeat<100;++repeat) {
        std::vector<CoarseCell> cells(24);
        std::vector<int> part(24);
        for(int i=0;i<24;++i) {
            cells[i].volume=repeat==0?(i<12?10:1):1+rng()%20;
            cells[i].face_area.fill(1.0);
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
        }
        if(repeat==0){assert(result.accepted_moves>0);std::cout<<"heterogeneous fixture: proxy maximum "
            <<max_work(result.before,cfg)<<" -> "<<max_work(result.after,cfg)<<"\n";}
        auto replay=original;auto repeated=balance_partition(cells,replay,2,cfg);
        assert(replay==part && repeated.accepted_moves==result.accepted_moves);
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
    std::cout<<"PASS: cost descent, cut budget, connectivity, boundary manifold, conservation, determinism, invalid weights\n";
}
