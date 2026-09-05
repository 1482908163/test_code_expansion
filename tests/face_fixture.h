#pragma once
#include "face_dependency.h"
namespace mesh_research {
struct FaceFixture {std::vector<FaceRecord> local;std::map<int,FaceRecord> expected;};
inline FaceFixture face_fixture(int rank,int size,int pass) {
    FaceFixture result;
        std::vector<std::array<int,4>> tets;
        for(int group=0;group<32;++group) {
            const int b=group*20;
            for(auto t:std::vector<std::array<int,4>>{{1,2,3,4},{1,3,2,5},{1,2,6,7},{1,8,9,10},{11,12,13,14}}) {
                for(int &v:t) v+=b;
                tets.push_back(t);
            }
        }
        const int side[4][3]={{1,2,3},{0,3,2},{0,1,3},{0,2,1}};
        std::map<std::array<int,3>,int> face_ids;
        std::vector<FaceRecord> full;
        for(std::size_t e=0;e<tets.size();++e) for(int k=0;k<4;++k) {
            FaceRecord f{};f[1]=static_cast<int>(e)+1;f[2]=static_cast<int>(e)%size;f[3]=-1;
            std::array<int,3> key;
            for(int j=0;j<3;++j){f[4+j]=tets[e][side[k][j]];key[j]=f[4+j];}
            std::sort(key.begin(),key.end());
            const auto at=face_ids.emplace(key,static_cast<int>(face_ids.size())+1);
            f[0]=at.first->second;f[10]=static_cast<int>(e)%3+1;f[12]=static_cast<int>(e)%2;
            full.push_back(f);
            // Pass 0: intentionally one sender. Other passes: arbitrary input
            // ownership distinct from final partition ownership, including idle ranks.
            const int source=pass==0?0:static_cast<int>((e*7+3)%size);
            if(source==rank)result.local.push_back(f);
        }
        // Independent reproduction of baseline merge in global element order.
        std::map<int,FaceRecord> global;
        for(const auto &f:full) {
            auto it=global.find(f[0]);
            if(it==global.end())global.emplace(f[0],f);
            else if(it->second[2]==f[2])global.erase(it);
            else {
                auto &a=it->second;a[3]=f[2];a[11]=f[10];
                for(int k=0;k<3;++k)a[7+k]=f[4+k];
            }
        }
        std::set<int> local_vertices;
        for(const auto &kv:global) {
            const auto &f=kv.second;
            if(f[2]==rank || f[3]==rank)for(int k=4;k<7;++k)local_vertices.insert(f[k]);
        }
        for(const auto &kv:global)for(int k=4;k<7;++k)
            if(local_vertices.count(kv.second[k])){result.expected.insert(kv);break;}
    return result;
}
}
