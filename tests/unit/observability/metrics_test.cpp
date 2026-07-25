#include "rtmp_server/observability/metrics.hpp"

#include <gtest/gtest.h>

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

} // namespace
} // namespace rtmp_server::observability
