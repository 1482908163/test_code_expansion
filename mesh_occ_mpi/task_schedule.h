#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
namespace mesh_research {
// 未开始的封闭子域才可改派；预算对完整计划（含未开始任务）逐次生效。
class TaskSchedule {
    std::vector<int> home_,owner_,node_;
    std::vector<std::map<int,int>> edges_;
    std::vector<double> weight_;
    std::set<std::pair<double,int>> pending_;
    std::vector<std::vector<int>> local_;
    std::vector<std::size_t> cursor_;
    long long cut_=0,initial_cut_=0,limit_=0;
public:
    TaskSchedule(std::vector<int> home,std::vector<int> nodes,
                 std::vector<std::map<int,int>> edges,std::vector<double> weights,double growth)
        :home_(std::move(home)),owner_(home_),node_(std::move(nodes)),edges_(std::move(edges)),weight_(std::move(weights)),
         local_(node_.size()),cursor_(local_.size(),0) {
        if(node_.size()<2 || std::any_of(node_.begin(),node_.end(),[&](int n){return n<0 || n>=static_cast<int>(node_.size());}) || home_.empty() || edges_.size()!=home_.size() || weight_.size()!=home_.size() || growth<0 || growth>1 || !std::isfinite(growth))
            throw std::runtime_error("invalid task graph");
        for(int h:home_)if(h<=0 || h>=static_cast<int>(node_.size()))throw std::runtime_error("invalid task home");
        for(int t=0;t<static_cast<int>(home_.size());++t) {
            if(home_[t]<=0 || home_[t]>=static_cast<int>(node_.size()) || !std::isfinite(weight_[t]) || weight_[t]<=0)
                throw std::runtime_error("invalid task home/weight");
            pending_.emplace(-weight_[t],t);local_[node_[home_[t]]].push_back(t);
            for(const auto &e:edges_[t]) {
                if(e.first<0 || e.first>=static_cast<int>(home_.size()) || e.first==t || e.second<=0 ||
                   edges_[e.first].count(t)==0 || edges_[e.first].at(t)!=e.second)
                    throw std::runtime_error("invalid task dependency");
                if(e.first>t && node_[home_[t]]!=node_[home_[e.first]])cut_+=e.second;
            }
        }
        for(auto &q:local_)std::stable_sort(q.begin(),q.end(),[&](int a,int b){return weight_[a]>weight_[b];});
        initial_cut_=cut_;limit_=cut_+static_cast<long long>(std::floor(cut_*growth));
    }
    int claim(int rank,bool dynamic) {
        if(rank<=0 || rank>=static_cast<int>(node_.size()))throw std::runtime_error("invalid task worker");
        int best=-1;double score=-1;long long best_delta=0;
        auto consider=[&](int t) {
            if(!pending_.count({-weight_[t],t}) || (!dynamic && home_[t]!=rank))return;
            long long delta=0;int affinity=0,total=0;
            for(const auto &e:edges_[t]) {
                const bool old=node_[owner_[t]]!=node_[owner_[e.first]],next=node_[rank]!=node_[owner_[e.first]];
                delta+=(static_cast<int>(next)-static_cast<int>(old))*e.second;
                total+=e.second;if(!next)affinity+=e.second;
            }
            if(cut_+delta>limit_)return;
            const double value=weight_[t]*(1.0+0.25*affinity/std::max(1,total));
            if(value>score || (value==score && t<best)){best=t;score=value;best_delta=delta;}
        };
        if(dynamic) {
            int checked=0;for(const auto &entry:pending_){consider(entry.second);if(++checked==64)break;}
            auto &q=local_[node_[rank]];auto &pos=cursor_[node_[rank]];
            while(pos<q.size() && !pending_.count({-weight_[q[pos]],q[pos]}))++pos;
            // 同节点的首个未开始任务总有零切分增量，保证严格预算下仍可完成。
            if(pos<q.size())consider(q[pos]);
        } else {
            for(const auto &entry:pending_)if(home_[entry.second]==rank){consider(entry.second);break;}
        }
        if(best>=0){owner_[best]=rank;cut_+=best_delta;pending_.erase({-weight_[best],best});}
        return best;
    }
    const std::vector<int> &owners() const {return owner_;}
    bool empty() const {return pending_.empty();}
    long long cut() const {return cut_;}
    long long initial_cut() const {return initial_cut_;}
    long long limit() const {return limit_;}
};
}
