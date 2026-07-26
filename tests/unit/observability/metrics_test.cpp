#include "rtmp_server/observability/metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rtmp_server::observability {
namespace {

TEST(MetricsTest, UnknownCounterDefaultsToZero) {
    Metrics metrics;
    EXPECT_EQ(metrics.counter("unknown"), 0u);
}

TEST(MetricsTest, IncrementCounterDefaultsToOnePerCall) {
    Metrics metrics;
    metrics.increment_counter("streams_created_total");
    metrics.increment_counter("streams_created_total");
    EXPECT_EQ(metrics.counter("streams_created_total"), 2u);
}

TEST(MetricsTest, IncrementCounterAcceptsAnExplicitDelta) {
    Metrics metrics;
    metrics.increment_counter("bytes_total", 1024);
    metrics.increment_counter("bytes_total", 512);
    EXPECT_EQ(metrics.counter("bytes_total"), 1536u);
}

TEST(MetricsTest, SetGaugeOverwritesThePreviousValue) {
    Metrics metrics;
    metrics.set_gauge("active_connections", 5);
    EXPECT_EQ(metrics.gauge("active_connections"), 5);
    metrics.set_gauge("active_connections", 3);
    EXPECT_EQ(metrics.gauge("active_connections"), 3);
}

TEST(MetricsTest, SnapshotsReflectAllRecordedNames) {
    Metrics metrics;
    metrics.increment_counter("a");
    metrics.increment_counter("b", 2);
    metrics.set_gauge("g", -1);

    auto counters = metrics.counters_snapshot();
    ASSERT_EQ(counters.size(), 2u);
    EXPECT_EQ(counters.at("a"), 1u);
    EXPECT_EQ(counters.at("b"), 2u);

    auto gauges = metrics.gauges_snapshot();
    ASSERT_EQ(gauges.size(), 1u);
    EXPECT_EQ(gauges.at("g"), -1);
}


// ===========================================================================
// Phase 7: declared catalog, cardinality guards, derived rates, exposition.
// ===========================================================================

TEST(MetricsCatalogTest, EveryPhase7MetricIsPresentInTheCatalog) {
    // The exact list docs/v2_promot.md PHASE 7 "Metrics" requires. Two are
    // exported under aggregated names (see below); the rest match verbatim.
    const std::vector<std::string_view> required = {
        "active_connections",        "active_publishers",       "active_viewers",
        "ingress_bytes_total",       "egress_bytes_total",      "ingress_bitrate",
        "egress_bitrate",            "outbound_queue_bytes",    "outbound_queue_packets",
        "dropped_video_frames",      "dropped_audio_frames",    "slow_viewer_recoveries",
        "slow_viewer_evictions",     "authentication_failures", "partial_send_count",
        "connection_timeouts",       "publisher_disconnects",   "viewer_disconnects",
        "gop_cache_bytes",           "gop_cache_packets",       "inter_worker_queue_depth",
        "inter_worker_queue_drops",  "io_uring_sq_full",        "io_uring_cq_overflow",
        "provided_buffer_exhaustion","recording_queue_depth",   "recording_failures",
        "process_memory_bytes",      "worker_cpu_usage",
    };

    const auto catalog = metric_catalog();
    for (const auto& name : required) {
        const bool found = std::any_of(catalog.begin(), catalog.end(),
                                       [&](const MetricDescriptor& d) { return d.name == name; });
        EXPECT_TRUE(found) << "required Phase 7 metric missing from the catalog: " << name;
    }

    // `viewers_per_stream` is exported as bounded aggregates rather than one
    // series per stream, because stream count is unbounded (see the
    // cardinality policy in metrics.hpp / docs/observability.md).
    const bool has_aggregates =
        std::any_of(catalog.begin(), catalog.end(),
                    [](const MetricDescriptor& d) { return d.name == "viewers_per_stream_max"; }) &&
        std::any_of(catalog.begin(), catalog.end(),
                    [](const MetricDescriptor& d) { return d.name == "viewers_per_stream_mean_milli"; });
    EXPECT_TRUE(has_aggregates);
}

TEST(MetricsCatalogTest, EveryCatalogNameIsUniqueAndPrometheusLegal) {
    std::set<std::string_view> seen;
    for (const auto& descriptor : metric_catalog()) {
        EXPECT_TRUE(seen.insert(descriptor.name).second) << "duplicate metric name: " << descriptor.name;
        EXPECT_TRUE(is_valid_dynamic_name(descriptor.name))
            << descriptor.name << " is not a legal metric name";
        EXPECT_FALSE(descriptor.help.empty()) << descriptor.name << " has no HELP text";
    }
}

TEST(MetricsTypedTest, CountersAccumulateAndGaugesMoveBothWays) {
    Metrics metrics;

    metrics.increment(MetricId::DroppedVideoFrames);
    metrics.increment(MetricId::DroppedVideoFrames, 9);
    EXPECT_EQ(metrics.value(MetricId::DroppedVideoFrames), 10);

    // Gauges must go down as well as up — active_viewers is decremented on
    // unsubscribe/eviction, so a counter-only implementation would be wrong.
    metrics.add(MetricId::ActiveViewers, +5);
    metrics.add(MetricId::ActiveViewers, -2);
    EXPECT_EQ(metrics.value(MetricId::ActiveViewers), 3);

    metrics.set(MetricId::ActiveViewers, 42);
    EXPECT_EQ(metrics.value(MetricId::ActiveViewers), 42);

    // Untouched metrics read zero, not garbage.
    EXPECT_EQ(metrics.value(MetricId::RecordingFailures), 0);
}

TEST(MetricsCardinalityTest, DynamicNamesCarryingAnIdentifierAreRejected) {
    Metrics metrics;

    // The exact failure mode the doc forbids: a raw connection/stream ID
    // interpolated into a metric name.
    metrics.increment_counter("connection_12345_bytes");
    metrics.increment_counter("stream_98765432_viewers");
    metrics.set_gauge("viewers_per_stream_1000", 5);

    EXPECT_EQ(metrics.counter("connection_12345_bytes"), 0u);
    EXPECT_EQ(metrics.counter("stream_98765432_viewers"), 0u);
    EXPECT_EQ(metrics.gauge("viewers_per_stream_1000"), 0);
    EXPECT_EQ(metrics.value(MetricId::MetricsRejectedNames), 3);

    // Names that are merely uppercase, dotted, or spaced are illegal too.
    metrics.increment_counter("Management.Create");
    metrics.increment_counter("has spaces");
    EXPECT_EQ(metrics.value(MetricId::MetricsRejectedNames), 5);

    // A short digit run is fine: it is a version/index, not an identifier.
    metrics.increment_counter("rtmp_v1_commands_total");
    EXPECT_EQ(metrics.counter("rtmp_v1_commands_total"), 1u);
}

TEST(MetricsCardinalityTest, TheDynamicRegistryIsBounded) {
    Metrics metrics;
    for (std::size_t i = 0; i < kMaxDynamicMetrics + 50; ++i) {
        // Distinct legal names ("aaa_bbb_..."), no digit runs.
        std::string name = "dyn_";
        std::size_t n = i;
        do {
            name += static_cast<char>('a' + (n % 26));
            n /= 26;
        } while (n > 0);
        metrics.increment_counter(name);
    }
    EXPECT_EQ(metrics.counters_snapshot().size(), kMaxDynamicMetrics);
    EXPECT_GT(metrics.value(MetricId::MetricsRejectedNames), 0);
}

TEST(MetricsWorkerTest, PerWorkerConnectionsAreBoundedByWorkerCount) {
    Metrics metrics;
    metrics.set_connections_for_worker(0, 10);
    metrics.set_connections_for_worker(3, 7);
    EXPECT_EQ(metrics.connections_for_worker(0), 10);
    EXPECT_EQ(metrics.connections_for_worker(3), 7);

    // Out-of-range worker indices are ignored, never allocated — worker count
    // is bounded by configuration, so this cannot grow.
    metrics.set_connections_for_worker(kMaxWorkers, 999);
    metrics.set_connections_for_worker(kMaxWorkers + 1000, 999);
    EXPECT_EQ(metrics.connections_for_worker(kMaxWorkers), 0);
}

TEST(MetricsAggregationTest, ViewersPerStreamIsAggregatedNotLabelled) {
    Metrics metrics;
    metrics.observe_viewers_per_stream(10);
    metrics.observe_viewers_per_stream(20);
    metrics.observe_viewers_per_stream(30);
    metrics.commit_viewers_per_stream();

    EXPECT_EQ(metrics.value(MetricId::ViewersPerStreamMax), 30);
    EXPECT_EQ(metrics.value(MetricId::ViewersPerStreamMeanMilli), 20000); // 20.000 * 1000
    EXPECT_EQ(metrics.value(MetricId::ActiveStreams), 3);

    // Committing resets the accumulator so the next window is independent.
    metrics.observe_viewers_per_stream(1);
    metrics.commit_viewers_per_stream();
    EXPECT_EQ(metrics.value(MetricId::ViewersPerStreamMax), 1);
    EXPECT_EQ(metrics.value(MetricId::ActiveStreams), 1);
}

TEST(MetricsDerivedTest, BitratesAreComputedFromTheByteCountersOverAWindow) {
    Metrics metrics;
    const auto t0 = std::chrono::steady_clock::time_point{} + std::chrono::seconds{100};

    // First sample only establishes a baseline — a rate needs two points.
    metrics.refresh_derived(t0);
    EXPECT_EQ(metrics.value(MetricId::EgressBitrate), 0);

    metrics.increment(MetricId::EgressBytesTotal, 1'000'000);
    metrics.increment(MetricId::IngressBytesTotal, 250'000);
    metrics.refresh_derived(t0 + std::chrono::seconds{2});

    EXPECT_EQ(metrics.value(MetricId::EgressBitrate), 4'000'000);  // 1 MB over 2 s
    EXPECT_EQ(metrics.value(MetricId::IngressBitrate), 1'000'000); // 250 kB over 2 s

    // A window with no traffic reports zero, not the previous rate.
    metrics.refresh_derived(t0 + std::chrono::seconds{4});
    EXPECT_EQ(metrics.value(MetricId::EgressBitrate), 0);
}

TEST(MetricsProcessTest, ResidentSetSizeIsSampledFromTheOs) {
    Metrics metrics;
    EXPECT_EQ(metrics.value(MetricId::ProcessMemoryBytes), 0);
    metrics.refresh_process_metrics();
    // A running test process always has a non-trivial RSS.
    EXPECT_GT(metrics.value(MetricId::ProcessMemoryBytes), 512 * 1024);
}

TEST(MetricsExportTest, PrometheusRenderingHasHelpTypeAndValueForEveryMetric) {
    Metrics metrics;
    metrics.increment(MetricId::SlowViewerEvictions, 3);
    metrics.set(MetricId::ActiveViewers, 17);
    metrics.set_connections_for_worker(2, 9);
    metrics.increment_counter("management_create_stream_total", 4);

    const std::string text = metrics.render_prometheus();

    EXPECT_NE(text.find("# HELP slow_viewer_evictions"), std::string::npos);
    EXPECT_NE(text.find("# TYPE slow_viewer_evictions counter"), std::string::npos);
    EXPECT_NE(text.find("\nslow_viewer_evictions 3\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE active_viewers gauge"), std::string::npos);
    EXPECT_NE(text.find("\nactive_viewers 17\n"), std::string::npos);

    // Only populated worker slots are emitted, so an 8-worker deployment does
    // not export 64 permanently-zero series.
    EXPECT_NE(text.find("connections_per_worker{worker=\"2\"} 9"), std::string::npos);
    EXPECT_EQ(text.find("connections_per_worker{worker=\"5\"}"), std::string::npos);

    EXPECT_NE(text.find("\nmanagement_create_stream_total 4\n"), std::string::npos);

    // No metric line may contain a raw identifier-looking label.
    EXPECT_EQ(text.find("connection_id="), std::string::npos);
    EXPECT_EQ(text.find("stream_key"), std::string::npos);
}

TEST(MetricsThreadingTest, ConcurrentIncrementsFromManyThreadsAreNotLost) {
    Metrics metrics;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&metrics]() {
            for (int i = 0; i < kPerThread; ++i) {
                metrics.increment(MetricId::EgressBytesTotal, 1);
                metrics.add(MetricId::ActiveViewers, +1);
                metrics.add(MetricId::ActiveViewers, -1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    // The hot path is lock-free atomics; every increment must be observed.
    EXPECT_EQ(metrics.value(MetricId::EgressBytesTotal), kThreads * kPerThread);
    EXPECT_EQ(metrics.value(MetricId::ActiveViewers), 0);
}

} // namespace
} // namespace rtmp_server::observability
