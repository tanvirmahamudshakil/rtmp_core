#include "rtmp_server/observability/metrics.hpp"

#include <algorithm>
#include <format>
#include <limits>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
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

    for (const auto& [name, counter_value] : counters_snapshot()) {
        out += std::format("# TYPE {} counter\n{} {}\n", name, name, counter_value);
    }
    for (const auto& [name, gauge_value] : gauges_snapshot()) {
        out += std::format("# TYPE {} gauge\n{} {}\n", name, name, gauge_value);
    }

    return out;
}

} // namespace rtmp_server::observability
