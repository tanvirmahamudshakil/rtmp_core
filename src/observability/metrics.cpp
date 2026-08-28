#include "rtmp_server/observability/metrics.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#endif

namespace rtmp_server::observability {
namespace {

constexpr std::array<MetricDescriptor, static_cast<std::size_t>(MetricId::kCount)> kCatalog = {{
#define RTMP_SERVER_METRIC_DESCRIPTOR(enumerator, name, kind, help) \
    MetricDescriptor{MetricId::enumerator, name, MetricKind::kind, help},
    RTMP_SERVER_METRIC_TABLE(RTMP_SERVER_METRIC_DESCRIPTOR)
#undef RTMP_SERVER_METRIC_DESCRIPTOR
}};

// Resident set size in bytes, or 0 if unavailable on this platform.
std::int64_t query_resident_set_bytes() noexcept {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::int64_t>(info.resident_size);
#elif defined(__linux__)
    // /proc/self/statm field 2 is the resident page count.
    std::FILE* file = std::fopen("/proc/self/statm", "re");
    if (file == nullptr) return 0;
    long long total_pages = 0;
    long long resident_pages = 0;
    const int scanned = std::fscanf(file, "%lld %lld", &total_pages, &resident_pages);
    std::fclose(file);
    if (scanned != 2 || resident_pages < 0) return 0;
    return static_cast<std::int64_t>(resident_pages) * static_cast<std::int64_t>(::sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

// Total CPU time (user+system) this process has consumed so far, in clock
// ticks, or -1 if unavailable (platform not supported, or /proc unreadable —
// e.g. a restricted container). refresh_process_metrics() turns two samples
// of this into a milli-cores rate; a single sample is meaningless.
std::int64_t query_process_cpu_ticks() noexcept {
#if defined(__linux__)
    std::FILE* file = std::fopen("/proc/self/stat", "re");
    if (file == nullptr) return -1;
    char buffer[512];
    const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (n == 0) return -1;
    buffer[n] = '\0';
    // Field 2 (comm) may itself contain spaces/parens, so resume parsing
    // after the last ')' rather than counting space-separated fields from
    // the start of the line.
    const char* rparen = std::strrchr(buffer, ')');
    if (rparen == nullptr) return -1;
    char state = '\0';
    long long ppid = 0, pgrp = 0, session = 0, tty_nr = 0, tpgid = 0, flags = 0;
    long long minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0, utime = 0, stime = 0;
    const int matched = std::sscanf(rparen + 1,
                                     " %c %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld", &state,
                                     &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags, &minflt, &cminflt, &majflt,
                                     &cmajflt, &utime, &stime);
    if (matched != 13) return -1;
    return utime + stime;
#else
    return -1;
#endif
}

// Per-logical-CPU cumulative counters from Linux's scheduler accounting.
// Each row is {cpu id, active jiffies, total jiffies}. guest/guest_nice are
// intentionally excluded because Linux already includes them in user/nice;
// adding them again is a common source of inflated "CPU percent" readings.
std::vector<std::array<std::uint64_t, 3>> query_system_cpu_times() noexcept {
    std::vector<std::array<std::uint64_t, 3>> out;
#if defined(__linux__)
    cpu_set_t allowed{};
    CPU_ZERO(&allowed);
    const bool have_affinity = ::sched_getaffinity(0, sizeof(allowed), &allowed) == 0;

    std::FILE* file = std::fopen("/proc/stat", "re");
    if (file == nullptr) return out;
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "cpu", 3) != 0) break;
        if (line[3] < '0' || line[3] > '9') continue; // skip aggregate "cpu " row

        unsigned core = 0;
        unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
        unsigned long long irq = 0, softirq = 0, steal = 0, guest = 0, guest_nice = 0;
        const int matched =
            std::sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &core, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal,
                        &guest, &guest_nice);
        if (matched < 5) continue;
        if (have_affinity && (core >= CPU_SETSIZE || !CPU_ISSET(core, &allowed))) continue;

        const std::uint64_t active = user + nice + system + irq + softirq + steal;
        const std::uint64_t total = active + idle + iowait;
        out.push_back({core, active, total});
    }
    std::fclose(file);
#endif
    return out;
}

} // namespace

std::span<const MetricDescriptor> metric_catalog() noexcept { return kCatalog; }

std::string_view metric_name(MetricId id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kCatalog.size()) return "invalid_metric";
    return kCatalog[index].name;
}

bool is_valid_dynamic_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 96) return false;

    std::size_t digit_run = 0;
    for (const char c : name) {
        const bool is_lower = c >= 'a' && c <= 'z';
        const bool is_digit = c >= '0' && c <= '9';
        const bool is_sep = c == '_' || c == ':';
        if (!is_lower && !is_digit && !is_sep) return false;

        // A run of 4+ digits is the signature of an interpolated identifier
        // (connection ID, stream ID, port, timestamp) leaking into a name.
        digit_run = is_digit ? digit_run + 1 : 0;
        if (digit_run >= 4) return false;
    }
    // A leading digit or separator is not a legal Prometheus metric name.
    return name.front() >= 'a' && name.front() <= 'z';
}

Metrics::Metrics() {
    for (auto& slot : slots_) slot.store(0, std::memory_order_relaxed);
    for (auto& slot : connections_per_worker_) slot.store(0, std::memory_order_relaxed);
    last_system_cpu_times_ = query_system_cpu_times();
    const auto hardware_cores = std::thread::hardware_concurrency();
    const auto visible_cores = !last_system_cpu_times_.empty()
                                   ? last_system_cpu_times_.size()
                                   : static_cast<std::size_t>(hardware_cores > 0 ? hardware_cores : 1);
    set(MetricId::CpuCoresAvailable, static_cast<std::int64_t>(visible_cores));
    cpu_core_usage_.reserve(last_system_cpu_times_.size());
    for (const auto& sample : last_system_cpu_times_) {
        cpu_core_usage_.push_back(CpuCoreUsage{static_cast<std::uint32_t>(sample[0]), 0});
    }
    if (!last_system_cpu_times_.empty()) {
        last_system_cpu_sample_ = std::chrono::steady_clock::now();
    }

    const auto ticks = query_process_cpu_ticks();
    if (ticks >= 0) {
        has_cpu_baseline_ = true;
        last_cpu_sample_ = std::chrono::steady_clock::now();
        last_cpu_ticks_ = ticks;
    }
}

void Metrics::increment(MetricId id, std::uint64_t delta) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kSlots) return;
    // Counters export as int64; a delta that large is a caller bug, so clamp
    // rather than wrap into a negative reading.
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto clamped = static_cast<std::int64_t>(std::min<std::uint64_t>(delta, kMax));
    slots_[index].fetch_add(clamped, std::memory_order_relaxed);
}

void Metrics::set(MetricId id, std::int64_t value) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kSlots) return;
    slots_[index].store(value, std::memory_order_relaxed);
}

void Metrics::add(MetricId id, std::int64_t delta) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kSlots) return;
    slots_[index].fetch_add(delta, std::memory_order_relaxed);
}

std::int64_t Metrics::value(MetricId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kSlots) return 0;
    return slots_[index].load(std::memory_order_relaxed);
}

void Metrics::set_connections_for_worker(std::size_t worker_index, std::int64_t connections) noexcept {
    if (worker_index >= kMaxWorkers) return; // bounded by config, never grow
    connections_per_worker_[worker_index].store(connections, std::memory_order_relaxed);
}

std::int64_t Metrics::connections_for_worker(std::size_t worker_index) const noexcept {
    if (worker_index >= kMaxWorkers) return 0;
    return connections_per_worker_[worker_index].load(std::memory_order_relaxed);
}

std::vector<Metrics::CpuCoreUsage> Metrics::cpu_core_usage_snapshot() const {
    std::lock_guard<std::mutex> lock(cpu_mutex_);
    return cpu_core_usage_;
}

void Metrics::observe_viewers_per_stream(std::int64_t viewers) noexcept {
    std::lock_guard<std::mutex> lock(viewers_mutex_);
    viewers_sample_max_ = std::max(viewers_sample_max_, viewers);
    viewers_sample_sum_ += viewers;
    ++viewers_sample_count_;
}

void Metrics::commit_viewers_per_stream() noexcept {
    std::int64_t max_viewers = 0;
    std::int64_t mean_milli = 0;
    std::int64_t streams = 0;
    {
        std::lock_guard<std::mutex> lock(viewers_mutex_);
        max_viewers = viewers_sample_max_;
        streams = viewers_sample_count_;
        if (viewers_sample_count_ > 0) {
            mean_milli = (viewers_sample_sum_ * 1000) / viewers_sample_count_;
        }
        viewers_sample_max_ = 0;
        viewers_sample_sum_ = 0;
        viewers_sample_count_ = 0;
    }
    set(MetricId::ViewersPerStreamMax, max_viewers);
    set(MetricId::ViewersPerStreamMeanMilli, mean_milli);
    set(MetricId::ActiveStreams, streams);
}

void Metrics::refresh_derived(std::chrono::steady_clock::time_point now) noexcept {
    const std::int64_t ingress = value(MetricId::IngressBytesTotal);
    const std::int64_t egress = value(MetricId::EgressBytesTotal);

    std::lock_guard<std::mutex> lock(derived_mutex_);
    if (!has_rate_baseline_) {
        has_rate_baseline_ = true;
        last_rate_sample_ = now;
        last_ingress_bytes_ = ingress;
        last_egress_bytes_ = egress;
        return; // a rate needs two samples
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_rate_sample_).count();
    if (elapsed <= 0.0) return;

    const auto bits_per_second = [elapsed](std::int64_t delta_bytes) {
        return static_cast<std::int64_t>((static_cast<double>(delta_bytes) * 8.0) / elapsed);
    };

    set(MetricId::IngressBitrate, bits_per_second(ingress - last_ingress_bytes_));
    set(MetricId::EgressBitrate, bits_per_second(egress - last_egress_bytes_));

    last_rate_sample_ = now;
    last_ingress_bytes_ = ingress;
    last_egress_bytes_ = egress;
}

void Metrics::refresh_process_metrics() noexcept {
    const std::int64_t rss = query_resident_set_bytes();
    if (rss > 0) set(MetricId::ProcessMemoryBytes, rss);

    const std::int64_t ticks = query_process_cpu_ticks();
    auto system_cpu_times = query_system_cpu_times();
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(cpu_mutex_);
    if (ticks >= 0) {
        if (!has_cpu_baseline_) {
            has_cpu_baseline_ = true;
            last_cpu_sample_ = now;
            last_cpu_ticks_ = ticks;
        } else {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::duration<double>>(now - last_cpu_sample_).count();
            // Near-simultaneous Prometheus/dashboard scrapes do not contain
            // enough scheduler ticks for a stable rate. Preserve the last
            // real sample instead of replacing it with a 0/100% spike.
            if (elapsed >= 0.25) {
#if defined(__linux__)
                const long raw_hz = ::sysconf(_SC_CLK_TCK);
#else
                const long raw_hz = 100;
#endif
                const double ticks_per_second = raw_hz > 0 ? static_cast<double>(raw_hz) : 100.0;
                const double cpu_seconds =
                    static_cast<double>(std::max<std::int64_t>(0, ticks - last_cpu_ticks_)) /
                    ticks_per_second;
                const double milli_cores = (cpu_seconds / elapsed) * 1000.0;
                set(MetricId::WorkerCpuUsage,
                    static_cast<std::int64_t>(std::max(0.0, milli_cores)));
                last_cpu_sample_ = now;
                last_cpu_ticks_ = ticks;
            }
        }
    }

    if (system_cpu_times.empty()) return;
    if (last_system_cpu_sample_ != std::chrono::steady_clock::time_point{} &&
        now - last_system_cpu_sample_ < std::chrono::milliseconds(250)) {
        return;
    }
    if (last_system_cpu_times_.empty()) {
        last_system_cpu_times_ = std::move(system_cpu_times);
        cpu_core_usage_.clear();
        for (const auto& sample : last_system_cpu_times_) {
            cpu_core_usage_.push_back(CpuCoreUsage{static_cast<std::uint32_t>(sample[0]), 0});
        }
        set(MetricId::CpuCoresAvailable,
            static_cast<std::int64_t>(last_system_cpu_times_.size()));
        last_system_cpu_sample_ = now;
        return;
    }

    std::vector<CpuCoreUsage> next_usage;
    next_usage.reserve(system_cpu_times.size());
    std::uint64_t aggregate_active = 0;
    std::uint64_t aggregate_total = 0;
    for (const auto& current : system_cpu_times) {
        const auto previous = std::lower_bound(
            last_system_cpu_times_.begin(), last_system_cpu_times_.end(), current[0],
            [](const auto& row, std::uint64_t core) { return row[0] < core; });
        std::int64_t milli_percent = 0;
        if (previous != last_system_cpu_times_.end() && (*previous)[0] == current[0] &&
            current[1] >= (*previous)[1] && current[2] >= (*previous)[2]) {
            const auto delta_active = current[1] - (*previous)[1];
            const auto delta_total = current[2] - (*previous)[2];
            if (delta_total > 0) {
                milli_percent = static_cast<std::int64_t>(
                    std::min<std::uint64_t>(100'000, (delta_active * 100'000 + delta_total / 2) /
                                                        delta_total));
                aggregate_active += delta_active;
                aggregate_total += delta_total;
            } else {
                const auto old_usage = std::find_if(
                    cpu_core_usage_.begin(), cpu_core_usage_.end(),
                    [&](const CpuCoreUsage& value) { return value.core == current[0]; });
                if (old_usage != cpu_core_usage_.end()) milli_percent = old_usage->milli_percent;
            }
        }
        next_usage.push_back(
            CpuCoreUsage{static_cast<std::uint32_t>(current[0]), milli_percent});
    }
    if (aggregate_total > 0) {
        set(MetricId::SystemCpuUsageMilliPercent,
            static_cast<std::int64_t>(
                std::min<std::uint64_t>(100'000, (aggregate_active * 100'000 + aggregate_total / 2) /
                                                    aggregate_total)));
    }
    set(MetricId::CpuCoresAvailable, static_cast<std::int64_t>(system_cpu_times.size()));
    cpu_core_usage_ = std::move(next_usage);
    last_system_cpu_times_ = std::move(system_cpu_times);
    last_system_cpu_sample_ = now;
}

void Metrics::increment_counter(std::string_view name, std::uint64_t delta) {
    if (!is_valid_dynamic_name(name)) {
        increment(MetricId::MetricsRejectedNames);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(std::string(name));
    if (it == counters_.end()) {
        if (counters_.size() >= kMaxDynamicMetrics) {
            // Bounded: refuse new names rather than let the registry grow.
            slots_[static_cast<std::size_t>(MetricId::MetricsRejectedNames)].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        counters_.emplace(std::string(name), delta);
        return;
    }
    it->second += delta;
}

void Metrics::set_gauge(std::string_view name, std::int64_t value) {
    if (!is_valid_dynamic_name(name)) {
        increment(MetricId::MetricsRejectedNames);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gauges_.find(std::string(name));
    if (it == gauges_.end()) {
        if (gauges_.size() >= kMaxDynamicMetrics) {
            slots_[static_cast<std::size_t>(MetricId::MetricsRejectedNames)].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        gauges_.emplace(std::string(name), value);
        return;
    }
    it->second = value;
}

std::uint64_t Metrics::counter(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = counters_.find(std::string(name));
    return it == counters_.end() ? 0 : it->second;
}

std::int64_t Metrics::gauge(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gauges_.find(std::string(name));
    return it == gauges_.end() ? 0 : it->second;
}

std::map<std::string, std::uint64_t> Metrics::counters_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

std::map<std::string, std::int64_t> Metrics::gauges_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gauges_;
}

std::vector<Metrics::Sample> Metrics::snapshot() const {
    std::vector<Sample> out;
    out.reserve(kSlots);
    for (const auto& descriptor : kCatalog) {
        out.push_back(Sample{descriptor.name, descriptor.kind, value(descriptor.id)});
    }
    return out;
}

std::string Metrics::render_prometheus() const {
    std::string out;
    out.reserve(8192);

    for (const auto& descriptor : kCatalog) {
        out += std::format("# HELP {} {}\n", descriptor.name, descriptor.help);
        out += std::format("# TYPE {} {}\n", descriptor.name,
                           descriptor.kind == MetricKind::Counter ? "counter" : "gauge");
        out += std::format("{} {}\n", descriptor.name, value(descriptor.id));
    }

    out += "# HELP connections_per_worker Connections owned by each event-loop worker\n";
    out += "# TYPE connections_per_worker gauge\n";
    for (std::size_t i = 0; i < kMaxWorkers; ++i) {
        const std::int64_t connections = connections_for_worker(i);
        // Only emit worker slots that have ever been populated, so an
        // 8-worker deployment does not export 64 permanently-zero series.
        if (connections != 0) {
            out += std::format("connections_per_worker{{worker=\"{}\"}} {}\n", i, connections);
        }
    }

    out +=
        "# HELP system_cpu_core_usage_milli_percent Per-logical-core CPU utilisation since "
        "the previous sample, in thousandths of a percent (100000 = 100%)\n";
    out += "# TYPE system_cpu_core_usage_milli_percent gauge\n";
    for (const auto& core : cpu_core_usage_snapshot()) {
        out += std::format("system_cpu_core_usage_milli_percent{{core=\"{}\"}} {}\n",
                           core.core, core.milli_percent);
    }

    for (const auto& [name, counter_value] : counters_snapshot()) {
        out += std::format("# TYPE {} counter\n{} {}\n", name, name, counter_value);
    }
    for (const auto& [name, gauge_value] : gauges_snapshot()) {
        out += std::format("# TYPE {} gauge\n{} {}\n", name, name, gauge_value);
    }

    return out;
}

} // namespace rtmp_server::observability
