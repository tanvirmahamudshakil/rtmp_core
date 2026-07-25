#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace rtmp_server::observability {

// Minimal in-process counter/gauge registry (Phase 9 "metrics",
// docs/rtmp_promot.md). Deliberately not a Prometheus client or any
// exporter — this is the same "domain logic without the transport" split
// Phase 8 used for the management API: a future HTTP `/metrics` endpoint
// would call snapshot() and format it, but standing up that endpoint needs
// the still-not-built HTTP server (see docs/control-api.md "What this phase
// deliberately does not do").
//
// Named counters/gauges are looked up by string name in a mutex-guarded
// map — simple and correct, not lock-free/per-core-sharded, because this is
// explicitly not meant to be incremented on the per-packet hot path (RTMP
// media routing has its own MediaIngest::MediaStats/RecorderStats for that);
// it's for management-API-rate and connection-lifecycle-rate events.
class Metrics {
public:
    void increment_counter(std::string_view name, std::uint64_t delta = 1);
    void set_gauge(std::string_view name, std::int64_t value);

    [[nodiscard]] std::uint64_t counter(std::string_view name) const;
    [[nodiscard]] std::int64_t gauge(std::string_view name) const;

    [[nodiscard]] std::map<std::string, std::uint64_t> counters_snapshot() const;
    [[nodiscard]] std::map<std::string, std::int64_t> gauges_snapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> counters_;
    std::map<std::string, std::int64_t> gauges_;
};

} // namespace rtmp_server::observability
