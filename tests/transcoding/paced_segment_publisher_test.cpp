#include <gtest/gtest.h>

#ifdef RTMP_NATIVE_TRANSCODE

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "rtmp_server/hls/segment.hpp"
#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/transcoding/native/paced_segment_publisher.hpp"

namespace {

using namespace std::chrono_literals;
using rtmp_server::hls::Segment;
using rtmp_server::hls::SegmentPtr;
using rtmp_server::hls::SegmentStore;
using rtmp_server::transcoding::native::PacedSegmentPublisher;
using rtmp_server::transcoding::native::PacedSegmentPublisherConfig;

SegmentPtr make_segment(std::uint64_t sequence, std::chrono::milliseconds duration) {
    auto segment = std::make_shared<Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = duration;
    return segment;
}

bool wait_for_count(const std::shared_ptr<SegmentStore>& store, std::size_t count,
                    std::chrono::milliseconds timeout = 500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (store->segment_count() >= count) return true;
        std::this_thread::sleep_for(2ms);
    }
    return store->segment_count() >= count;
}

TEST(PacedSegmentPublisherTest, WaitsForConfiguredMediaRunwayBeforeFirstRelease) {
    auto store = std::make_shared<SegmentStore>();
    PacedSegmentPublisherConfig config;
    config.startup_buffer = 90ms;
    config.fallback_interval = 30ms;
    PacedSegmentPublisher publisher(store, config);

    publisher.push(make_segment(0, 30ms));
    publisher.push(make_segment(1, 30ms));
    std::this_thread::sleep_for(40ms);
    EXPECT_EQ(store->segment_count(), 0U);
    EXPECT_EQ(publisher.buffered_duration(), 60ms);

    publisher.push(make_segment(2, 30ms));
    EXPECT_TRUE(wait_for_count(store, 1));
    EXPECT_EQ(publisher.buffered_duration(), 60ms);
}

TEST(PacedSegmentPublisherTest, UsesActualSegmentDurationInsteadOfShortTargetHint) {
    auto store = std::make_shared<SegmentStore>();
    PacedSegmentPublisherConfig config;
    config.startup_buffer = 0ms;
    config.fallback_interval = 10ms;
    PacedSegmentPublisher publisher(store, config);

    publisher.push(make_segment(0, 100ms));
    publisher.push(make_segment(1, 100ms));
    publisher.push(make_segment(2, 100ms));
    ASSERT_TRUE(wait_for_count(store, 1));
    std::this_thread::sleep_for(45ms);
    EXPECT_EQ(store->segment_count(), 1U);
    EXPECT_TRUE(wait_for_count(store, 2, 250ms));
}

TEST(PacedSegmentPublisherTest, ReprimesAfterACompleteUpstreamUnderrun) {
    auto store = std::make_shared<SegmentStore>();
    PacedSegmentPublisherConfig config;
    config.startup_buffer = 60ms;
    config.recovery_buffer = 30ms;
    config.fallback_interval = 30ms;
    PacedSegmentPublisher publisher(store, config);

    publisher.push(make_segment(0, 30ms));
    publisher.push(make_segment(1, 30ms));
    ASSERT_TRUE(wait_for_count(store, 2));

    publisher.push(make_segment(2, 30ms));
    EXPECT_TRUE(wait_for_count(store, 3));
}

TEST(PacedSegmentPublisherTest, RecoveryDoesNotWaitForTheFullColdStartRunway) {
    auto store = std::make_shared<SegmentStore>();
    PacedSegmentPublisherConfig config;
    config.startup_buffer = 90ms;
    config.recovery_buffer = 30ms;
    config.fallback_interval = 30ms;
    PacedSegmentPublisher publisher(store, config);

    publisher.push(make_segment(0, 30ms));
    publisher.push(make_segment(1, 30ms));
    publisher.push(make_segment(2, 30ms));
    ASSERT_TRUE(wait_for_count(store, 3));

    publisher.push(make_segment(3, 30ms));
    EXPECT_TRUE(wait_for_count(store, 4));
}

// A source that hands over more media than real time (a deep upstream window,
// a catch-up burst after a reconnect) used to leave that surplus queued
// forever: every viewer then watched the stream that far behind the live edge
// for the life of the job. Past max_buffer the publisher releases slightly
// faster than real time until the backlog is gone.
TEST(PacedSegmentPublisherTest, DrainsFasterThanRealTimeWhileOverTheBufferBound) {
    auto store = std::make_shared<SegmentStore>();
    PacedSegmentPublisherConfig config;
    config.startup_buffer = 0ms;
    config.recovery_buffer = 0ms;
    config.fallback_interval = 20ms;
    config.max_buffer = 60ms;
    config.drain_ratio = 0.5;
    PacedSegmentPublisher publisher(store, config);

    // 360ms of media queued against a 60ms bound: every release below is
    // taken while the backlog is still over it.
    for (std::uint64_t i = 0; i < 6; ++i) publisher.push(make_segment(i, 60ms));

    // Real-time pacing would need ~240ms to reach the fourth segment; the
    // halved interval reaches it in ~120ms.
    EXPECT_TRUE(wait_for_count(store, 4, 200ms));
    // ...but it is still pacing, not flushing the queue in one burst.
    EXPECT_LT(store->segment_count(), 6U);
}

// The drain bound must never undercut the runway the publisher is deliberately
// filling, or priming would read as a backlog and defeat itself.
TEST(PacedSegmentPublisherTest, BufferBoundIsRaisedToAtLeastTheStartupRunway) {
    auto store = std::make_shared<SegmentStore>();
    PacedSegmentPublisherConfig config;
    config.startup_buffer = 90ms;
    config.fallback_interval = 30ms;
    config.max_buffer = 10ms; // below the runway: clamped up in the constructor
    PacedSegmentPublisher publisher(store, config);

    publisher.push(make_segment(0, 30ms));
    publisher.push(make_segment(1, 30ms));
    std::this_thread::sleep_for(40ms);
    EXPECT_EQ(store->segment_count(), 0U); // still priming, not draining

    publisher.push(make_segment(2, 30ms));
    EXPECT_TRUE(wait_for_count(store, 1));
}

} // namespace

#endif // RTMP_NATIVE_TRANSCODE
