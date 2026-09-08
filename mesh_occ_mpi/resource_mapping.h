#pragma once
#include "partition_cost.h"
#include <fstream>
#include <numeric>
#include <set>

namespace mesh_research {
// 同一节点内的整组分区一起迁移；不改变粗单元归属、共享面或节点内关系。
struct ResourceModel {
    int ranks=0, levels=0, refines=0, rpn=0;
    std::array<std::array<double,phase_features>,phase_count> coefficient{};
    std::vector<double> slowdown;
    void read(const std::string &path) {
        std::ifstream in(path);read(in);
    }
    void read(std::istream &in) {
        std::string magic;int features=0;
        if(!(in>>magic>>ranks>>levels>>refines>>rpn>>features) || magic!="mesh_resource_v1" ||
           ranks<=0 || rpn<=0 || ranks%rpn || features!=phase_features)
            throw std::runtime_error("invalid resource model header");
        for(auto &phase:coefficient)for(auto &value:phase)
            if(!(in>>value) || !std::isfinite(value) || value<0)
                throw std::runtime_error("invalid resource coefficient");
        if(in>>magic)throw std::runtime_error("trailing resource model data");
        slowdown.assign(ranks/rpn,1.0);
    }
    // Names are gathered once before coarse meshing, outside the core interval.
    void read_capacities(const std::string &path,const std::vector<std::string> &hosts) {
        std::ifstream in(path);read_capacities(in,hosts);
    }
    void read_capacities(std::istream &in,const std::vector<std::string> &hosts) {
        std::string magic;int p=0,r=0;
        if(!(in>>magic>>p>>r) || magic!="mesh_capacity_v1" || p!=ranks || r!=rpn ||
           hosts.size()!=static_cast<std::size_t>(ranks))
            throw std::runtime_error("capacity allocation mismatch");
        std::set<std::string> unique;
        for(int i=0;i<ranks;++i) {
            std::string host;double factor;
            if(!(in>>host>>factor) || host!=hosts[i] || !std::isfinite(factor) || factor<=0)
                throw std::runtime_error("capacity hostname/value mismatch");
            if(i%rpn==0) {
                if(!unique.insert(host).second)throw std::runtime_error("non-block node allocation");
                slowdown[i/rpn]=factor;
            } else if(host!=hosts[i-i%rpn] || factor!=slowdown[i/rpn])
                throw std::runtime_error("capacity requires complete contiguous node groups");
        }
        if(in>>magic)throw std::runtime_error("trailing capacity data");
    }
    std::array<double,phase_count> predict(const std::array<double,phase_features> &x) const {
        std::array<double,phase_count> y{};
        for(int k=0;k<phase_count;++k)for(int j=0;j<phase_features;++j)y[k]+=coefficient[k][j]*x[j];
        return y;
    }
};
struct MappingResult {
    std::vector<int> placement;
    double before=0,candidate=0,seconds=0;
    int moved_nodes=0;
    bool adopted=false;
};
inline MappingResult map_node_bundles(const std::vector<double> &work,
        const std::vector<double> &slowdown,int rpn,int shift,
        double min_seconds,double min_fraction,bool optimize) {
    const auto start=std::chrono::steady_clock::now();
    const int p=static_cast<int>(work.size()),nodes=static_cast<int>(slowdown.size());
    if(rpn<=0 || p==0 || p%rpn || nodes!=p/rpn || shift<0 || shift>=p || shift%rpn)
        throw std::runtime_error("invalid node bundle mapping dimensions");
    MappingResult result;result.placement.resize(p);
    std::vector<double> bundle(nodes,0);
    for(int i=0;i<p;++i) {
        if(!std::isfinite(work[i]) || work[i]<=0)throw std::runtime_error("invalid intrinsic work");
        result.placement[i]=(i+shift)%p;
        bundle[i/rpn]=std::max(bundle[i/rpn],work[i]);
    }
    for(int h=0;h<nodes;++h) {
        if(!std::isfinite(slowdown[h]) || slowdown[h]<=0)throw std::runtime_error("invalid node slowdown");
        result.before=std::max(result.before,bundle[h]*slowdown[(h+shift/rpn)%nodes]);
    }
    result.candidate=result.before;
    if(optimize && nodes>1) {
        std::vector<int> heavy(nodes),fast(nodes),target(nodes);
        std::iota(heavy.begin(),heavy.end(),0);std::iota(fast.begin(),fast.end(),0);
        std::stable_sort(heavy.begin(),heavy.end(),[&](int a,int b){return bundle[a]>bundle[b];});
        std::stable_sort(fast.begin(),fast.end(),[&](int a,int b){return slowdown[a]<slowdown[b];});
        result.candidate=0;
        for(int h=0;h<nodes;++h) {
            target[heavy[h]]=fast[h];
            result.candidate=std::max(result.candidate,bundle[heavy[h]]*slowdown[fast[h]]);
        }
        // 真正计入映射时间；固定阈值只筛掉可预见的无收益情况，不使用独立最坏误差相减。
        result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        if(result.before-result.candidate>std::max(min_seconds,min_fraction*result.before)+result.seconds) {
            result.adopted=true;
            for(int i=0;i<p;++i)result.placement[i]=target[i/rpn]*rpn+i%rpn;
            for(int h=0;h<nodes;++h)result.moved_nodes+=target[h]!=(h+shift/rpn)%nodes;
        }
    }
    result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    return result;
}
}
