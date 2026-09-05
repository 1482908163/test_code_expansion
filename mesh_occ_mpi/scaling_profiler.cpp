#include "scaling_profiler.h"
#include <climits>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace scaling {
namespace {
std::string quoted(const std::string &s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4)
                             << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    return out.str() + '"';
}
}
Profiler &Profiler::instance() { static Profiler p; return p; }
void Profiler::configure(MPI_Comm comm, const ProfileConfig &config) {
    comm_ = comm; config_ = config; active_ = false;
    stages_.clear(); metrics_.clear(); metadata_.clear();
}
void Profiler::begin_core() { stages_.clear(); active_ = true; }
void Profiler::set_total_elapsed(double seconds) {
    set_metric("core_seconds", seconds); active_ = false;
}
void Profiler::add_metadata(const std::string &key, const std::string &value) {
    if (config_.enabled) metadata_[key] = value;
}
void Profiler::set_metric(const std::string &key, double value) {
    if (config_.enabled && std::isfinite(value)) metrics_[key] = value;
}
void Profiler::add_metric(const std::string &key, double value) {
    if (enabled() && std::isfinite(value)) metrics_[key] += value;
}
void Profiler::add_communication(const std::string &name, std::uint64_t sends,
                                std::uint64_t receives, std::uint64_t sent,
                                std::uint64_t received) {
    if (!enabled()) return;
    auto &s = stages_[name]; s.sends += sends; s.receives += receives;
    s.sent += sent; s.received += received;
}
StageScope::StageScope(const std::string &name, const std::string &category)
    : stage_(name) {
    auto &p = Profiler::instance();
    active_ = p.enabled() && category != "io" && category != "postprocess";
    if (active_) { p.stages_[name].category = category; start_ = MPI_Wtime(); }
}
StageScope::~StageScope() {
    if (!active_) return;
    auto &s = Profiler::instance().stages_[stage_];
    s.seconds += MPI_Wtime() - start_; ++s.calls;
}
void Profiler::finalize() {
    if (!config_.enabled) return;
    active_ = false;
    int rank, size;
    MPI_Comm_rank(comm_, &rank); MPI_Comm_size(comm_, &size);
    std::ostringstream row;
    row << std::setprecision(17) << "{\"rank\":" << rank << ",\"ranks\":" << size
        << ",\"repeat\":" << config_.repeat << ",\"experiment\":"
        << quoted(config_.experiment) << ",\"metadata\":{";
    bool first = true;
    for (const auto &m : metadata_) {
        if (!first) row << ',';
        first = false; row << quoted(m.first) << ':' << quoted(m.second);
    }
    row << "},\"metrics\":{"; first = true;
    for (const auto &m : metrics_) {
        if (!first) row << ',';
        first = false; row << quoted(m.first) << ':' << m.second;
    }
    row << "},\"stages\":{"; first = true;
    for (const auto &entry : stages_) {
        if (!first) row << ',';
        first = false;
        const auto &s = entry.second;
        row << quoted(entry.first) << ":{\"category\":" << quoted(s.category)
            << ",\"seconds\":" << s.seconds << ",\"calls\":" << s.calls
            << ",\"send_messages\":" << s.sends << ",\"receive_messages\":" << s.receives
            << ",\"send_bytes\":" << s.sent << ",\"receive_bytes\":" << s.received << '}';
    }
    row << "}}\n";
    const std::string local = row.str();
    if (local.size() > INT_MAX) MPI_Abort(comm_, MPI_ERR_COUNT);
    const int length = static_cast<int>(local.size());
    std::vector<int> lengths(size), offsets(size);
    // Reporting collectives are outside core_seconds and all stage timers.
    MPI_Gather(&length, 1, MPI_INT, lengths.data(), 1, MPI_INT, 0, comm_);
    std::vector<char> all;
    if (rank == 0) {
        long long total = 0;
        for (int i = 0; i < size; ++i) {
            offsets[i] = static_cast<int>(total); total += lengths[i];
            if (total > INT_MAX) MPI_Abort(comm_, MPI_ERR_COUNT);
        }
        all.resize(static_cast<std::size_t>(total));
    }
    MPI_Gatherv(local.data(), length, MPI_CHAR, all.data(), lengths.data(),
                offsets.data(), MPI_CHAR, 0, comm_);
    if (rank == 0) {
        try {
            std::filesystem::create_directories(config_.output_root);
            const auto path = std::filesystem::path(config_.output_root) / "rank_profiles.jsonl";
            std::ofstream out(path);
            out.write(all.data(), static_cast<std::streamsize>(all.size()));
            out.close();
            if (!out) throw std::runtime_error("cannot write rank_profiles.jsonl");
        } catch (const std::exception &e) {
            std::fprintf(stderr, "profile output failed: %s\n", e.what());
            MPI_Abort(comm_, MPI_ERR_OTHER);
        }
    }
}
} // namespace scaling
