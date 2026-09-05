#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#include "mpi.h"
#include "scaling_profiler.h"
#include "face_dependency.h"

namespace mesh_research {
inline void check_mpi(int rc, MPI_Comm comm) {
    if (rc != MPI_SUCCESS) MPI_Abort(comm, rc);
}
inline void protocol_error(MPI_Comm comm, const char *message) {
    int rank; MPI_Comm_rank(comm, &rank);
    std::fprintf(stderr, "sparse face protocol rank %d: %s\n", rank, message);
    MPI_Abort(comm, MPI_ERR_OTHER);
    throw std::runtime_error(message);
}

// Hoefler/Siebert/Lumsdaine NBX termination: synchronous sends + Ibarrier.
// No dense peer counts. Keep probing until the nonblocking barrier completes.
// The communicator is dedicated to this protocol, and rounds use distinct tags.
inline std::vector<FaceRecord> sparse_exchange(const FacePackets &outgoing,
        MPI_Comm comm, int tag, const std::string &stage, std::size_t chunk = 4096) {
    static_assert(sizeof(FaceRecord) == 14 * sizeof(int), "wire record has padding");
    if (chunk == 0 || chunk > INT_MAX / 14) protocol_error(comm, "invalid packet size");
    int rank, size;
    MPI_Comm_rank(comm, &rank); MPI_Comm_size(comm, &size);
    auto &profile = scaling::Profiler::instance();
    if (profile.split_collectives()) {
        scaling::StageScope wait(stage + "_pre_collective_wait", "synchronization");
        check_mpi(MPI_Barrier(comm), comm);
    }
    scaling::StageScope elapsed(stage, "communication");
    std::vector<MPI_Request> requests;
    std::vector<FaceRecord> incoming;
    std::uint64_t sent = 0, received = 0, sends = 0, receives = 0;
    for (const auto &peer : outgoing) {
        if (peer.first < 0 || peer.first >= size) protocol_error(comm, "invalid destination");
        if (peer.first == rank) {
            incoming.insert(incoming.end(), peer.second.begin(), peer.second.end());
            continue;
        }
        for (std::size_t begin = 0; begin < peer.second.size(); begin += chunk) {
            const std::size_t count = std::min(chunk, peer.second.size() - begin);
            MPI_Request request;
            check_mpi(MPI_Issend(peer.second[begin].data(), static_cast<int>(14 * count),
                                MPI_INT, peer.first, tag, comm, &request), comm);
            requests.push_back(request); sent += count * sizeof(FaceRecord); ++sends;
        }
    }
    if (requests.size() > INT_MAX) protocol_error(comm, "too many MPI requests");
    MPI_Request barrier = MPI_REQUEST_NULL;
    bool barrier_started = false;
    int done = 0;
    while (!done) {
        int found = 0;
        MPI_Status status;
        check_mpi(MPI_Iprobe(MPI_ANY_SOURCE, tag, comm, &found, &status), comm);
        if (found) {
            int words;
            check_mpi(MPI_Get_count(&status, MPI_INT, &words), comm);
            if (words <= 0 || words % 14) protocol_error(comm, "malformed face packet");
            const std::size_t before = incoming.size();
            incoming.resize(before + static_cast<std::size_t>(words / 14));
            check_mpi(MPI_Recv(incoming[before].data(), words, MPI_INT,
                              status.MPI_SOURCE, tag, comm, MPI_STATUS_IGNORE), comm);
            received += static_cast<std::uint64_t>(words) * sizeof(int); ++receives;
        }
        if (!barrier_started) {
            int sent_all = requests.empty();
            if (!sent_all) check_mpi(MPI_Testall(static_cast<int>(requests.size()),
                        requests.data(), &sent_all, MPI_STATUSES_IGNORE), comm);
            if (sent_all) {
                check_mpi(MPI_Ibarrier(comm, &barrier), comm);
                barrier_started = true;
            }
        }
        if (barrier_started) check_mpi(MPI_Test(&barrier, &done, MPI_STATUS_IGNORE), comm);
    }
    profile.add_communication(stage, sends, receives, sent, received);
    profile.add_metric("sparse_termination_collectives", 1);
    return incoming;
}

// Return every boundary face intersecting this rank's boundary vertex set.
// This is a conservative closure: PartFaceCreate uses owned faces, while
// computeadj also needs remote interfaces incident to a local edge or vertex.
inline std::map<int, FaceRecord> exchange_face_closure(
        const std::vector<FaceRecord> &local, MPI_Comm comm, std::size_t chunk = 4096) {
    int size; MPI_Comm_size(comm, &size);
    try {
        auto by_face=face_directory_packets(local,size);
        auto raw=sparse_exchange(by_face,comm,101,"face_sparse_match",chunk);
        by_face.clear();
        auto boundary=match_faces(std::move(raw));
        auto by_vertex=vertex_directory_packets(boundary,size);
        boundary.clear();
        auto star=sparse_exchange(by_vertex,comm,102,"face_sparse_directory",chunk);
        by_vertex.clear();
        auto to_consumer=consumer_packets(std::move(star));
        auto delivered=sparse_exchange(to_consumer,comm,103,"face_sparse_deliver",chunk);
        return merge_delivery(delivered);
    } catch(const std::exception &e) {
        protocol_error(comm,e.what());
    }
    return {};
}
} // namespace mesh_research
