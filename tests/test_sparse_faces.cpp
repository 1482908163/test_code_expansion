#include "sparse_face_exchange.h"
#include "face_fixture.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
using namespace mesh_research;
int main(int argc,char **argv) {
    MPI_Init(&argc,&argv);
    int rank,size;MPI_Comm_rank(MPI_COMM_WORLD,&rank);MPI_Comm_size(MPI_COMM_WORLD,&size);
    scaling::ProfileConfig cfg;cfg.enabled=true;cfg.split_collectives=argc>2;
    cfg.core_only=true;cfg.output_root=argc>1?argv[1]:"/tmp/face_test";
    auto &profile=scaling::Profiler::instance();profile.configure(MPI_COMM_WORLD,cfg);
    profile.add_metadata("algorithm","sparse");profile.add_metadata("timing_mode",cfg.split_collectives?"split":"natural");
    profile.add_metadata("core_only","true");
    profile.begin_core();double begin=MPI_Wtime();
    for(int pass=0;pass<3;++pass) {
        const auto fixture=face_fixture(rank,size,pass);
        if(rank==0 && pass==1)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto actual=exchange_face_closure(fixture.local,MPI_COMM_WORLD,pass==2?4096:3);
        if(actual!=fixture.expected)protocol_error(MPI_COMM_WORLD,"reference closure mismatch in MPI test");
        // This also exercises a completely empty exchange on every rank.
        const auto empty=exchange_face_closure({},MPI_COMM_WORLD,3);
        assert(empty.empty());
    }
    profile.set_total_elapsed(MPI_Wtime()-begin);
    profile.finalize();
    if(rank==0)std::cout<<"PASS: "<<size<<" MPI ranks; face/edge/vertex-only adjacency, internal faces, disconnected domains, empty inputs, skewed arrivals, packet chunking\n";
    MPI_Finalize();
}
