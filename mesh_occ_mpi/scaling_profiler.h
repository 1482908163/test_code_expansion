#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "mpi.h"

namespace scaling {

struct ProfileConfig {
    bool enabled = false;
    bool collect_hardware_cache = false;
    bool core_only = false;
    std::string output_root;
    std::string experiment = "strong_scaling";
    int repeat = 1;
};

struct ProcessSnapshot {
    std::uint64_t rchar = 0;
    std::uint64_t wchar = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t minor_faults = 0;
    std::uint64_t major_faults = 0;
    std::uint64_t voluntary_context_switches = 0;
    std::uint64_t involuntary_context_switches = 0;
};

struct PerfValues {
    double cycles = 0.0;
    double instructions = 0.0;
    double cache_references = 0.0;
    double cache_misses = 0.0;
    bool valid = false;
};

class PerfCounterSet;

class Profiler {
public:
    static Profiler &instance();

    Profiler(const Profiler &) = delete;
    Profiler &operator=(const Profiler &) = delete;

    void configure(MPI_Comm comm, const ProfileConfig &config);
    bool enabled() const;
    bool core_only() const;

    void add_metadata(const std::string &key, const std::string &value);
    void set_metric(const std::string &key, double value);
    void add_metric(const std::string &key, double value);

    void add_communication(const std::string &stage,
                           std::uint64_t send_messages,
                           std::uint64_t receive_messages,
                           std::uint64_t send_bytes,
                           std::uint64_t receive_bytes);

    void set_total_elapsed(double seconds);
    void finalize();

private:
    friend class StageScope;

    struct StageLocalStats {
        double wall_seconds = 0.0;
        double cycles = 0.0;
        double instructions = 0.0;
        double cache_references = 0.0;
        double cache_misses = 0.0;
        std::uint64_t calls = 0;
        std::uint64_t perf_samples = 0;
        std::uint64_t rchar = 0;
        std::uint64_t wchar = 0;
        std::uint64_t read_bytes = 0;
        std::uint64_t write_bytes = 0;
        std::uint64_t minor_faults = 0;
        std::uint64_t major_faults = 0;
        std::uint64_t voluntary_context_switches = 0;
        std::uint64_t involuntary_context_switches = 0;
        std::uint64_t send_messages = 0;
        std::uint64_t receive_messages = 0;
        std::uint64_t send_bytes = 0;
        std::uint64_t receive_bytes = 0;
    };

    Profiler();
    ~Profiler();

    ProcessSnapshot process_snapshot(bool include_io_counters) const;
    bool start_perf_counters();
    PerfValues stop_perf_counters();
    void record_stage(const std::string &stage,
                      const std::string &category,
                      double elapsed,
                      const ProcessSnapshot &before,
                      const ProcessSnapshot &after,
                      const PerfValues &perf);

    MPI_Comm comm_ = MPI_COMM_NULL;
    ProfileConfig config_;
    int rank_ = 0;
    int process_count_ = 1;
    bool configured_ = false;
    bool perf_active_ = false;
    std::string cache_error_;
    std::unique_ptr<PerfCounterSet> perf_counters_;
    std::map<std::string, StageLocalStats> stages_;
    std::map<std::string, std::string> stage_categories_;
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
    std::string category_;
    double start_time_ = 0.0;
    ProcessSnapshot before_;
    bool active_ = false;
    bool perf_started_ = false;
    bool collect_io_counters_ = false;
};

} // namespace scaling
