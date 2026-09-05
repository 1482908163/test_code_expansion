#include "face_fixture.h"
#include <cassert>
#include <iostream>
using namespace mesh_research;
static std::vector<std::vector<FaceRecord>> route(const std::vector<FacePackets> &packets) {
    std::vector<std::vector<FaceRecord>> incoming(packets.size());
    for(const auto &sender:packets)for(const auto &peer:sender) {
        assert(peer.first>=0 && peer.first<static_cast<int>(incoming.size()));
        incoming[peer.first].insert(incoming[peer.first].end(),peer.second.begin(),peer.second.end());
    }
    return incoming;
}
int main() {
    for(int ranks:{1,2,3,7,16,256})for(int pass=0;pass<3;++pass) {
        std::vector<FaceFixture> fixture;
        std::vector<FacePackets> packets(ranks);
        for(int r=0;r<ranks;++r) {
            fixture.push_back(face_fixture(r,ranks,pass));
            packets[r]=face_directory_packets(fixture[r].local,ranks);
        }
        auto received=route(packets);
        for(int r=0;r<ranks;++r)packets[r]=vertex_directory_packets(match_faces(received[r]),ranks);
        received=route(packets);
        for(int r=0;r<ranks;++r)packets[r]=consumer_packets(received[r]);
        received=route(packets);
        for(int r=0;r<ranks;++r)assert(merge_delivery(received[r])==fixture[r].expected);
    }
    FaceRecord bad{};bad[0]=1;bad[1]=1;bad[2]=0;bad[3]=-1;
    bool rejected=false;
    try {match_faces({bad,bad,bad});}catch(const std::runtime_error&){rejected=true;}
    assert(rejected);
    std::cout<<"PASS: 18 distributed-data simulations, up to 256 virtual ranks, exact reference closure, empty owners, non-manifold rejection\n";
}
