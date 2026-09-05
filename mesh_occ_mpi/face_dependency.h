#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
namespace mesh_research {
// All wire fields are initialized MPI_INTs: face, source element, two owners,
// two oriented triangles, two domain indices, first orientation, directory key.
using FaceRecord = std::array<int, 14>;
using FacePackets = std::map<int, std::vector<FaceRecord>>;
inline int directory_owner(int key, int size) {
    // Integer mixing avoids all nearby mesh entities hitting one directory rank.
    std::uint32_t x = static_cast<std::uint32_t>(key);
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15;
    x *= 0x846ca68bU; x ^= x >> 16;
    return static_cast<int>(x % static_cast<std::uint32_t>(size));
}
inline std::vector<FaceRecord> match_faces(std::vector<FaceRecord> records) {
    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
        return a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]);
    });
    std::vector<FaceRecord> boundary;
    for (std::size_t i = 0; i < records.size();) {
        std::size_t end = i + 1;
        while (end < records.size() && records[end][0] == records[i][0]) ++end;
        if (end - i > 2) throw std::runtime_error("non-manifold coarse face");
        FaceRecord f = records[i];
        if (end - i == 2) {
            const auto &other = records[i + 1];
            if (other[1] == f[1]) throw std::runtime_error("duplicate element-face record");
            auto a = std::array<int,3>{f[4],f[5],f[6]};
            auto b = std::array<int,3>{other[4],other[5],other[6]};
            std::sort(a.begin(),a.end()); std::sort(b.begin(),b.end());
            if (a != b) throw std::runtime_error("inconsistent face identifiers");
            if (f[2] == other[2]) { i = end; continue; }
            f[3] = other[2];
            std::copy_n(other.begin() + 4, 3, f.begin() + 7);
            f[11] = other[10];
        }
        boundary.push_back(f); i = end;
    }
    return boundary;
}


inline FacePackets face_directory_packets(const std::vector<FaceRecord> &local,int size) {
    FacePackets result;
    for(const auto &f:local)result[directory_owner(f[0],size)].push_back(f);
    return result;
}
inline FacePackets vertex_directory_packets(const std::vector<FaceRecord> &boundary,int size) {
    FacePackets result;
    for(const auto &f:boundary)for(int k=4;k<7;++k) {
        auto incidence=f;incidence[13]=f[k];
        result[directory_owner(f[k],size)].push_back(incidence);
    }
    return result;
}
inline FacePackets consumer_packets(std::vector<FaceRecord> star) {
    std::sort(star.begin(),star.end(),[](const auto&a,const auto&b){
        return a[13]<b[13] || (a[13]==b[13] && a[0]<b[0]);
    });
    FacePackets result;
    for(std::size_t i=0;i<star.size();) {
        std::size_t end=i;std::set<int> consumers;
        while(end<star.size() && star[end][13]==star[i][13]) {
            consumers.insert(star[end][2]);
            if(star[end][3]>=0)consumers.insert(star[end][3]);
            ++end;
        }
        for(std::size_t j=i;j<end;++j) {
            auto f=star[j];f[13]=0;
            for(int owner:consumers)result[owner].push_back(f);
        }
        i=end;
    }
    for(auto &entry:result) {
        auto &v=entry.second;
        std::sort(v.begin(),v.end());v.erase(std::unique(v.begin(),v.end()),v.end());
    }
    return result;
}
inline std::map<int,FaceRecord> merge_delivery(const std::vector<FaceRecord> &delivered) {
    std::map<int,FaceRecord> result;
    for(const auto &f:delivered) {
        auto it=result.emplace(f[0],f);
        if(!it.second && it.first->second!=f)
            throw std::runtime_error("conflicting matched face records");
    }
    return result;
}
} // namespace mesh_research
