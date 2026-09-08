#pragma once
#include <cstdint>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

namespace mesh_research {
// 固定整数运算定义重排，避免依赖标准库 shuffle 的实现差异。
inline std::vector<int> sampling_cell_order(int cells,int seed,bool permute) {
    if(cells<=0 || seed< -1) throw std::runtime_error("invalid partition sampling input");
    std::vector<int> order(cells);std::iota(order.begin(),order.end(),0);
    if(!permute || seed<0) return order;
    std::uint64_t state=static_cast<std::uint32_t>(seed);
    auto next=[&]() {
        std::uint64_t z=(state+=UINT64_C(0x9e3779b97f4a7c15));
        z=(z^(z>>30))*UINT64_C(0xbf58476d1ce4e5b9);
        z=(z^(z>>27))*UINT64_C(0x94d049bb133111eb);
        return z^(z>>31);
    };
    for(int i=cells-1;i>0;--i) {
        const auto bound=static_cast<std::uint64_t>(i)+1;
        const auto threshold=(std::uint64_t(0)-bound)%bound;
        std::uint64_t value;do {value=next();} while(value<threshold);
        std::swap(order[i],order[value%bound]);
    }
    return order;
}

// 以每个分区第一次出现的粗单元规范化编号，标签置换不算新分区。
inline std::vector<int> canonical_partition(const std::vector<int> &labels,int parts) {
    if(parts<=0 || labels.size()<static_cast<std::size_t>(parts))
        throw std::runtime_error("invalid partition size");
    std::vector<int> mapping(parts,-1),result;result.reserve(labels.size());int next=0;
    for(int p:labels) {
        if(p<0 || p>=parts) throw std::runtime_error("partition label out of range");
        if(mapping[p]<0) mapping[p]=next++;
        result.push_back(mapping[p]);
    }
    if(next!=parts) throw std::runtime_error("empty calibration partition");
    return result;
}

inline int physical_partition_rank(int logical,int parts,int shift) {
    if(parts<=0 || logical<0 || logical>=parts || shift<0 || shift>=parts)
        throw std::runtime_error("invalid partition placement");
    return static_cast<int>((static_cast<long long>(logical)+shift)%parts);
}

inline std::vector<int> read_partition_reference(const std::string &path,int parts,
                                                int cells,int seed,const std::string &variant) {
    std::ifstream in(path);std::string magic,method,extra;int p=0,n=0,s=0;
    if(!(in>>magic>>p>>n>>s>>method) || magic!="mesh_partition_v1" || p!=parts ||
       n!=cells || s!=seed || method!=variant)
        throw std::runtime_error("partition reference identity mismatch");
    std::vector<int> labels(cells);
    for(int &v:labels) if(!(in>>v)) throw std::runtime_error("incomplete partition reference");
    if(in>>extra) throw std::runtime_error("unexpected partition reference content");
    canonical_partition(labels,parts);
    return labels;
}
}
