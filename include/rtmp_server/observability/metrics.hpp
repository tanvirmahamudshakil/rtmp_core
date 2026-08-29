#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtmp_server::observability {

// Phase 7 metric registry (docs/v2_promot.md PHASE 7 "Metrics").
//
// Two access paths, deliberately:
//
//  1. The *declared catalog* (MetricId + RTMP_SERVER_METRIC_TABLE below).
//     Every metric Phase 7 requires is a compile-time enumerator backed by
//     one slot in a fixed std::array<std::atomic<std::int64_t>>. Increments
//     are lock-free and O(1) with no string hashing, so this path is safe to
//     call from the per-packet media hot path and from io_uring completion
//     handlers. Because the set is fixed at compile time it is structurally
//     impossible to blow up cardinality here.
//
//  2. The *dynamic* string-keyed path (increment_counter/set_gauge), kept
//     for backwards compatibility with Phase 5's StreamManager and
//     ManagementApi. This path takes a mutex and is explicitly NOT for the
//     hot path. It is now guarded: names are validated (see
//     is_valid_dynamic_name) and the map is bounded by kMaxDynamicMetrics,
//     so a caller that accidentally interpolates a connection ID into a
//     metric name cannot grow the registry without bound. Rejected writes
//     bump `metrics_rejected_names_total` instead of being silently lost.
//
// High-cardinality policy (doc: "Avoid high-cardinality metric labels such
// as raw connection IDs"):
//   * No metric in the catalog is labelled by connection ID, stream ID,
//     stream key, subscriber ID or remote address.
//   * `viewers_per_stream` is unbounded in the number of streams, so it is
//     NOT exposed per stream. It is exposed as the bounded aggregate gauges
//     viewers_per_stream_max / viewers_per_stream_mean_milli, fed by
//     observe_viewers_per_stream().
//   * `connections_per_worker` IS per-worker-labelled, because worker count
//     is bounded by configuration (kMaxWorkers), not by client behaviour.

// X-macro catalog: (enumerator, exported name, kind, help text).
// Kind is Counter (monotonic, never decreases) or Gauge (can go up/down).
#define RTMP_SERVER_METRIC_TABLE(X)                                                                                  \
    /* -- connection/session gauges -------------------------------------- */                                        \
    X(ActiveConnections, "active_connections", Gauge, "Currently established RTMP connections")                       \
    X(ActivePublishers, "active_publishers", Gauge, "Currently publishing sessions")                                  \
    X(ActiveViewers, "active_viewers", Gauge, "Currently subscribed playback sessions")                               \
    X(ViewersPerStreamMax, "viewers_per_stream_max", Gauge, "Largest per-stream viewer count observed in the last "   \
                                                            "observation window")                                     \
    X(ViewersPerStreamMeanMilli, "viewers_per_stream_mean_milli", Gauge,                                              \
      "Mean viewers per active stream, scaled by 1000 to keep an integer gauge")                                      \
    X(ActiveStreams, "active_streams", Gauge, "Streams with at least one publisher or viewer")                        \
    /* -- byte/bitrate counters ------------------------------------------ */                                        \
    X(IngressBytesTotal, "ingress_bytes_total", Counter, "Total bytes read from publisher sockets")                   \
    X(EgressBytesTotal, "egress_bytes_total", Counter, "Total media payload bytes delivered to viewer transports")   \
    X(IngressBitrate, "ingress_bitrate", Gauge, "Ingress bits/sec over the last refresh_derived() window")            \
    X(EgressBitrate, "egress_bitrate", Gauge, "Egress bits/sec over the last refresh_derived() window")               \
    /* -- backpressure ---------------------------------------------------- */                                       \
    X(OutboundQueueBytes, "outbound_queue_bytes", Gauge, "Bytes currently queued across all viewer outbound queues")  \
    X(OutboundQueuePackets, "outbound_queue_packets", Gauge, "Packets currently queued across all viewer queues")     \
    X(DroppedVideoFrames, "dropped_video_frames", Counter, "Video frames dropped by the slow-viewer policy")          \
    X(DroppedAudioFrames, "dropped_audio_frames", Counter, "Audio frames dropped by the slow-viewer policy")          \
    X(SlowViewerRecoveries, "slow_viewer_recoveries", Counter, "Viewers that resumed after a WaitingForKeyframe "     \
                                                               "stall")                                               \
    X(SlowViewerEvictions, "slow_viewer_evictions", Counter, "Viewers forcibly unsubscribed for falling behind")      \
    /* -- security/lifecycle ---------------------------------------------- */                                       \
    X(AuthenticationFailures, "authentication_failures", Counter, "Rejected publish/play/management credentials")     \
    X(PartialSendCount, "partial_send_count", Counter, "Socket writes that transmitted fewer bytes than requested")   \
    X(ConnectionTimeouts, "connection_timeouts", Counter, "Connections closed for idle/handshake/publisher timeout")  \
    X(PublisherDisconnects, "publisher_disconnects", Counter, "Publisher sessions that ended, for any reason")        \
    X(ViewerDisconnects, "viewer_disconnects", Counter, "Viewer sessions that ended, for any reason")                 \
    /* -- GOP cache -------------------------------------------------------- */                                      \
    X(GopCacheBytes, "gop_cache_bytes", Gauge, "Bytes currently held across all per-stream GOP caches")               \
    X(GopCachePackets, "gop_cache_packets", Gauge, "Frames currently held across all per-stream GOP caches")          \
    /* -- multi-worker routing (Phase 4) ------------------------------------ */                                     \
    X(InterWorkerQueueDepth, "inter_worker_queue_depth", Gauge, "Frames queued across all cross-worker inboxes")      \
    X(InterWorkerQueueDrops, "inter_worker_queue_drops", Counter, "Frames dropped because a worker inbox was full")   \
    /* -- io_uring transport (Linux-only call sites) ------------------------ */                                     \
    X(IoUringSqFull, "io_uring_sq_full", Counter, "Submission-queue-full events observed by an event loop")           \
    X(IoUringCqOverflow, "io_uring_cq_overflow", Counter, "Completion-queue overflow events reported by the kernel")  \
    X(ProvidedBufferExhaustion, "provided_buffer_exhaustion", Counter, "Reads that found the provided buffer ring "   \
                                                                       "empty")                                       \
    /* -- recording (Phase 6) ------------------------------------------------ */                                    \
    X(RecordingQueueDepth, "recording_queue_depth", Gauge, "Items queued in the async recording sink")                \
    X(RecordingFailures, "recording_failures", Counter, "Recording write/rotate/finalize failures")                   \
    /* -- process ------------------------------------------------------------ */                                    \
    X(ProcessMemoryBytes, "process_memory_bytes", Gauge, "Resident set size of this process")                         \
    X(WorkerCpuUsage, "worker_cpu_usage", Gauge, "This process's CPU utilisation since the previous sample, in "     \
                                                 "milli-cores (1000 = 1 core fully busy)")                            \
    X(SystemCpuUsageMilliPercent, "system_cpu_usage_milli_percent", Gauge,                                           \
      "CPU utilisation across the logical cores available to this process, in thousandths of a percent "             \
      "(100000 = 100%)")                                                                                            \
    X(CpuCoresAvailable, "cpu_cores_available", Gauge, "Logical CPU cores available to this process "                \
                                                        "(Linux affinity mask when available)")                       \
    /* -- registry self-observability ---------------------------------------- */                                    \
    X(MetricsRejectedNames, "metrics_rejected_names_total", Counter,                                                  \
      "Dynamic metric writes rejected for an invalid or over-budget name")

enum class MetricId : std::uint16_t {
#define RTMP_SERVER_METRIC_ENUMERATOR(enumerator, name, kind, help) enumerator,
    RTMP_SERVER_METRIC_TABLE(RTMP_SERVER_METRIC_ENUMERATOR)
#undef RTMP_SERVER_METRIC_ENUMERATOR
        kCount
};

enum class MetricKind : std::uint8_t { Counter, Gauge };

struct MetricDescriptor {
    MetricId id;
    std::string_view name;
    MetricKind kind;
    std::string_view help;
};

// The full declared catalog, indexed by static_cast<std::size_t>(MetricId).
[[nodiscard]] std::span<const MetricDescriptor> metric_catalog() noexcept;

[[nodiscard]] std::string_view metric_name(MetricId id) noexcept;

// Upper bound on worker slots for the per-worker `connections_per_worker`
// gauge. Bounded by configuration, never by remote input, so labelling by
// worker index does not create unbounded cardinality.
// Upper bound only for the per-worker metrics array below; the server runs
// one worker per logical core, so this must stay >= the largest host it can
// be deployed on. 256 covers current high-core-count servers; a machine with
// more cores still runs correctly, only its highest worker indices are
// omitted from the per-worker connection gauge.
inline constexpr std::size_t kMaxWorkers = 256;

// Upper bound on distinct names the dynamic string-keyed path will hold.
inline constexpr std::size_t kMaxDynamicMetrics = 256;

// A dynamic metric name is accepted only if it is non-empty, at most 96
// chars, made of [a-z0-9_:] and contains no run of 4+ digits (the shape an
// interpolated connection/stream ID takes). This is the mechanical guard
// behind the "no raw connection IDs as labels" rule.
[[nodiscard]] bool is_valid_dynamic_name(std::string_view name) noexcept;

class Metrics {
public:
    Metrics();

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // ---- declared catalog: lock-free, hot-path safe ----------------------

    // Counters only ever move forward; `delta` is unsigned by construction.
    void increment(MetricId id, std::uint64_t delta = 1) noexcept;

    // Gauges: absolute set, or signed delta (for +1/-1 on active_* counts).
    void set(MetricId id, std::int64_t value) noexcept;
    void add(MetricId id, std::int64_t delta) noexcept;

    [[nodiscard]] std::int64_t value(MetricId id) const noexcept;

    // ---- bounded-cardinality per-worker gauge -----------------------------

    // Ignores worker indices >= kMaxWorkers rather than growing.
    void set_connections_for_worker(std::size_t worker_index, std::int64_t connections) noexcept;
    [[nodiscard]] std::int64_t connections_for_worker(std::size_t worker_index) const noexcept;

    struct CpuCoreUsage {
        std::uint32_t core = 0;
        std::int64_t milli_percent = 0;
    };

    // One bounded sample per logical CPU visible to this process. Linux data
    // comes from consecutive /proc/stat counters, filtered through the
    // process affinity mask, so these are kernel measurements rather than a
    // frontend estimate. Empty on platforms where those counters are absent.
    [[nodiscard]] std::vector<CpuCoreUsage> cpu_core_usage_snapshot() const;

    // ---- viewers-per-stream aggregation ------------------------------------

    // Feed one sample per active stream, then call
    // commit_viewers_per_stream() to publish max/mean. Aggregating instead of
    // labelling is what keeps stream count out of the metric cardinality.
    void observe_viewers_per_stream(std::int64_t viewers) noexcept;
    void commit_viewers_per_stream() noexcept;

    // ---- derived rates ------------------------------------------------------

    // Recomputes ingress_bitrate/egress_bitrate from the byte counters and
    // the wall time since the previous call. Intended to be called from a
    // low-frequency timer or from the /metrics handler, never per packet.
    void refresh_derived(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;

    // Samples process_memory_bytes (RSS) from the OS. Best-effort: leaves the
    // gauge untouched if the platform query fails.
    void refresh_process_metrics() noexcept;

    // ---- dynamic (legacy, mutex-guarded, NOT hot-path) -----------------------

    void increment_counter(std::string_view name, std::uint64_t delta = 1);
    void set_gauge(std::string_view name, std::int64_t value);

    [[nodiscard]] std::uint64_t counter(std::string_view name) const;
    [[nodiscard]] std::int64_t gauge(std::string_view name) const;

    [[nodiscard]] std::map<std::string, std::uint64_t> counters_snapshot() const;
    [[nodiscard]] std::map<std::string, std::int64_t> gauges_snapshot() const;

    // ---- export --------------------------------------------------------------

    struct Sample {
        std::string_view name;
        MetricKind kind;
        std::int64_t value;
    };

    // Declared catalog only, in catalog order.
    [[nodiscard]] std::vector<Sample> snapshot() const;

    // Prometheus text exposition (v0.0.4) of the declared catalog, the
    // per-worker gauge and the dynamic map.
    [[nodiscard]] std::string render_prometheus() const;

private:
    static constexpr std::size_t kSlots = static_cast<std::size_t>(MetricId::kCount);

    std::array<std::atomic<std::int64_t>, kSlots> slots_;
    std::array<std::atomic<std::int64_t>, kMaxWorkers> connections_per_worker_;

    // Viewers-per-stream accumulation. Guarded by its own mutex because it is
    // a multi-value commit, and it is called from a periodic sampler rather
    // than the packet path.
    mutable std::mutex viewers_mutex_;
    std::int64_t viewers_sample_max_ = 0;
    std::int64_t viewers_sample_sum_ = 0;
    std::int64_t viewers_sample_count_ = 0;

    mutable std::mutex derived_mutex_;
    std::chrono::steady_clock::time_point last_rate_sample_{};
    std::int64_t last_ingress_bytes_ = 0;
    std::int64_t last_egress_bytes_ = 0;
    bool has_rate_baseline_ = false;

    mutable std::mutex cpu_mutex_;
    std::chrono::steady_clock::time_point last_cpu_sample_{};
    std::int64_t last_cpu_ticks_ = 0;
    bool has_cpu_baseline_ = false;
    // {logical CPU id, active jiffies, total jiffies}; only touched while
    // cpu_mutex_ is held.
    std::vector<std::array<std::uint64_t, 3>> last_system_cpu_times_;
    std::vector<CpuCoreUsage> cpu_core_usage_;
    std::chrono::steady_clock::time_point last_system_cpu_sample_{};

    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> counters_;
    std::map<std::string, std::int64_t> gauges_;
};

} // namespace rtmp_server::observability
