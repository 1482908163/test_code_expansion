#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace mesh_research {
struct CostConfig {
    int levels = 0, refines = 0, sweeps = 4;
    double cut_growth = 0.05;
    // Surface construction/refinement, remeshing, volume refinement, numbering.
    // Defaults count proxy operations, NOT calibrated seconds.
    std::array<double,4> weights{1,1,1,1};
};
struct CoarseCell {
    std::array<int,4> vertices{};
    std::array<std::array<int,3>,4> face_vertices{};
    double volume = 0;
    std::array<double,4> face_area{};
    std::array<int,4> neighbor{-1,-1,-1,-1};
};
struct PartCost {
    int cells = 0, faces = 0;
    double volume = 0, area = 0;
};
struct BalanceResult {
    std::vector<PartCost> before, after;
    long long cut_before = 0, cut_after = 0, accepted_moves = 0;
};
inline std::array<double,4> cost_features(const PartCost &p, const CostConfig &c) {
    if (p.cells == 0) return {0,0,0,0};
    if (p.faces <= 0 || p.area <= 0 || p.volume <= 0)
        throw std::runtime_error("invalid subdomain geometry for cost prediction");
    const double surface_growth = std::pow(4.0,c.levels);
    // A_triangle = sqrt(3)/4 * h^2, V_tet = sqrt(2)/12 * h^3.
    // The average boundary triangle area estimates a subdomain mesh spacing.
    // This geometric proxy is explicitly a hypothesis for Netgen remeshing.
    const double h2 = 4.0*p.area / (std::sqrt(3.0)*p.faces*surface_growth);
    const double remesh = p.volume / (std::sqrt(2.0)/12.0*std::pow(h2,1.5));
    const double volume_growth = std::pow(8.0,c.refines);
    return {p.faces*(1.0+(surface_growth-1.0)/3.0), remesh,
            remesh*(volume_growth-1.0)/7.0, remesh*volume_growth};
}
inline double predicted_cost(const PartCost &p, const CostConfig &c) {
    const auto f = cost_features(p,c);
    double result = 0;
    for (int k=0;k<4;++k) result += c.weights[k]*f[k];
    if (!std::isfinite(result)) throw std::runtime_error("cost model overflow");
    return result;
}
inline std::vector<PartCost> partition_stats(const std::vector<CoarseCell> &cells,
                                           const std::vector<int> &part, int count) {
    if (cells.size()!=part.size() || count < 1) throw std::runtime_error("invalid partition");
    std::vector<PartCost> stats(count);
    for (std::size_t i=0;i<cells.size();++i) {
        if (part[i]<0 || part[i]>=count) throw std::runtime_error("invalid partition label");
        auto &p=stats[part[i]];
        ++p.cells; p.volume+=cells[i].volume;
        for (int k=0;k<4;++k) {
            int n=cells[i].neighbor[k];
            if (n>=static_cast<int>(cells.size())) throw std::runtime_error("invalid neighbor");
            if (n<0 || part[n]!=part[i]) {++p.faces;p.area+=cells[i].face_area[k];}
        }
    }
    return stats;
}
inline long long cut_faces(const std::vector<CoarseCell> &cells,const std::vector<int> &part) {
    long long result=0;
    for (std::size_t i=0;i<cells.size();++i)
        for(int n:cells[i].neighbor)
            if(n>static_cast<int>(i) && part[n]!=part[i]) ++result;
    return result;
}
// Removing one dual-graph vertex must not split any existing component.
// Requiring its remaining same-part neighbors to stay connected is sufficient.
inline bool can_remove(int cell,const std::vector<CoarseCell> &cells,
                       const std::vector<int> &part) {
    std::set<int> required;
    for(int n:cells[cell].neighbor) if(n>=0 && part[n]==part[cell]) required.insert(n);
    if(required.size()<2) return true;
    std::vector<int> queue{*required.begin()};
    std::set<int> visited{queue[0]};
    for(std::size_t j=0;j<queue.size();++j) {
        required.erase(queue[j]);
        if(required.empty()) return true;
        for(int n:cells[queue[j]].neighbor) {
            if(n>=0 && n!=cell && part[n]==part[cell] && visited.insert(n).second)
                queue.push_back(n);
        }
    }
    return false;
}
// A changed boundary is a closed triangle 2-manifold at a vertex iff its
// link is empty or one cycle. Check all four affected vertices in both parts;
// boundary links at all other vertices are unchanged. This does not repair
// pre-existing non-manifold regions away from the move.
inline bool manifold_after_move(int cell,int target,const std::vector<CoarseCell> &cells,
        const std::vector<int> &part,const std::map<int,std::vector<int>> &incidence) {
    const int source=part[cell];
    const auto owner=[&](int i){return i==cell?target:part[i];};
    for(int vertex:cells[cell].vertices)for(int p:{source,target}) {
        std::map<int,std::vector<int>> link;
        for(int i:incidence.at(vertex)) {
            if(owner(i)!=p)continue;
            for(int k=0;k<4;++k) {
                const int n=cells[i].neighbor[k];
                if(n>=0 && owner(n)==p)continue;
                const auto &f=cells[i].face_vertices[k];
                if(std::find(f.begin(),f.end(),vertex)==f.end())continue;
                int a=-1,b=-1;
                for(int v:f)if(v!=vertex){if(a<0)a=v;else b=v;}
                if(a<0 || b<0 || a==b)return false;
                link[a].push_back(b);link[b].push_back(a);
            }
        }
        if(link.empty())continue;
        for(const auto &v:link)if(v.second.size()!=2)return false;
        std::vector<int> queue{link.begin()->first};std::set<int> visited{queue[0]};
        for(std::size_t j=0;j<queue.size();++j)
            for(int v:link.at(queue[j]))if(visited.insert(v).second)queue.push_back(v);
        if(visited.size()!=link.size())return false;
    }
    return true;
}
// Boundary-aware local minimax refinement of a METIS seed partition.
// Only neighboring labels are candidates; no new parts or fine-grid migration.
// Every accepted move keeps the pair maximum nonincreasing and decreases the
// pair squared cost, under a hard global cut-face budget. Other parts unchanged.
inline BalanceResult balance_partition(const std::vector<CoarseCell> &cells,
        std::vector<int> &part,int count,const CostConfig &config,bool optimize=true) {
    if(config.levels<0 || config.refines<0 || config.levels+config.refines>13 ||
       config.sweeps<0 || config.cut_growth<0 || !std::isfinite(config.cut_growth))
        throw std::runtime_error("invalid balancing parameters");
    for(double w:config.weights)
        if(!std::isfinite(w)||w<0) throw std::runtime_error("invalid cost weights");
    if(std::accumulate(config.weights.begin(),config.weights.end(),0.0)<=0)
        throw std::runtime_error("cost weights must not all be zero");
    BalanceResult result;
    result.before=partition_stats(cells,part,count); result.after=result.before;
    for(const auto &p:result.before) if(p.cells==0) throw std::runtime_error("empty METIS partition");
    result.cut_before=cut_faces(cells,part); result.cut_after=result.cut_before;
    if(!optimize) return result;
    std::map<int,std::vector<int>> incidence;
    for(std::size_t i=0;i<cells.size();++i)for(int v:cells[i].vertices) {
        if(v<=0)throw std::runtime_error("missing coarse vertex identity");
        incidence[v].push_back(static_cast<int>(i));
    }
    const double budget=(1.0+config.cut_growth)*result.cut_before;
    std::vector<double> work(count);
    for(int p=0;p<count;++p) work[p]=predicted_cost(result.after[p],config);
    std::vector<int> order(cells.size());std::iota(order.begin(),order.end(),0);
    for(int sweep=0;sweep<config.sweeps;++sweep) {
        std::sort(order.begin(),order.end(),[&](int a,int b){
            return work[part[a]]>work[part[b]] ||
                   (work[part[a]]==work[part[b]] && a<b);
        });
        long long moves=0;
        for(int i:order) {
            const int from=part[i];
            if(result.after[from].cells<=1) continue;
            std::set<int> targets;
            for(int n:cells[i].neighbor) if(n>=0 && part[n]!=from) targets.insert(part[n]);
            double best_gain=0, best_a=0,best_b=0;
            int best_to=-1, best_cut=0;
            PartCost best_from,best_target;
            for(int to:targets) {
                PartCost a=result.after[from],b=result.after[to];
                --a.cells; ++b.cells;
                a.volume-=cells[i].volume; b.volume+=cells[i].volume;
                int cut_delta=0;
                for(int k=0;k<4;++k) {
                    const int n=cells[i].neighbor[k];
                    const int other=n<0?-1:part[n];
                    const double area=cells[i].face_area[k];
                    int da=-1,db=1;
                    if(other==from) {da=1;db=1;++cut_delta;}
                    else if(other==to) {da=-1;db=-1;--cut_delta;}
                    a.faces+=da;b.faces+=db;a.area+=da*area;b.area+=db*area;
                }
                if(result.cut_after+cut_delta>budget || a.area<=0 || b.area<=0 ||
                   a.volume<=0 || b.volume<=0 || a.faces<=0 || b.faces<=0) continue;
                const double wa=predicted_cost(a,config),wb=predicted_cost(b,config);
                const double old_max=std::max(work[from],work[to]);
                if(std::max(wa,wb)>old_max) continue;
                const double old_sq=work[from]*work[from]+work[to]*work[to];
                const double gain=old_sq-wa*wa-wb*wb;
                if(gain>1e-12*old_sq && gain>best_gain) {
                    best_gain=gain;best_to=to;best_cut=cut_delta;
                    best_a=wa;best_b=wb;best_from=a;best_target=b;
                }
            }
            if(best_to<0 || !can_remove(i,cells,part) ||
               !manifold_after_move(i,best_to,cells,part,incidence)) continue;
            part[i]=best_to;result.after[from]=best_from;result.after[best_to]=best_target;
            work[from]=best_a;work[best_to]=best_b;result.cut_after+=best_cut;
            ++moves;++result.accepted_moves;
        }
        if(!moves) break;
    }
    return result;
}
} // namespace mesh_research
