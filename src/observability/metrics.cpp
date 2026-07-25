#include "rtmp_server/observability/metrics.hpp"

namespace rtmp_server::observability {

void Metrics::increment_counter(std::string_view name, std::uint64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[std::string(name)] += delta;
}

void Metrics::set_gauge(std::string_view name, std::int64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[std::string(name)] = value;
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

} // namespace rtmp_server::observability
