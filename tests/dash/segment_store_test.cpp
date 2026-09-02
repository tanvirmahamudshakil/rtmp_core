#include <gtest/gtest.h>

#include "rtmp_server/dash/segment_store.hpp"

using namespace rtmp_server;
using namespace rtmp_server::dash;

namespace {

SegmentPtr make_segment(std::uint64_t number, std::size_t bytes = 100) {
    auto segment = std::make_shared<Segment>();
    segment->number = number;
    segment->name = "chunk-" + std::to_string(number) + ".m4s";
    segment->data = core::SharedBuffer::copy_from(std::vector<std::byte>(bytes, std::byte{0xAB}));
    segment->duration = std::chrono::milliseconds(4000);
    segment->init_epoch = 1;
    return segment;
}

InitSegmentPtr make_init(std::uint64_t epoch) {
    auto init = std::make_shared<InitSegment>();
    init->data = core::SharedBuffer::copy_from(std::vector<std::byte>(32, std::byte{0x11}));
    init->epoch = epoch;
    return init;
}

} // namespace

TEST(DashSegmentStoreTest, SegmentsAreResolvableByNameAndByNumber) {
    SegmentStore store;
    store.add_segment(make_segment(0));
    store.add_segment(make_segment(1));

    ASSERT_NE(store.find_segment("chunk-0.m4s"), nullptr);
    ASSERT_NE(store.find_segment_by_number(1), nullptr);
    EXPECT_EQ(store.find_segment("chunk-99.m4s"), nullptr);
    EXPECT_EQ(store.find_segment_by_number(99), nullptr);
}

TEST(DashSegmentStoreTest, InitSegmentIsWhateverWasMostRecentlySet) {
    SegmentStore store;
    EXPECT_EQ(store.current_init(), nullptr);
    store.set_init_segment(make_init(1));
    ASSERT_NE(store.current_init(), nullptr);
    EXPECT_EQ(store.current_init()->epoch, 1u);
    store.set_init_segment(make_init(2));
    EXPECT_EQ(store.current_init()->epoch, 2u);
}

TEST(DashSegmentStoreTest, WindowAndRetentionGraceBoundTheRetainedSegments) {
    SegmentStoreConfig config;
    config.live_window_segments = 2;
    config.retention_grace_segments = 1;
    SegmentStore store(config);

    for (std::uint64_t i = 0; i < 6; ++i) store.add_segment(make_segment(i));

    // Retained = window + grace = 3, so only 3..5 should still resolve.
    EXPECT_EQ(store.segment_count(), 3u);
    EXPECT_EQ(store.find_segment("chunk-2.m4s"), nullptr);
    EXPECT_NE(store.find_segment("chunk-3.m4s"), nullptr);
    EXPECT_NE(store.find_segment("chunk-5.m4s"), nullptr);
    EXPECT_EQ(store.stats().segments_evicted, 3u);
}

TEST(DashSegmentStoreTest, ByteCapEvictsWhileAlwaysKeepingAtLeastOneSegment) {
    SegmentStoreConfig config;
    config.live_window_segments = 100;
    config.retention_grace_segments = 0;
    config.max_total_bytes = 250;
    SegmentStore store(config);

    store.add_segment(make_segment(0, 100));
    store.add_segment(make_segment(1, 100));
    store.add_segment(make_segment(2, 100)); // now 300 bytes held, over the cap

    EXPECT_LE(store.stats().bytes_held, 300u);
    EXPECT_GE(store.segment_count(), 1u);
    EXPECT_NE(store.find_segment("chunk-2.m4s"), nullptr); // newest always survives
}

TEST(DashSegmentStoreTest, StartAndNextNumberTrackTheWindow) {
    SegmentStoreConfig config;
    config.live_window_segments = 2;
    config.retention_grace_segments = 0;
    SegmentStore store(config);
    EXPECT_EQ(store.start_number(), 0u);
    EXPECT_EQ(store.next_number(), 0u);

    store.add_segment(make_segment(0));
    store.add_segment(make_segment(1));
    store.add_segment(make_segment(2));
    EXPECT_EQ(store.start_number(), 1u); // segment 0 evicted
    EXPECT_EQ(store.next_number(), 3u);
}

TEST(DashSegmentStoreTest, ClearResetsEverything) {
    SegmentStore store;
    store.set_init_segment(make_init(1));
    store.add_segment(make_segment(0));
    store.mark_ended();

    store.clear();
    EXPECT_EQ(store.segment_count(), 0u);
    EXPECT_FALSE(store.ended());
    EXPECT_EQ(store.stats().bytes_held, 0u);
    // clear() only touches the segment deque, not the init segment (a
    // publisher reconnect on the same codec configuration should not have to
    // re-derive it before the next segment can be served).
    EXPECT_NE(store.current_init(), nullptr);
}

TEST(DashSegmentStoreTest, MarkEndedIsObservable) {
    SegmentStore store;
    EXPECT_FALSE(store.ended());
    store.mark_ended();
    EXPECT_TRUE(store.ended());
}
