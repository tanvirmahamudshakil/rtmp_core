#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rtmp_server/loadgen/rtmp_client.hpp"

namespace rtmp_server::loadgen {

// Declarative description of one load run (docs/v2_promot.md PHASE 7
// "Required scenarios"). Everything the doc asks to be configurable is a
// field here: bitrate and keyframe interval (via MediaProfile), connection
// ramp-up, slow viewers, abrupt disconnects, and publisher reconnects.
struct ScenarioConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 1935;
    std::string application = "live";

    // Stream keys are generated as `stream_key_prefix + index`. Publisher i
    // and its viewers share the same key so fan-out actually has a source.
    std::string stream_key_prefix = "loadtest-";

    std::uint32_t publishers = 1;
    std::uint32_t viewers_per_publisher = 100;

    MediaProfile media;

    // Connection ramp-up: spread all connection attempts evenly across this
    // window instead of opening them at once. Zero means "all at once",
    // which is the "viewer connection burst" scenario.
    std::chrono::milliseconds ramp_up{5000};

    std::chrono::seconds duration{30};

    // Fraction (0..1) of viewers configured as slow readers, and the per-tick
    // read budget those viewers use.
    double slow_viewer_fraction = 0.0;
    std::size_t slow_viewer_read_budget = 4096;

    // Fraction (0..1) of viewers that abruptly disconnect (RST, no RTMP
    // teardown) once the run is halfway through.
    double abrupt_disconnect_fraction = 0.0;

    // If non-zero, every publisher abruptly drops and fully reconnects on
    // this interval, restarting its media timeline.
    std::chrono::seconds publisher_reconnect_interval{0};

    // Poll granularity. Also the media generation tick.
    std::chrono::milliseconds tick{20};
};

// Aggregate outcome of a run. All values are MEASURED, never modelled.
struct ScenarioReport {
    std::uint32_t publishers_requested = 0;
    std::uint32_t viewers_requested = 0;
    std::uint32_t publishers_streaming = 0; // reached publish acknowledgement
    std::uint32_t viewers_streaming = 0;    // reached play acknowledgement
    std::uint32_t clients_failed = 0;

    std::uint64_t total_bytes_sent = 0;
    std::uint64_t total_bytes_received = 0;
    std::uint64_t media_messages_sent = 0;
    std::uint64_t media_messages_received = 0;
    std::uint64_t keyframes_received = 0;
    std::uint64_t payloads_verified = 0;
    std::uint64_t payloads_corrupt = 0;
    std::uint64_t partial_writes = 0;
    std::uint64_t publisher_reconnects = 0;
    std::uint64_t abrupt_disconnects = 0;

    // Latency distribution across viewers, in microseconds.
    std::uint64_t connect_latency_p50_us = 0;
    std::uint64_t connect_latency_p99_us = 0;
    std::uint64_t handshake_latency_p50_us = 0;
    std::uint64_t handshake_latency_p99_us = 0;
    std::uint64_t play_latency_p50_us = 0;
    std::uint64_t play_latency_p99_us = 0;
    std::uint64_t first_media_latency_p50_us = 0;
    std::uint64_t first_media_latency_p99_us = 0;

    std::chrono::milliseconds elapsed{0};
    double egress_bitrate_bps = 0.0; // measured at the viewers
    double ingress_bitrate_bps = 0.0;

    // Distinct failure reasons observed, for diagnosis. Bounded to a handful
    // of entries so a mass failure does not produce a million-line report.
    std::vector<std::string> failure_reasons;

    [[nodiscard]] std::string to_text() const;
};

// Runs one scenario to completion against a real server listening on
// host:port. Owns all sockets; blocks the calling thread for `duration`.
//
// Concurrency model: a single thread drives every client through poll(2).
// One publisher plus a few hundred viewers on loopback is comfortably within
// one thread's budget (each tick is a poll, a bounded recv per ready fd, and
// one media generation pass per publisher), and a single thread keeps the
// measured latencies free of cross-thread scheduling noise. For viewer counts
// where one thread would become the bottleneck, run several instances of the
// tool — the report states measured throughput so a saturated generator is
// visible rather than silently mis-attributed to the server.
[[nodiscard]] ScenarioReport run_scenario(const ScenarioConfig& config);

} // namespace rtmp_server::loadgen
