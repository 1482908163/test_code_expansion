#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "mpi.h"

namespace scaling {
struct ProfileConfig {
    bool enabled = false;
    bool core_only = false;
    bool split_collectives = true;
    std::string output_root;
    std::string experiment = "mesh_algorithms";
    int repeat = 1;
};

// Deliberately limited to the two research questions. No cache, /proc, I/O
// counters, peer graph dumps, or per-rank output files.
class Profiler {
public:
    static Profiler &instance();
    void configure(MPI_Comm comm, const ProfileConfig &config);
    bool enabled() const { return config_.enabled && active_; }
    bool core_only() const { return config_.core_only; }
    bool split_collectives() const { return enabled() && config_.split_collectives; }
    void begin_core();
    void set_total_elapsed(double seconds);
    void add_metadata(const std::string &key, const std::string &value);
    void set_metric(const std::string &key, double value);
    void add_metric(const std::string &key, double value);
    void add_communication(const std::string &stage, std::uint64_t sends,
                           std::uint64_t receives, std::uint64_t sent,
                           std::uint64_t received);
    void finalize();
private:
    friend class StageScope;
    struct Stage {
        double seconds = 0;
        std::uint64_t calls = 0, sends = 0, receives = 0, sent = 0, received = 0;
        std::string category;
    };
    MPI_Comm comm_ = MPI_COMM_NULL;
    ProfileConfig config_;
    bool active_ = false;
    std::map<std::string, Stage> stages_;
    std::map<std::string, double> metrics_;
    std::map<std::string, std::string> metadata_;
};
class StageScope {
public:
    StageScope(const std::string &stage, const std::string &category);
    ~StageScope();
    StageScope(const StageScope &) = delete;
    StageScope &operator=(const StageScope &) = delete;
private:
    std::string stage_;
    double start_ = 0;
    bool active_ = false;
};
} // namespace scaling
