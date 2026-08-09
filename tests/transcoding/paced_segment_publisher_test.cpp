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

} // namespace

#endif // RTMP_NATIVE_TRANSCODE
