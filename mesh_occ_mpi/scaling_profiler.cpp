#include "scaling_profiler.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <ctime>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

namespace scaling {
namespace {

struct StageDefinition {
    const char *name;
    const char *category;
};

const std::vector<StageDefinition> &stage_definitions()
{
    static const std::vector<StageDefinition> definitions = {
        {"geometry_load", "io"},
        {"coarse_local_size", "setup"},
        {"coarse_edge_mesh", "setup"},
        {"coarse_surface_mesh", "setup"},
        {"coarse_volume_mesh", "setup"},
        {"coarse_mesh_save", "io"},
        {"submesh_initialization", "setup"},
        {"metis_partition", "compute"},
        {"face_map_setup", "compute"},
        {"face_local_extract", "compute"},
        {"face_pre_collective_wait", "synchronization"},
        {"face_collective_setup", "compute"},
        {"face_allgatherv", "communication"},
        {"face_global_merge", "compute"},
        {"part_face_create", "compute"},
        {"surface_refine", "compute"},
        {"refined_surface_save", "io"},
        {"local_volume_mesh", "compute"},
        {"volume_refine", "compute"},
        {"refined_volume_save", "io"},
        {"adjacency_build", "compute"},
        {"vertex_numbering_local", "compute"},
        {"vertex_count_pre_collective_wait", "synchronization"},
        {"vertex_count_allgather", "communication"},
        {"element_count_pre_collective_wait", "synchronization"},
        {"element_count_allgather", "communication"},
        {"vertex_exchange_prepare", "compute"},
        {"vertex_exchange", "communication"},
        {"vertex_exchange_unpack", "compute"},
        {"partition_result_io", "io"},
        {"volume_exchange_pack", "compute"},
        {"volume_size_exchange_pre_collective_wait", "synchronization"},
        {"volume_size_exchange", "communication"},
        {"volume_payload_prepare", "compute"},
        {"volume_payload_exchange", "communication"},
        {"volume_exchange_unpack", "compute"},
        {"final_mesh_save", "io"},
        {"testout_io", "io"},
        {"final_count_exchange", "communication"},
        {"quality_surface_compute", "postprocess"},
        {"quality_reduce_pre_collective_wait", "synchronization"},
        {"quality_reduce", "communication"},
        {"quality_summary_io", "io"},
        {"quality_volume_compute", "postprocess"},
        {"quality_rank_io", "io"},
    };
    return definitions;
}

const std::vector<std::string> &metric_definitions()
{
    static const std::vector<std::string> definitions = {
        "total_wall_seconds",
        "coarse_points",
        "coarse_surface_elements",
        "coarse_volume_elements",
        "facemap_entries",
        "physical_boundary_faces",
        "partition_boundary_faces",
        "local_points_before_adjacency",
        "local_surface_elements_before_adjacency",
        "local_volume_elements_before_adjacency",
        "shared_vertices",
        "adjacent_processes",
        "vertex_send_items",
        "vertex_receive_items",
        "vertex_num_s",
        "vertex_num_r",
        "vertex_waitall_seconds",
        "volume_send_items",
        "volume_receive_items",
        "volume_num_s",
        "volume_num_r",
        "volume_waitall_seconds",
        "local_points_after_adjacency",
        "local_surface_elements_after_adjacency",
        "local_volume_elements_after_adjacency",
        "peak_rss_mib",
        "hardware_cache_available",
        "process_io_available",
    };
    return definitions;
}

const std::vector<std::string> &peer_exchange_definitions()
{
    static const std::vector<std::string> definitions = {
        "vertex_exchange",
        "volume_payload_exchange",
    };
    return definitions;
}

std::uint64_t positive_delta(std::uint64_t after, std::uint64_t before)
{
    return after >= before ? after - before : 0;
}

std::string trim_colon(std::string key)
{
    if (!key.empty() && key.back() == ':') {
        key.pop_back();
    }
    return key;
}

std::string sanitize_component(const std::string &input)
{
    std::string result;
    result.reserve(input.size());
    for (unsigned char value : input) {
        if ((value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_') {
            result.push_back(static_cast<char>(value));
        } else {
            result.push_back('_');
        }
    }
    return result.empty() ? "strong_scaling" : result;
}

std::string current_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &value);
#else
    localtime_r(&value, &local_time);
#endif
    std::ostringstream output;
    output << std::put_time(&local_time, "%Y%m%d-%H%M%S");
    return output.str();
}

std::string csv_escape(const std::string &value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (char character : value) {
        if (character == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(character);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string category_label(const std::string &category)
{
    if (category == "setup") return "setup(初始化)";
    if (category == "compute") return "compute(计算)";
    if (category == "communication") return "communication(通信)";
    if (category == "synchronization") return "synchronization(同步等待)";
    if (category == "io") return "io(输入输出)";
    if (category == "postprocess") return "postprocess(后处理)";
    if (category == "unprofiled") return "unprofiled(未细分)";
    return category;
}

struct WireStage {
    double wall_seconds = 0.0;
    double calls = 0.0;
    double cycles = 0.0;
    double instructions = 0.0;
    double cache_references = 0.0;
    double cache_misses = 0.0;
    double perf_samples = 0.0;
    double rchar = 0.0;
    double wchar = 0.0;
    double read_bytes = 0.0;
    double write_bytes = 0.0;
    double minor_faults = 0.0;
    double major_faults = 0.0;
    double voluntary_context_switches = 0.0;
    double involuntary_context_switches = 0.0;
    double send_messages = 0.0;
    double receive_messages = 0.0;
    double send_bytes = 0.0;
    double receive_bytes = 0.0;
};

struct ScalarStats {
    double minimum = 0.0;
    double average = 0.0;
    double maximum = 0.0;
};

ScalarStats summarize(const std::vector<double> &values)
{
    if (values.empty()) return {};
    ScalarStats result;
    result.minimum = *std::min_element(values.begin(), values.end());
    result.maximum = *std::max_element(values.begin(), values.end());
    result.average = std::accumulate(values.begin(), values.end(), 0.0) /
                     static_cast<double>(values.size());
    return result;
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    if (values.size() == 1) return values.front();
    const double position = fraction * static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double weight = position - static_cast<double>(lower);
    return values[lower] * (1.0 - weight) + values[upper] * weight;
}

std::string join_integers(const std::vector<int> &values)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ';';
        output << values[index];
    }
    return output.str();
}

std::string join_peer_items(const std::vector<int> &peers,
                            const std::vector<std::uint64_t> &items)
{
    std::ostringstream output;
    const std::size_t count = std::min(peers.size(), items.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) output << ';';
        output << peers[index] << ':' << items[index];
    }
    return output.str();
}

double safe_ratio(double numerator, double denominator)
{
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

double bytes_to_mib(double bytes)
{
    return bytes / (1024.0 * 1024.0);
}

std::ofstream open_output_file(const std::filesystem::path &path)
{
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("cannot open profiler output: " + path.string());
    }
    output.exceptions(std::ios::badbit | std::ios::failbit);
    return output;
}

} // namespace

class PerfCounterSet {
public:
    explicit PerfCounterSet(bool requested)
    {
        if (!requested) return;
#ifdef __linux__
        struct EventConfig {
            const char *name;
            std::uint64_t config;
        };
        const EventConfig configs[] = {
            {"cycles", PERF_COUNT_HW_CPU_CYCLES},
            {"instructions", PERF_COUNT_HW_INSTRUCTIONS},
            {"cache_references", PERF_COUNT_HW_CACHE_REFERENCES},
            {"cache_misses", PERF_COUNT_HW_CACHE_MISSES},
        };

        for (std::size_t index = 0; index < std::size(configs); ++index) {
            perf_event_attr attributes{};
            attributes.size = sizeof(attributes);
            attributes.type = PERF_TYPE_HARDWARE;
            attributes.config = configs[index].config;
            attributes.disabled = index == 0 ? 1 : 0;
            attributes.exclude_kernel = 1;
            attributes.exclude_hv = 1;
            attributes.inherit = 1;
            attributes.read_format = PERF_FORMAT_GROUP |
                                     PERF_FORMAT_ID |
                                     PERF_FORMAT_TOTAL_TIME_ENABLED |
                                     PERF_FORMAT_TOTAL_TIME_RUNNING;

            const int group_fd = index == 0 ? -1 : leader_fd_;
            const int fd = static_cast<int>(syscall(__NR_perf_event_open,
                                                     &attributes,
                                                     0,
                                                     -1,
                                                     group_fd,
                                                     0));
            if (fd < 0) {
                error_ = std::string("perf_event_open(") + configs[index].name +
                         ") failed: " + std::strerror(errno);
                close_all();
                return;
            }
            if (index == 0) leader_fd_ = fd;

            std::uint64_t id = 0;
            if (ioctl(fd, PERF_EVENT_IOC_ID, &id) != 0) {
                error_ = std::string("PERF_EVENT_IOC_ID failed: ") + std::strerror(errno);
                ::close(fd);
                close_all();
                return;
            }
            fds_.push_back(fd);
            ids_.push_back(id);
        }
        available_ = leader_fd_ >= 0 && fds_.size() == 4;
#else
        error_ = "hardware cache counters require Linux perf_event_open";
#endif
    }

    ~PerfCounterSet()
    {
        close_all();
    }

    bool available() const { return available_; }
    const std::string &error() const { return error_; }

    bool start()
    {
#ifdef __linux__
        if (!available_) return false;
        if (ioctl(leader_fd_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0) {
            error_ = std::string("PERF_EVENT_IOC_RESET failed: ") + std::strerror(errno);
            return false;
        }
        if (ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
            error_ = std::string("PERF_EVENT_IOC_ENABLE failed: ") + std::strerror(errno);
            return false;
        }
        return true;
#else
        return false;
#endif
    }

    PerfValues stop()
    {
        PerfValues result;
#ifdef __linux__
        if (!available_) return result;
        if (ioctl(leader_fd_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0) {
            error_ = std::string("PERF_EVENT_IOC_DISABLE failed: ") + std::strerror(errno);
            return result;
        }

        const std::size_t words = 3 + 2 * fds_.size();
        std::vector<std::uint64_t> buffer(words, 0);
        const ssize_t expected = static_cast<ssize_t>(words * sizeof(std::uint64_t));
        const ssize_t actual = ::read(leader_fd_, buffer.data(), static_cast<std::size_t>(expected));
        if (actual != expected || buffer[0] != fds_.size()) {
            error_ = std::string("perf counter group read failed: ") + std::strerror(errno);
            return result;
        }

        const double time_enabled = static_cast<double>(buffer[1]);
        const double time_running = static_cast<double>(buffer[2]);
        if (time_enabled <= 0.0 || time_running <= 0.0) {
            error_ = "perf counter group was not scheduled on the CPU";
            return result;
        }
        const double scale = time_enabled / time_running;
        double values[4] = {0.0, 0.0, 0.0, 0.0};
        for (std::size_t index = 0; index < fds_.size(); ++index) {
            const double value = static_cast<double>(buffer[3 + 2 * index]) * scale;
            const std::uint64_t id = buffer[4 + 2 * index];
            const auto found = std::find(ids_.begin(), ids_.end(), id);
            if (found != ids_.end()) {
                values[static_cast<std::size_t>(found - ids_.begin())] = value;
            }
        }
        if (values[0] <= 0.0 || values[1] <= 0.0) {
            error_ = "hardware counters returned no cycle/instruction data";
            return result;
        }
        result.cycles = values[0];
        result.instructions = values[1];
        result.cache_references = values[2];
        result.cache_misses = values[3];
        result.valid = true;
#endif
        return result;
    }

private:
    void close_all()
    {
        for (int fd : fds_) {
            if (fd >= 0) ::close(fd);
        }
        fds_.clear();
        ids_.clear();
        leader_fd_ = -1;
        available_ = false;
    }

    int leader_fd_ = -1;
    std::vector<int> fds_;
    std::vector<std::uint64_t> ids_;
    bool available_ = false;
    std::string error_;
};

Profiler &Profiler::instance()
{
    static Profiler profiler;
    return profiler;
}

Profiler::Profiler() = default;
Profiler::~Profiler() = default;

void Profiler::configure(MPI_Comm comm, const ProfileConfig &config)
{
    comm_ = comm;
    config_ = config;
    configured_ = true;
    MPI_Comm_rank(comm_, &rank_);
    MPI_Comm_size(comm_, &process_count_);

    stages_.clear();
    stage_categories_.clear();
    peer_exchanges_.clear();
    metrics_.clear();
    metadata_.clear();
    cache_error_.clear();
    perf_active_ = false;

    for (const auto &definition : stage_definitions()) {
        stages_[definition.name] = StageLocalStats{};
        stage_categories_[definition.name] = definition.category;
    }
    for (const auto &metric : metric_definitions()) {
        metrics_[metric] = 0.0;
    }

    if (config_.enabled && config_.collect_hardware_cache) {
        perf_counters_ = std::make_unique<PerfCounterSet>(true);
        if (!perf_counters_->available()) {
            cache_error_ = perf_counters_->error();
        }
    } else {
        perf_counters_.reset();
    }
    metrics_["hardware_cache_available"] = 0.0;
    if (config_.enabled) {
        std::ifstream io_probe("/proc/self/io");
        metrics_["process_io_available"] = io_probe.is_open() ? 1.0 : 0.0;
    }
}

bool Profiler::enabled() const
{
    return configured_ && config_.enabled;
}

bool Profiler::core_only() const
{
    return enabled() && config_.core_only;
}

void Profiler::add_metadata(const std::string &key, const std::string &value)
{
    if (!enabled()) return;
    metadata_[key] = value;
}

void Profiler::set_metric(const std::string &key, double value)
{
    if (!enabled()) return;
    metrics_[key] = value;
}

void Profiler::add_metric(const std::string &key, double value)
{
    if (!enabled()) return;
    metrics_[key] += value;
}

void Profiler::add_communication(const std::string &stage,
                                 std::uint64_t send_messages,
                                 std::uint64_t receive_messages,
                                 std::uint64_t send_bytes,
                                 std::uint64_t receive_bytes)
{
    if (!enabled()) return;
    auto &stats = stages_[stage];
    stats.send_messages += send_messages;
    stats.receive_messages += receive_messages;
    stats.send_bytes += send_bytes;
    stats.receive_bytes += receive_bytes;
}

void Profiler::record_peer_exchange(const std::string &stage,
                                    int num_s,
                                    const int *destinations,
                                    const int *send_items,
                                    int num_r,
                                    const int *sources,
                                    const int *receive_items,
                                    std::uint64_t item_bytes)
{
    if (!enabled() || !config_.collect_communication_graph) return;
    if (num_s < 0 || num_r < 0 ||
        (num_s > 0 && (destinations == nullptr || send_items == nullptr)) ||
        (num_r > 0 && (sources == nullptr || receive_items == nullptr))) {
        throw std::invalid_argument("invalid peer exchange description for " + stage);
    }

    PeerExchangeLocal exchange;
    exchange.item_bytes = item_bytes;
    if (num_s > 0) {
        exchange.destinations.assign(destinations, destinations + num_s);
    }
    if (num_r > 0) {
        exchange.sources.assign(sources, sources + num_r);
    }
    exchange.send_items.reserve(static_cast<std::size_t>(num_s));
    exchange.receive_items.reserve(static_cast<std::size_t>(num_r));
    for (int index = 0; index < num_s; ++index) {
        if (send_items[index] < 0) {
            throw std::invalid_argument("negative send count for " + stage);
        }
        exchange.send_items.push_back(static_cast<std::uint64_t>(send_items[index]));
    }
    for (int index = 0; index < num_r; ++index) {
        if (receive_items[index] < 0) {
            throw std::invalid_argument("negative receive count for " + stage);
        }
        exchange.receive_items.push_back(static_cast<std::uint64_t>(receive_items[index]));
    }
    peer_exchanges_[stage] = std::move(exchange);
}

void Profiler::set_total_elapsed(double seconds)
{
    set_metric("total_wall_seconds", seconds);
}

ProcessSnapshot Profiler::process_snapshot(bool include_io_counters) const
{
    ProcessSnapshot result;
    if (include_io_counters) {
        std::ifstream input("/proc/self/io");
        std::string key;
        std::uint64_t value = 0;
        while (input >> key >> value) {
            key = trim_colon(key);
            if (key == "rchar") result.rchar = value;
            else if (key == "wchar") result.wchar = value;
            else if (key == "read_bytes") result.read_bytes = value;
            else if (key == "write_bytes") result.write_bytes = value;
        }
    }

    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        result.minor_faults = static_cast<std::uint64_t>(usage.ru_minflt);
        result.major_faults = static_cast<std::uint64_t>(usage.ru_majflt);
        result.voluntary_context_switches = static_cast<std::uint64_t>(usage.ru_nvcsw);
        result.involuntary_context_switches = static_cast<std::uint64_t>(usage.ru_nivcsw);
    }
    return result;
}

bool Profiler::start_perf_counters()
{
    if (!enabled() || perf_active_ || !cache_error_.empty() || !perf_counters_ ||
        !perf_counters_->available()) {
        return false;
    }
    perf_active_ = perf_counters_->start();
    if (!perf_active_ && cache_error_.empty()) cache_error_ = perf_counters_->error();
    return perf_active_;
}

PerfValues Profiler::stop_perf_counters()
{
    if (!perf_active_ || !perf_counters_) return {};
    PerfValues values = perf_counters_->stop();
    perf_active_ = false;
    if (!values.valid && cache_error_.empty()) cache_error_ = perf_counters_->error();
    return values;
}

void Profiler::record_stage(const std::string &stage,
                            const std::string &category,
                            double elapsed,
                            const ProcessSnapshot &before,
                            const ProcessSnapshot &after,
                            const PerfValues &perf)
{
    if (!enabled()) return;
    auto &stats = stages_[stage];
    if (!category.empty()) stage_categories_[stage] = category;
    stats.wall_seconds += elapsed;
    stats.calls += 1;
    stats.rchar += positive_delta(after.rchar, before.rchar);
    stats.wchar += positive_delta(after.wchar, before.wchar);
    stats.read_bytes += positive_delta(after.read_bytes, before.read_bytes);
    stats.write_bytes += positive_delta(after.write_bytes, before.write_bytes);
    stats.minor_faults += positive_delta(after.minor_faults, before.minor_faults);
    stats.major_faults += positive_delta(after.major_faults, before.major_faults);
    stats.voluntary_context_switches +=
        positive_delta(after.voluntary_context_switches, before.voluntary_context_switches);
    stats.involuntary_context_switches +=
        positive_delta(after.involuntary_context_switches, before.involuntary_context_switches);
    if (perf.valid) {
        stats.cycles += perf.cycles;
        stats.instructions += perf.instructions;
        stats.cache_references += perf.cache_references;
        stats.cache_misses += perf.cache_misses;
        stats.perf_samples += 1;
    }
}

StageScope::StageScope(const std::string &stage, const std::string &category)
    : stage_(stage), category_(category)
{
    auto &profiler = Profiler::instance();
    if (!profiler.enabled()) return;
    active_ = true;
    collect_io_counters_ = category_ == "io";
    before_ = profiler.process_snapshot(collect_io_counters_);
    perf_started_ = profiler.start_perf_counters();
    start_time_ = MPI_Wtime();
}

StageScope::~StageScope()
{
    if (!active_) return;
    const double elapsed = MPI_Wtime() - start_time_;
    auto &profiler = Profiler::instance();
    PerfValues perf;
    if (perf_started_) perf = profiler.stop_perf_counters();
    const ProcessSnapshot after = profiler.process_snapshot(collect_io_counters_);
    profiler.record_stage(stage_, category_, elapsed, before_, after, perf);
}

void Profiler::finalize()
{
    if (!enabled()) return;

    std::uint64_t perf_samples = 0;
    for (const auto &[stage, stats] : stages_) {
        (void)stage;
        perf_samples += stats.perf_samples;
    }
    metrics_["hardware_cache_available"] =
        config_.collect_hardware_cache && perf_counters_ && perf_counters_->available() &&
                cache_error_.empty() && perf_samples > 0
            ? 1.0
            : 0.0;
    if (config_.collect_hardware_cache && perf_samples == 0 && cache_error_.empty()) {
        cache_error_ = "no valid hardware counter samples were collected";
    }

    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
        metrics_["peak_rss_mib"] = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
        metrics_["peak_rss_mib"] = static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
    }

    const auto &definitions = stage_definitions();
    std::vector<WireStage> local_stages(definitions.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto found = stages_.find(definitions[index].name);
        if (found == stages_.end()) continue;
        const auto &source = found->second;
        auto &target = local_stages[index];
        target.wall_seconds = source.wall_seconds;
        target.calls = static_cast<double>(source.calls);
        target.cycles = source.cycles;
        target.instructions = source.instructions;
        target.cache_references = source.cache_references;
        target.cache_misses = source.cache_misses;
        target.perf_samples = static_cast<double>(source.perf_samples);
        target.rchar = static_cast<double>(source.rchar);
        target.wchar = static_cast<double>(source.wchar);
        target.read_bytes = static_cast<double>(source.read_bytes);
        target.write_bytes = static_cast<double>(source.write_bytes);
        target.minor_faults = static_cast<double>(source.minor_faults);
        target.major_faults = static_cast<double>(source.major_faults);
        target.voluntary_context_switches =
            static_cast<double>(source.voluntary_context_switches);
        target.involuntary_context_switches =
            static_cast<double>(source.involuntary_context_switches);
        target.send_messages = static_cast<double>(source.send_messages);
        target.receive_messages = static_cast<double>(source.receive_messages);
        target.send_bytes = static_cast<double>(source.send_bytes);
        target.receive_bytes = static_cast<double>(source.receive_bytes);
    }

    std::vector<WireStage> all_stages;
    if (rank_ == 0) all_stages.resize(definitions.size() * process_count_);
    const int stage_bytes = static_cast<int>(local_stages.size() * sizeof(WireStage));
    MPI_Gather(local_stages.data(), stage_bytes, MPI_BYTE,
               rank_ == 0 ? all_stages.data() : nullptr, stage_bytes, MPI_BYTE,
               0, comm_);

    const auto &metric_names = metric_definitions();
    std::vector<double> local_metrics(metric_names.size(), 0.0);
    for (std::size_t index = 0; index < metric_names.size(); ++index) {
        const auto found = metrics_.find(metric_names[index]);
        if (found != metrics_.end()) local_metrics[index] = found->second;
    }
    std::vector<double> all_metrics;
    if (rank_ == 0) all_metrics.resize(metric_names.size() * process_count_);
    MPI_Gather(local_metrics.data(), static_cast<int>(local_metrics.size()), MPI_DOUBLE,
               rank_ == 0 ? all_metrics.data() : nullptr,
               static_cast<int>(local_metrics.size()), MPI_DOUBLE,
               0, comm_);

    char processor[MPI_MAX_PROCESSOR_NAME] = {};
    int processor_length = 0;
    MPI_Get_processor_name(processor, &processor_length);
    std::vector<char> all_processors;
    if (rank_ == 0) {
        all_processors.resize(static_cast<std::size_t>(process_count_) * MPI_MAX_PROCESSOR_NAME, 0);
    }
    MPI_Gather(processor, MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
               rank_ == 0 ? all_processors.data() : nullptr,
               MPI_MAX_PROCESSOR_NAME, MPI_CHAR, 0, comm_);

    // Detailed peer lists are gathered only after total_wall_seconds has been
    // fixed by the application.  This keeps the optional diagnostic traffic
    // and rank-0 CSV output outside all reported application timings.
    constexpr int peer_record_width = 5;
    std::vector<long long> local_peer_records;
    std::vector<int> peer_value_counts;
    std::vector<int> peer_value_displacements;
    std::vector<long long> all_peer_records;
    if (config_.collect_communication_graph) {
        const auto &peer_definitions = peer_exchange_definitions();
        for (std::size_t stage = 0; stage < peer_definitions.size(); ++stage) {
            const auto found = peer_exchanges_.find(peer_definitions[stage]);
            if (found == peer_exchanges_.end()) continue;
            const PeerExchangeLocal &exchange = found->second;
            for (std::size_t index = 0; index < exchange.destinations.size(); ++index) {
                const std::uint64_t items = exchange.send_items[index];
                local_peer_records.insert(
                    local_peer_records.end(),
                    {static_cast<long long>(stage), 0,
                     static_cast<long long>(exchange.destinations[index]),
                     static_cast<long long>(items),
                     static_cast<long long>(items * exchange.item_bytes)});
            }
            for (std::size_t index = 0; index < exchange.sources.size(); ++index) {
                const std::uint64_t items = exchange.receive_items[index];
                local_peer_records.insert(
                    local_peer_records.end(),
                    {static_cast<long long>(stage), 1,
                     static_cast<long long>(exchange.sources[index]),
                     static_cast<long long>(items),
                     static_cast<long long>(items * exchange.item_bytes)});
            }
        }

        const int local_value_count = static_cast<int>(local_peer_records.size());
        if (rank_ == 0) peer_value_counts.resize(process_count_, 0);
        MPI_Gather(&local_value_count, 1, MPI_INT,
                   rank_ == 0 ? peer_value_counts.data() : nullptr,
                   1, MPI_INT, 0, comm_);

        if (rank_ == 0) {
            peer_value_displacements.resize(process_count_, 0);
            int total_values = 0;
            for (int rank = 0; rank < process_count_; ++rank) {
                peer_value_displacements[rank] = total_values;
                total_values += peer_value_counts[rank];
            }
            all_peer_records.resize(static_cast<std::size_t>(total_values));
        }
        MPI_Gatherv(local_peer_records.data(), local_value_count, MPI_LONG_LONG,
                    rank_ == 0 ? all_peer_records.data() : nullptr,
                    rank_ == 0 ? peer_value_counts.data() : nullptr,
                    rank_ == 0 ? peer_value_displacements.data() : nullptr,
                    MPI_LONG_LONG, 0, comm_);
    }

    if (rank_ == 0) {
        try {
            namespace fs = std::filesystem;
            const std::string timestamp = current_timestamp();
            const std::string experiment = sanitize_component(config_.experiment);
            std::ostringstream process_folder;
            process_folder << 'p' << std::setw(5) << std::setfill('0') << process_count_;
            std::ostringstream repeat_folder;
            repeat_folder << "repeat_" << std::setw(2) << std::setfill('0') << config_.repeat
                          << '_' << timestamp;
            const fs::path run_directory = fs::path(config_.output_root) /
                                           experiment /
                                           process_folder.str() /
                                           repeat_folder.str();
            fs::create_directories(run_directory);

            std::vector<std::string> hosts(process_count_);
            for (int rank = 0; rank < process_count_; ++rank) {
                hosts[rank] = std::string(all_processors.data() +
                                          static_cast<std::size_t>(rank) * MPI_MAX_PROCESSOR_NAME);
            }

            const auto metric_index = [&](const std::string &name) -> std::size_t {
                const auto found = std::find(metric_names.begin(), metric_names.end(), name);
                return found == metric_names.end()
                           ? metric_names.size()
                           : static_cast<std::size_t>(found - metric_names.begin());
            };
            const auto metric_values = [&](const std::string &name) {
                std::vector<double> values(process_count_, 0.0);
                const std::size_t index = metric_index(name);
                if (index == metric_names.size()) return values;
                for (int rank = 0; rank < process_count_; ++rank) {
                    values[rank] = all_metrics[static_cast<std::size_t>(rank) * metric_names.size() + index];
                }
                return values;
            };
            const auto stage_values = [&](const std::string &name,
                                          double WireStage::*member) {
                std::vector<double> values(process_count_, 0.0);
                const auto found = std::find_if(
                    definitions.begin(), definitions.end(),
                    [&](const StageDefinition &definition) {
                        return name == definition.name;
                    });
                if (found == definitions.end()) return values;
                const std::size_t stage = static_cast<std::size_t>(
                    std::distance(definitions.begin(), found));
                for (int rank = 0; rank < process_count_; ++rank) {
                    const WireStage &wire = all_stages[static_cast<std::size_t>(rank) *
                                                        definitions.size() + stage];
                    values[rank] = wire.*member;
                }
                return values;
            };

            const std::vector<double> total_times = metric_values("total_wall_seconds");
            const ScalarStats total_stats = summarize(total_times);
            const ScalarStats peak_rss_stats = summarize(metric_values("peak_rss_mib"));
            const ScalarStats local_volume_stats =
                summarize(metric_values("local_volume_elements_before_adjacency"));
            const ScalarStats cache_available_stats =
                summarize(metric_values("hardware_cache_available"));
            const ScalarStats io_available_stats =
                summarize(metric_values("process_io_available"));
            const std::vector<double> volume_alltoall_wait_times = stage_values(
                "volume_size_exchange_pre_collective_wait", &WireStage::wall_seconds);
            const std::vector<double> volume_alltoall_times = stage_values(
                "volume_size_exchange", &WireStage::wall_seconds);
            const std::vector<double> volume_payload_times = stage_values(
                "volume_payload_exchange", &WireStage::wall_seconds);
            const std::vector<double> volume_waitall_times =
                metric_values("volume_waitall_seconds");
            const std::vector<double> volume_send_neighbors = stage_values(
                "volume_payload_exchange", &WireStage::send_messages);
            const std::vector<double> volume_receive_neighbors = stage_values(
                "volume_payload_exchange", &WireStage::receive_messages);
            const std::vector<double> volume_send_bytes = stage_values(
                "volume_payload_exchange", &WireStage::send_bytes);
            const std::vector<double> volume_receive_bytes = stage_values(
                "volume_payload_exchange", &WireStage::receive_bytes);
            const ScalarStats volume_alltoall_wait_stats = summarize(volume_alltoall_wait_times);
            const ScalarStats volume_alltoall_stats = summarize(volume_alltoall_times);
            const ScalarStats volume_payload_stats = summarize(volume_payload_times);
            const ScalarStats volume_waitall_stats = summarize(volume_waitall_times);
            const ScalarStats volume_send_neighbor_stats = summarize(volume_send_neighbors);
            const ScalarStats volume_receive_neighbor_stats = summarize(volume_receive_neighbors);
            const ScalarStats volume_send_byte_stats = summarize(volume_send_bytes);
            const ScalarStats volume_receive_byte_stats = summarize(volume_receive_bytes);
            const double volume_directed_edges = std::accumulate(
                volume_send_neighbors.begin(), volume_send_neighbors.end(), 0.0);
            const double possible_directed_edges =
                process_count_ > 1
                    ? static_cast<double>(process_count_) * static_cast<double>(process_count_ - 1)
                    : 0.0;
            const double volume_edge_density = safe_ratio(
                volume_directed_edges, possible_directed_edges);

            std::map<std::string, std::vector<double>> category_times;
            for (const std::string category : {"setup", "compute", "communication",
                                                "synchronization", "io", "postprocess"}) {
                category_times[category] = std::vector<double>(process_count_, 0.0);
            }

            double cache_references_total = 0.0;
            double cache_misses_total = 0.0;
            double cycles_total = 0.0;
            double instructions_total = 0.0;
            double minor_faults_total = 0.0;
            double major_faults_total = 0.0;
            double rchar_total = 0.0;
            double wchar_total = 0.0;
            double read_bytes_total = 0.0;
            double write_bytes_total = 0.0;

            for (int rank = 0; rank < process_count_; ++rank) {
                for (std::size_t stage = 0; stage < definitions.size(); ++stage) {
                    const WireStage &wire = all_stages[static_cast<std::size_t>(rank) *
                                                        definitions.size() + stage];
                    category_times[definitions[stage].category][rank] += wire.wall_seconds;
                    cache_references_total += wire.cache_references;
                    cache_misses_total += wire.cache_misses;
                    cycles_total += wire.cycles;
                    instructions_total += wire.instructions;
                    minor_faults_total += wire.minor_faults;
                    major_faults_total += wire.major_faults;
                    rchar_total += wire.rchar;
                    wchar_total += wire.wchar;
                    read_bytes_total += wire.read_bytes;
                    write_bytes_total += wire.write_bytes;
                }
            }

            const ScalarStats setup_stats = summarize(category_times["setup"]);
            const ScalarStats compute_stats = summarize(category_times["compute"]);
            const ScalarStats communication_stats = summarize(category_times["communication"]);
            const ScalarStats synchronization_stats = summarize(category_times["synchronization"]);
            std::vector<double> communication_and_wait_times(process_count_, 0.0);
            for (int rank = 0; rank < process_count_; ++rank) {
                communication_and_wait_times[rank] =
                    category_times["communication"][rank] +
                    category_times["synchronization"][rank];
            }
            const ScalarStats communication_and_wait_stats =
                summarize(communication_and_wait_times);
            const ScalarStats io_stats = summarize(category_times["io"]);
            const ScalarStats postprocess_stats = summarize(category_times["postprocess"]);
            std::vector<double> unprofiled_times(process_count_, 0.0);
            for (int rank = 0; rank < process_count_; ++rank) {
                double profiled = 0.0;
                for (const std::string category : {"setup", "compute", "communication",
                                                    "synchronization", "io", "postprocess"}) {
                    profiled += category_times[category][rank];
                }
                unprofiled_times[rank] = std::max(0.0, total_times[rank] - profiled);
            }
            const ScalarStats unprofiled_stats = summarize(unprofiled_times);
            const double profile_coverage_percent = 100.0 * safe_ratio(
                total_stats.average - unprofiled_stats.average, total_stats.average);

            {
                std::ofstream output = open_output_file(run_directory / "stages.csv");
                output << "stage,category,time_min_s,time_avg_s,time_max_s,max_over_avg,"
                          "max_percent_of_total,cache_references_total,cache_misses_total,"
                          "cache_miss_rate_percent,instructions_total,cycles_total,ipc,"
                          "minor_faults_total,major_faults_total,rchar_total_bytes,wchar_total_bytes,"
                          "physical_read_total_bytes,physical_write_total_bytes,"
                          "send_messages_total,receive_messages_total,send_total_bytes,"
                          "receive_total_bytes,send_max_rank_bytes,receive_max_rank_bytes,"
                          "time_p50_s,time_p95_s,time_p99_s,"
                          "send_messages_avg,send_messages_p95,send_messages_p99,send_messages_max,"
                          "receive_messages_avg,receive_messages_p95,receive_messages_p99,receive_messages_max,"
                          "send_bytes_avg,send_bytes_p95,send_bytes_p99,"
                          "receive_bytes_avg,receive_bytes_p95,receive_bytes_p99\n";
                output << std::setprecision(12);
                for (std::size_t stage = 0; stage < definitions.size(); ++stage) {
                    std::vector<double> times(process_count_, 0.0);
                    double refs = 0.0, misses = 0.0, instructions = 0.0, cycles = 0.0;
                    double minflt = 0.0, majflt = 0.0, rchar = 0.0, wchar = 0.0;
                    double read_bytes = 0.0, write_bytes = 0.0;
                    double send_messages = 0.0, receive_messages = 0.0;
                    double send_bytes = 0.0, receive_bytes = 0.0;
                    double send_bytes_max = 0.0, receive_bytes_max = 0.0;
                    std::vector<double> send_message_values(process_count_, 0.0);
                    std::vector<double> receive_message_values(process_count_, 0.0);
                    std::vector<double> send_byte_values(process_count_, 0.0);
                    std::vector<double> receive_byte_values(process_count_, 0.0);
                    for (int rank = 0; rank < process_count_; ++rank) {
                        const WireStage &wire = all_stages[static_cast<std::size_t>(rank) *
                                                            definitions.size() + stage];
                        times[rank] = wire.wall_seconds;
                        refs += wire.cache_references;
                        misses += wire.cache_misses;
                        instructions += wire.instructions;
                        cycles += wire.cycles;
                        minflt += wire.minor_faults;
                        majflt += wire.major_faults;
                        rchar += wire.rchar;
                        wchar += wire.wchar;
                        read_bytes += wire.read_bytes;
                        write_bytes += wire.write_bytes;
                        send_messages += wire.send_messages;
                        receive_messages += wire.receive_messages;
                        send_bytes += wire.send_bytes;
                        receive_bytes += wire.receive_bytes;
                        send_bytes_max = std::max(send_bytes_max, wire.send_bytes);
                        receive_bytes_max = std::max(receive_bytes_max, wire.receive_bytes);
                        send_message_values[rank] = wire.send_messages;
                        receive_message_values[rank] = wire.receive_messages;
                        send_byte_values[rank] = wire.send_bytes;
                        receive_byte_values[rank] = wire.receive_bytes;
                    }
                    const ScalarStats time = summarize(times);
                    const ScalarStats send_message_stats = summarize(send_message_values);
                    const ScalarStats receive_message_stats = summarize(receive_message_values);
                    const ScalarStats send_byte_stats = summarize(send_byte_values);
                    const ScalarStats receive_byte_stats = summarize(receive_byte_values);
                    output << definitions[stage].name << ',' << definitions[stage].category << ','
                           << time.minimum << ',' << time.average << ',' << time.maximum << ','
                           << safe_ratio(time.maximum, time.average) << ','
                           << 100.0 * safe_ratio(time.maximum, total_stats.maximum) << ','
                           << refs << ',' << misses << ',' << 100.0 * safe_ratio(misses, refs) << ','
                           << instructions << ',' << cycles << ','
                           << safe_ratio(instructions, cycles) << ','
                           << minflt << ',' << majflt << ',' << rchar << ',' << wchar << ','
                           << read_bytes << ',' << write_bytes << ','
                           << send_messages << ',' << receive_messages << ','
                           << send_bytes << ',' << receive_bytes << ','
                           << send_bytes_max << ',' << receive_bytes_max << ','
                           << percentile(times, 0.50) << ','
                           << percentile(times, 0.95) << ','
                           << percentile(times, 0.99) << ','
                           << send_message_stats.average << ','
                           << percentile(send_message_values, 0.95) << ','
                           << percentile(send_message_values, 0.99) << ','
                           << send_message_stats.maximum << ','
                           << receive_message_stats.average << ','
                           << percentile(receive_message_values, 0.95) << ','
                           << percentile(receive_message_values, 0.99) << ','
                           << receive_message_stats.maximum << ','
                           << send_byte_stats.average << ','
                           << percentile(send_byte_values, 0.95) << ','
                           << percentile(send_byte_values, 0.99) << ','
                           << receive_byte_stats.average << ','
                           << percentile(receive_byte_values, 0.95) << ','
                           << percentile(receive_byte_values, 0.99) << '\n';
                }
            }

            {
                std::ofstream output = open_output_file(run_directory / "rank_stages.csv");
                output << "rank,host,stage,category,time_s,calls,cache_references,cache_misses,"
                          "cache_miss_rate_percent,instructions,cycles,ipc,minor_faults,major_faults,"
                          "rchar_bytes,wchar_bytes,physical_read_bytes,physical_write_bytes,"
                          "voluntary_context_switches,involuntary_context_switches,"
                          "send_messages,receive_messages,send_bytes,receive_bytes\n";
                output << std::setprecision(12);
                for (int rank = 0; rank < process_count_; ++rank) {
                    for (std::size_t stage = 0; stage < definitions.size(); ++stage) {
                        const WireStage &wire = all_stages[static_cast<std::size_t>(rank) *
                                                            definitions.size() + stage];
                        output << rank << ',' << csv_escape(hosts[rank]) << ','
                               << definitions[stage].name << ',' << definitions[stage].category << ','
                               << wire.wall_seconds << ',' << wire.calls << ','
                               << wire.cache_references << ',' << wire.cache_misses << ','
                               << 100.0 * safe_ratio(wire.cache_misses, wire.cache_references) << ','
                               << wire.instructions << ',' << wire.cycles << ','
                               << safe_ratio(wire.instructions, wire.cycles) << ','
                               << wire.minor_faults << ',' << wire.major_faults << ','
                               << wire.rchar << ',' << wire.wchar << ','
                               << wire.read_bytes << ',' << wire.write_bytes << ','
                               << wire.voluntary_context_switches << ','
                               << wire.involuntary_context_switches << ','
                               << wire.send_messages << ',' << wire.receive_messages << ','
                               << wire.send_bytes << ',' << wire.receive_bytes << '\n';
                    }
                }
            }

            {
                std::ofstream output = open_output_file(run_directory / "rank_metrics.csv");
                output << "rank,host";
                for (const auto &metric : metric_names) output << ',' << metric;
                output << '\n' << std::setprecision(12);
                for (int rank = 0; rank < process_count_; ++rank) {
                    output << rank << ',' << csv_escape(hosts[rank]);
                    for (std::size_t metric = 0; metric < metric_names.size(); ++metric) {
                        output << ',' << all_metrics[static_cast<std::size_t>(rank) *
                                                     metric_names.size() + metric];
                    }
                    output << '\n';
                }
            }

            if (config_.collect_communication_graph) {
                struct RankPeerGraph {
                    std::vector<int> outgoing;
                    std::vector<int> incoming;
                    std::vector<std::uint64_t> send_items;
                    std::vector<std::uint64_t> receive_items;
                    std::uint64_t send_bytes = 0;
                    std::uint64_t receive_bytes = 0;
                };
                struct EdgeObservation {
                    bool sender_seen = false;
                    bool receiver_seen = false;
                    std::uint64_t send_items = 0;
                    std::uint64_t receive_items = 0;
                    std::uint64_t send_bytes = 0;
                    std::uint64_t receive_bytes = 0;
                };

                const auto &peer_definitions = peer_exchange_definitions();
                std::vector<std::vector<RankPeerGraph>> rank_graphs(
                    peer_definitions.size(),
                    std::vector<RankPeerGraph>(static_cast<std::size_t>(process_count_)));
                std::map<std::tuple<std::size_t, int, int>, EdgeObservation> edges;

                for (int rank = 0; rank < process_count_; ++rank) {
                    const int value_count = peer_value_counts[rank];
                    if (value_count % peer_record_width != 0) {
                        throw std::runtime_error("corrupt communication graph record width");
                    }
                    const int begin = peer_value_displacements[rank];
                    for (int value = 0; value < value_count; value += peer_record_width) {
                        const std::size_t offset = static_cast<std::size_t>(begin + value);
                        const long long stage_value = all_peer_records[offset];
                        const long long direction = all_peer_records[offset + 1];
                        const long long peer_value = all_peer_records[offset + 2];
                        const long long item_value = all_peer_records[offset + 3];
                        const long long byte_value = all_peer_records[offset + 4];
                        if (stage_value < 0 ||
                            stage_value >= static_cast<long long>(peer_definitions.size()) ||
                            (direction != 0 && direction != 1) ||
                            peer_value < 0 || peer_value >= process_count_ ||
                            peer_value == rank || item_value < 0 || byte_value < 0) {
                            throw std::runtime_error("invalid communication graph record");
                        }

                        const std::size_t stage = static_cast<std::size_t>(stage_value);
                        const int peer = static_cast<int>(peer_value);
                        const std::uint64_t items = static_cast<std::uint64_t>(item_value);
                        const std::uint64_t bytes = static_cast<std::uint64_t>(byte_value);
                        RankPeerGraph &graph = rank_graphs[stage][rank];
                        if (direction == 0) {
                            graph.outgoing.push_back(peer);
                            graph.send_items.push_back(items);
                            graph.send_bytes += bytes;
                            EdgeObservation &edge = edges[{stage, rank, peer}];
                            edge.sender_seen = true;
                            edge.send_items = items;
                            edge.send_bytes = bytes;
                        } else {
                            graph.incoming.push_back(peer);
                            graph.receive_items.push_back(items);
                            graph.receive_bytes += bytes;
                            EdgeObservation &edge = edges[{stage, peer, rank}];
                            edge.receiver_seen = true;
                            edge.receive_items = items;
                            edge.receive_bytes = bytes;
                        }
                    }
                }

                {
                    std::ofstream output = open_output_file(
                        run_directory / "communication_peers.csv");
                    output << "rank,host,stage,num_s,num_r,out_neighbors,in_neighbors,"
                              "out_items_by_peer,in_items_by_peer,only_out_neighbors,"
                              "only_in_neighbors,neighbor_union,asymmetry_ratio,"
                              "send_items,receive_items,send_bytes,receive_bytes\n";
                    output << std::setprecision(12);
                    for (std::size_t stage = 0; stage < peer_definitions.size(); ++stage) {
                        for (int rank = 0; rank < process_count_; ++rank) {
                            RankPeerGraph &graph = rank_graphs[stage][rank];
                            std::vector<std::pair<int, std::uint64_t>> outgoing_pairs;
                            std::vector<std::pair<int, std::uint64_t>> incoming_pairs;
                            for (std::size_t index = 0; index < graph.outgoing.size(); ++index) {
                                outgoing_pairs.emplace_back(
                                    graph.outgoing[index], graph.send_items[index]);
                            }
                            for (std::size_t index = 0; index < graph.incoming.size(); ++index) {
                                incoming_pairs.emplace_back(
                                    graph.incoming[index], graph.receive_items[index]);
                            }
                            std::sort(outgoing_pairs.begin(), outgoing_pairs.end());
                            std::sort(incoming_pairs.begin(), incoming_pairs.end());
                            graph.outgoing.clear();
                            graph.send_items.clear();
                            graph.incoming.clear();
                            graph.receive_items.clear();
                            for (const auto &[peer, items] : outgoing_pairs) {
                                graph.outgoing.push_back(peer);
                                graph.send_items.push_back(items);
                            }
                            for (const auto &[peer, items] : incoming_pairs) {
                                graph.incoming.push_back(peer);
                                graph.receive_items.push_back(items);
                            }

                            std::vector<int> only_out;
                            std::vector<int> only_in;
                            std::vector<int> neighbor_union;
                            std::set_difference(
                                graph.outgoing.begin(), graph.outgoing.end(),
                                graph.incoming.begin(), graph.incoming.end(),
                                std::back_inserter(only_out));
                            std::set_difference(
                                graph.incoming.begin(), graph.incoming.end(),
                                graph.outgoing.begin(), graph.outgoing.end(),
                                std::back_inserter(only_in));
                            std::set_union(
                                graph.outgoing.begin(), graph.outgoing.end(),
                                graph.incoming.begin(), graph.incoming.end(),
                                std::back_inserter(neighbor_union));
                            const double asymmetry = safe_ratio(
                                static_cast<double>(only_out.size() + only_in.size()),
                                static_cast<double>(neighbor_union.size()));
                            const std::uint64_t send_items = std::accumulate(
                                graph.send_items.begin(), graph.send_items.end(),
                                static_cast<std::uint64_t>(0));
                            const std::uint64_t receive_items = std::accumulate(
                                graph.receive_items.begin(), graph.receive_items.end(),
                                static_cast<std::uint64_t>(0));
                            output << rank << ',' << csv_escape(hosts[rank]) << ','
                                   << peer_definitions[stage] << ','
                                   << graph.outgoing.size() << ',' << graph.incoming.size() << ','
                                   << csv_escape(join_integers(graph.outgoing)) << ','
                                   << csv_escape(join_integers(graph.incoming)) << ','
                                   << csv_escape(join_peer_items(
                                          graph.outgoing, graph.send_items)) << ','
                                   << csv_escape(join_peer_items(
                                          graph.incoming, graph.receive_items)) << ','
                                   << csv_escape(join_integers(only_out)) << ','
                                   << csv_escape(join_integers(only_in)) << ','
                                   << neighbor_union.size() << ',' << asymmetry << ','
                                   << send_items << ',' << receive_items << ','
                                   << graph.send_bytes << ',' << graph.receive_bytes << '\n';
                        }
                    }
                }

                {
                    std::ofstream output = open_output_file(
                        run_directory / "communication_edges.csv");
                    output << "stage,source_rank,destination_rank,send_items,"
                              "receiver_items,send_bytes,receiver_bytes,sender_seen,"
                              "receiver_seen,count_match\n";
                    for (const auto &[key, edge] : edges) {
                        const auto &[stage, source, destination] = key;
                        const bool match = edge.sender_seen && edge.receiver_seen &&
                                           edge.send_items == edge.receive_items &&
                                           edge.send_bytes == edge.receive_bytes;
                        output << peer_definitions[stage] << ',' << source << ','
                               << destination << ',' << edge.send_items << ','
                               << edge.receive_items << ',' << edge.send_bytes << ','
                               << edge.receive_bytes << ',' << (edge.sender_seen ? 1 : 0) << ','
                               << (edge.receiver_seen ? 1 : 0) << ',' << (match ? 1 : 0) << '\n';
                    }
                }

                {
                    std::ofstream output = open_output_file(
                        run_directory / "communication_graph_summary.csv");
                    output << "stage,processes,directed_edges,possible_directed_edges,"
                              "edge_density,edge_density_percent,zero_edge_sparsity_percent,"
                              "num_s_num_r_different_ranks,asymmetric_ranks,"
                              "rank_asymmetry_avg,rank_asymmetry_p95,"
                              "rank_asymmetry_p99,rank_asymmetry_max,count_mismatch_edges,"
                              "missing_sender_edges,missing_receiver_edges,"
                              "send_neighbors_avg,send_neighbors_p95,send_neighbors_p99,send_neighbors_max,"
                              "receive_neighbors_avg,receive_neighbors_p95,receive_neighbors_p99,receive_neighbors_max,"
                              "send_items_total,receive_items_total,send_bytes_total,receive_bytes_total\n";
                    output << std::setprecision(12);
                    for (std::size_t stage = 0; stage < peer_definitions.size(); ++stage) {
                        std::vector<double> asymmetries(process_count_, 0.0);
                        std::vector<double> send_neighbors(process_count_, 0.0);
                        std::vector<double> receive_neighbors(process_count_, 0.0);
                        std::uint64_t send_items_total = 0;
                        std::uint64_t receive_items_total = 0;
                        std::uint64_t send_bytes_total = 0;
                        std::uint64_t receive_bytes_total = 0;
                        int asymmetric_ranks = 0;
                        int count_different_ranks = 0;
                        for (int rank = 0; rank < process_count_; ++rank) {
                            const RankPeerGraph &graph = rank_graphs[stage][rank];
                            std::vector<int> only_out;
                            std::vector<int> only_in;
                            std::vector<int> neighbor_union;
                            std::set_difference(
                                graph.outgoing.begin(), graph.outgoing.end(),
                                graph.incoming.begin(), graph.incoming.end(),
                                std::back_inserter(only_out));
                            std::set_difference(
                                graph.incoming.begin(), graph.incoming.end(),
                                graph.outgoing.begin(), graph.outgoing.end(),
                                std::back_inserter(only_in));
                            std::set_union(
                                graph.outgoing.begin(), graph.outgoing.end(),
                                graph.incoming.begin(), graph.incoming.end(),
                                std::back_inserter(neighbor_union));
                            asymmetries[rank] = safe_ratio(
                                static_cast<double>(only_out.size() + only_in.size()),
                                static_cast<double>(neighbor_union.size()));
                            if (!only_out.empty() || !only_in.empty()) ++asymmetric_ranks;
                            if (graph.outgoing.size() != graph.incoming.size()) {
                                ++count_different_ranks;
                            }
                            send_neighbors[rank] = static_cast<double>(graph.outgoing.size());
                            receive_neighbors[rank] = static_cast<double>(graph.incoming.size());
                            send_items_total += std::accumulate(
                                graph.send_items.begin(), graph.send_items.end(),
                                static_cast<std::uint64_t>(0));
                            receive_items_total += std::accumulate(
                                graph.receive_items.begin(), graph.receive_items.end(),
                                static_cast<std::uint64_t>(0));
                            send_bytes_total += graph.send_bytes;
                            receive_bytes_total += graph.receive_bytes;
                        }

                        double directed_edges = 0.0;
                        int mismatch_edges = 0;
                        int missing_sender_edges = 0;
                        int missing_receiver_edges = 0;
                        for (const auto &[key, edge] : edges) {
                            if (std::get<0>(key) != stage) continue;
                            if (edge.sender_seen) directed_edges += 1.0;
                            if (!edge.sender_seen) ++missing_sender_edges;
                            if (!edge.receiver_seen) ++missing_receiver_edges;
                            if (!edge.sender_seen || !edge.receiver_seen ||
                                edge.send_items != edge.receive_items ||
                                edge.send_bytes != edge.receive_bytes) {
                                ++mismatch_edges;
                            }
                        }
                        const double possible_edges =
                            process_count_ > 1
                                ? static_cast<double>(process_count_) *
                                      static_cast<double>(process_count_ - 1)
                                : 0.0;
                        const double density = safe_ratio(directed_edges, possible_edges);
                        const double sparsity_percent =
                            possible_edges > 0.0 ? 100.0 * (1.0 - density) : 0.0;
                        const ScalarStats asymmetry_stats = summarize(asymmetries);
                        const ScalarStats send_neighbor_stats = summarize(send_neighbors);
                        const ScalarStats receive_neighbor_stats = summarize(receive_neighbors);
                        output << peer_definitions[stage] << ',' << process_count_ << ','
                               << directed_edges << ',' << possible_edges << ',' << density << ','
                               << 100.0 * density << ',' << sparsity_percent << ','
                               << count_different_ranks << ',' << asymmetric_ranks << ','
                               << asymmetry_stats.average << ','
                               << percentile(asymmetries, 0.95) << ','
                               << percentile(asymmetries, 0.99) << ','
                               << asymmetry_stats.maximum << ',' << mismatch_edges << ','
                               << missing_sender_edges << ',' << missing_receiver_edges << ','
                               << send_neighbor_stats.average << ','
                               << percentile(send_neighbors, 0.95) << ','
                               << percentile(send_neighbors, 0.99) << ','
                               << send_neighbor_stats.maximum << ','
                               << receive_neighbor_stats.average << ','
                               << percentile(receive_neighbors, 0.95) << ','
                               << percentile(receive_neighbors, 0.99) << ','
                               << receive_neighbor_stats.maximum << ','
                               << send_items_total << ',' << receive_items_total << ','
                               << send_bytes_total << ',' << receive_bytes_total << '\n';
                    }
                }
            }

            const double communication_fraction_avg = 100.0 * safe_ratio(
                communication_stats.average, total_stats.average);
            const double communication_fraction_max = 100.0 * safe_ratio(
                communication_stats.maximum, total_stats.maximum);
            const double arrival_wait_fraction_avg = 100.0 * safe_ratio(
                synchronization_stats.average, total_stats.average);
            const double arrival_wait_fraction_max = 100.0 * safe_ratio(
                synchronization_stats.maximum, total_stats.maximum);
            const double communication_plus_wait_fraction_avg = 100.0 * safe_ratio(
                communication_and_wait_stats.average, total_stats.average);
            const double communication_plus_wait_fraction_max = 100.0 * safe_ratio(
                communication_and_wait_stats.maximum, total_stats.maximum);
            const double io_fraction_max =
                100.0 * safe_ratio(io_stats.maximum, total_stats.maximum);

            {
                std::ofstream output = open_output_file(run_directory / "run_summary.csv");
                output << "experiment,processes,repeat,timestamp,total_wall_avg_s,total_wall_max_s,"
                          "unprofiled_avg_s,unprofiled_max_s,profile_coverage_percent,"
                          "setup_avg_s,setup_max_s,compute_avg_s,compute_max_s,postprocess_avg_s,"
                          "postprocess_max_s,communication_avg_s,communication_max_s,"
                          "synchronization_avg_s,synchronization_max_s,"
                          "communication_fraction_avg_percent,communication_fraction_max_percent,"
                          "arrival_wait_fraction_avg_percent,arrival_wait_fraction_max_percent,"
                          "communication_plus_wait_fraction_avg_percent,"
                          "communication_plus_wait_fraction_max_percent,"
                          "io_avg_s,io_max_s,io_fraction_max_percent,"
                          "io_counter_available_ranks,"
                          "cache_available_ranks,cache_references_total,cache_misses_total,"
                          "cache_miss_rate_percent,instructions_total,cycles_total,ipc,"
                          "minor_faults_total,major_faults_total,logical_read_total_bytes,"
                          "logical_write_total_bytes,physical_read_total_bytes,"
                          "physical_write_total_bytes,peak_rss_avg_mib,peak_rss_max_mib,"
                          "local_volume_elements_avg,local_volume_elements_max,"
                          "volume_imbalance_max_over_avg,"
                          "volume_alltoall_wait_p95_s,volume_alltoall_wait_p99_s,volume_alltoall_wait_max_s,"
                          "volume_alltoall_p95_s,volume_alltoall_p99_s,volume_alltoall_max_s,"
                          "volume_payload_p95_s,volume_payload_p99_s,volume_payload_max_s,"
                          "volume_waitall_p95_s,volume_waitall_p99_s,volume_waitall_max_s,"
                          "volume_send_neighbors_avg,volume_send_neighbors_p95,"
                          "volume_send_neighbors_p99,volume_send_neighbors_max,"
                          "volume_receive_neighbors_avg,volume_receive_neighbors_p95,"
                          "volume_receive_neighbors_p99,volume_receive_neighbors_max,"
                          "volume_send_bytes_avg,volume_send_bytes_p95,volume_send_bytes_p99,volume_send_bytes_max,"
                          "volume_receive_bytes_avg,volume_receive_bytes_p95,volume_receive_bytes_p99,volume_receive_bytes_max,"
                          "volume_directed_edges,volume_edge_density,volume_graph_capture,core_only\n";
                output << std::setprecision(12)
                       << csv_escape(config_.experiment) << ',' << process_count_ << ','
                       << config_.repeat << ',' << timestamp << ','
                       << total_stats.average << ',' << total_stats.maximum << ','
                       << unprofiled_stats.average << ',' << unprofiled_stats.maximum << ','
                       << profile_coverage_percent << ','
                       << setup_stats.average << ',' << setup_stats.maximum << ','
                       << compute_stats.average << ',' << compute_stats.maximum << ','
                       << postprocess_stats.average << ',' << postprocess_stats.maximum << ','
                       << communication_stats.average << ',' << communication_stats.maximum << ','
                       << synchronization_stats.average << ',' << synchronization_stats.maximum << ','
                       << communication_fraction_avg << ',' << communication_fraction_max << ','
                       << arrival_wait_fraction_avg << ',' << arrival_wait_fraction_max << ','
                       << communication_plus_wait_fraction_avg << ','
                       << communication_plus_wait_fraction_max << ','
                       << io_stats.average << ',' << io_stats.maximum << ',' << io_fraction_max << ','
                       << io_available_stats.average * process_count_ << ','
                       << cache_available_stats.average * process_count_ << ','
                       << cache_references_total << ',' << cache_misses_total << ','
                       << 100.0 * safe_ratio(cache_misses_total, cache_references_total) << ','
                       << instructions_total << ',' << cycles_total << ','
                       << safe_ratio(instructions_total, cycles_total) << ','
                       << minor_faults_total << ',' << major_faults_total << ','
                       << rchar_total << ',' << wchar_total << ','
                       << read_bytes_total << ',' << write_bytes_total << ','
                       << peak_rss_stats.average << ',' << peak_rss_stats.maximum << ','
                       << local_volume_stats.average << ',' << local_volume_stats.maximum << ','
                       << safe_ratio(local_volume_stats.maximum, local_volume_stats.average) << ','
                       << percentile(volume_alltoall_wait_times, 0.95) << ','
                       << percentile(volume_alltoall_wait_times, 0.99) << ','
                       << volume_alltoall_wait_stats.maximum << ','
                       << percentile(volume_alltoall_times, 0.95) << ','
                       << percentile(volume_alltoall_times, 0.99) << ','
                       << volume_alltoall_stats.maximum << ','
                       << percentile(volume_payload_times, 0.95) << ','
                       << percentile(volume_payload_times, 0.99) << ','
                       << volume_payload_stats.maximum << ','
                       << percentile(volume_waitall_times, 0.95) << ','
                       << percentile(volume_waitall_times, 0.99) << ','
                       << volume_waitall_stats.maximum << ','
                       << volume_send_neighbor_stats.average << ','
                       << percentile(volume_send_neighbors, 0.95) << ','
                       << percentile(volume_send_neighbors, 0.99) << ','
                       << volume_send_neighbor_stats.maximum << ','
                       << volume_receive_neighbor_stats.average << ','
                       << percentile(volume_receive_neighbors, 0.95) << ','
                       << percentile(volume_receive_neighbors, 0.99) << ','
                       << volume_receive_neighbor_stats.maximum << ','
                       << volume_send_byte_stats.average << ','
                       << percentile(volume_send_bytes, 0.95) << ','
                       << percentile(volume_send_bytes, 0.99) << ','
                       << volume_send_byte_stats.maximum << ','
                       << volume_receive_byte_stats.average << ','
                       << percentile(volume_receive_bytes, 0.95) << ','
                       << percentile(volume_receive_bytes, 0.99) << ','
                       << volume_receive_byte_stats.maximum << ','
                       << volume_directed_edges << ',' << volume_edge_density << ','
                       << (config_.collect_communication_graph ? 1 : 0) << ','
                       << (config_.core_only ? 1 : 0) << '\n';
            }

            {
                std::ofstream output = open_output_file(run_directory / "metadata.txt");
                output << "experiment=" << config_.experiment << '\n';
                output << "processes=" << process_count_ << '\n';
                output << "repeat=" << config_.repeat << '\n';
                output << "timestamp=" << timestamp << '\n';
                output << "core_only=" << (config_.core_only ? "true" : "false") << '\n';
                output << "communication_graph_requested="
                       << (config_.collect_communication_graph ? "true" : "false") << '\n';
                output << "hardware_cache_requested="
                       << (config_.collect_hardware_cache ? "true" : "false") << '\n';
                output << "hardware_cache_available_ranks="
                       << cache_available_stats.average * process_count_ << '\n';
                if (!cache_error_.empty()) output << "hardware_cache_note=" << cache_error_ << '\n';
                output << "process_io_counter_available_ranks="
                       << io_available_stats.average * process_count_ << '\n';
                char mpi_version[MPI_MAX_LIBRARY_VERSION_STRING] = {};
                int mpi_version_length = 0;
                MPI_Get_library_version(mpi_version, &mpi_version_length);
                output << "mpi_library=" << std::string(mpi_version, mpi_version_length) << '\n';
                output << "compiler=" << __VERSION__ << '\n';
                for (const auto &[key, value] : metadata_) {
                    output << key << '=' << value << '\n';
                }
            }

            {
                std::ofstream output = open_output_file(run_directory / "summary.txt");
                output << "Strong Scaling Profile (强扩展分析)\n";
                output << "============================================================\n";
                output << "Experiment (实验): " << config_.experiment << '\n';
                output << "Processes (进程数): " << process_count_ << '\n';
                output << "Repeat (重复编号): " << config_.repeat << '\n';
                output << "Mode (模式): "
                       << (config_.core_only ? "core-only (仅核心算法)" : "full (完整程序)") << '\n';
                output << std::fixed << std::setprecision(6);
                output << "End-to-end max time (端到端最大时间): "
                       << total_stats.maximum << " s\n";
                output << "Profile coverage (阶段计时覆盖率): "
                       << profile_coverage_percent << "%\n";
                output << "Collective/communication execution fraction (通信执行占比, avg/max): "
                       << communication_fraction_avg << "% / "
                       << communication_fraction_max << "%\n";
                output << "Collective arrival-wait fraction (集合通信到达等待占比, avg/max): "
                       << arrival_wait_fraction_avg << "% / "
                       << arrival_wait_fraction_max << "%\n";
                output << "Communication + arrival wait (通信执行与到达等待合计, avg/max): "
                       << communication_plus_wait_fraction_avg << "% / "
                       << communication_plus_wait_fraction_max << "%\n";
                output << "I/O max fraction (最大 I/O 占比): " << io_fraction_max << "%\n";
                output << "Process I/O counters (/proc/self/io): available on "
                       << io_available_stats.average * process_count_ << '/'
                       << process_count_ << " ranks\n";
                output << "Peak RSS max (最大常驻内存): " << peak_rss_stats.maximum << " MiB\n";
                output << "Local volume imbalance max/avg (体单元负载不均衡): "
                       << safe_ratio(local_volume_stats.maximum, local_volume_stats.average) << '\n';
                if (!config_.collect_hardware_cache) {
                    output << "Hardware cache counters (硬件缓存计数器): not requested (未请求)\n";
                } else if (cache_available_stats.average == 1.0) {
                    output << "Hardware cache miss rate (硬件缓存未命中率): "
                           << 100.0 * safe_ratio(cache_misses_total, cache_references_total) << "%\n";
                    output << "IPC (每周期指令数): "
                           << safe_ratio(instructions_total, cycles_total) << '\n';
                } else {
                    output << "Hardware cache counters (硬件缓存计数器): available on "
                           << cache_available_stats.average * process_count_ << '/'
                           << process_count_ << " ranks\n";
                    if (!cache_error_.empty()) output << "Reason (原因): " << cache_error_ << '\n';
                }
                if (io_available_stats.average == 1.0) {
                    output << "Logical I/O read/write (逻辑读/写): "
                           << bytes_to_mib(rchar_total) << " / "
                           << bytes_to_mib(wchar_total) << " MiB\n";
                    output << "Physical I/O read/write (实际存储读/写): "
                           << bytes_to_mib(read_bytes_total) << " / "
                           << bytes_to_mib(write_bytes_total) << " MiB\n\n";
                } else {
                    output << "Logical/physical I/O bytes (逻辑/物理 I/O 字节): N/A\n\n";
                }

                output << "Category Breakdown (分类汇总)\n";
                output << std::left << std::setw(32) << "Category"
                       << std::right << std::setw(14) << "Avg(s)"
                       << std::setw(14) << "Max(s)"
                       << std::setw(14) << "Max/Avg" << '\n';
                output << std::string(74, '-') << '\n';
                for (const auto &[category, stats] : std::vector<std::pair<std::string, ScalarStats>>{
                         {"setup", setup_stats},
                         {"compute", compute_stats},
                         {"communication", communication_stats},
                         {"synchronization", synchronization_stats},
                         {"io", io_stats},
                         {"postprocess", postprocess_stats},
                         {"unprofiled", unprofiled_stats}}) {
                    output << std::left << std::setw(32) << category_label(category)
                           << std::right << std::setw(14) << stats.average
                           << std::setw(14) << stats.maximum
                           << std::setw(14) << safe_ratio(stats.maximum, stats.average) << '\n';
                }

                output << "\nStage Breakdown (阶段明细，按最大时间降序)\n";
                struct StageRow {
                    std::string name;
                    std::string category;
                    ScalarStats time;
                    double cache_miss_rate = 0.0;
                    double send_mib = 0.0;
                    double receive_mib = 0.0;
                };
                std::vector<StageRow> rows;
                for (std::size_t stage = 0; stage < definitions.size(); ++stage) {
                    std::vector<double> times(process_count_, 0.0);
                    double refs = 0.0, misses = 0.0, send_bytes = 0.0, receive_bytes = 0.0;
                    double calls = 0.0;
                    for (int rank = 0; rank < process_count_; ++rank) {
                        const WireStage &wire = all_stages[static_cast<std::size_t>(rank) *
                                                            definitions.size() + stage];
                        times[rank] = wire.wall_seconds;
                        calls += wire.calls;
                        refs += wire.cache_references;
                        misses += wire.cache_misses;
                        send_bytes += wire.send_bytes;
                        receive_bytes += wire.receive_bytes;
                    }
                    const ScalarStats time = summarize(times);
                    if (calls == 0.0 && time.maximum == 0.0) continue;
                    rows.push_back({definitions[stage].name,
                                    definitions[stage].category,
                                    time,
                                    100.0 * safe_ratio(misses, refs),
                                    bytes_to_mib(send_bytes),
                                    bytes_to_mib(receive_bytes)});
                }
                std::sort(rows.begin(), rows.end(), [](const StageRow &left, const StageRow &right) {
                    return left.time.maximum > right.time.maximum;
                });
                output << std::left << std::setw(30) << "Stage"
                       << std::setw(18) << "Category"
                       << std::right << std::setw(11) << "Avg(s)"
                       << std::setw(11) << "Max(s)"
                       << std::setw(10) << "M/A"
                       << std::setw(12) << "Miss(%)"
                       << std::setw(12) << "SendMiB"
                       << std::setw(12) << "RecvMiB" << '\n';
                output << std::string(116, '-') << '\n';
                for (const auto &row : rows) {
                    output << std::left << std::setw(30) << row.name
                           << std::setw(18) << row.category
                           << std::right << std::setw(11) << row.time.average
                           << std::setw(11) << row.time.maximum
                           << std::setw(10) << safe_ratio(row.time.maximum, row.time.average);
                    if (cache_available_stats.average == 1.0) {
                        output << std::setw(12) << row.cache_miss_rate;
                    } else {
                        output << std::setw(12) << "N/A";
                    }
                    output << std::setw(12) << row.send_mib
                           << std::setw(12) << row.receive_mib << '\n';
                }

                output << "\nFiles (文件说明)\n";
                output << "  summary.txt       人工阅读摘要\n";
                output << "  run_summary.csv   单次运行总指标\n";
                output << "  stages.csv        按阶段聚合指标\n";
                output << "  rank_stages.csv   各进程逐阶段原始指标\n";
                output << "  rank_metrics.csv  各进程网格规模与内存指标\n";
                output << "  metadata.txt      命令、平台和参数信息\n";
                if (config_.collect_communication_graph) {
                    output << "  communication_peers.csv        逐 rank 收发邻居集\n";
                    output << "  communication_edges.csv        逐有向边消息量核对\n";
                    output << "  communication_graph_summary.csv 通信图密度与不对称统计\n";
                }
            }

            std::cout << "[PROFILE] results written to " << run_directory.string() << std::endl;
        } catch (const std::exception &error) {
            std::cerr << "[PROFILE] failed to write results: " << error.what() << std::endl;
        }
    }

    MPI_Barrier(comm_);
}

} // namespace scaling
