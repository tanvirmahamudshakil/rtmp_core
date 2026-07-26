#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/hls/segment_store.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls;
using namespace std::chrono_literals;

namespace {

SegmentPtr make_segment(std::uint64_t sequence, std::size_t bytes = 1024,
                        bool discontinuity = false) {
    auto segment = std::make_shared<Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = 4000ms;
    segment->discontinuity = discontinuity;
    segment->data = core::SharedBuffer::adopt(std::vector<std::byte>(bytes, std::byte{0x47}));
    return segment;
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

TEST(SegmentStoreTest, StoresAndRetrievesSegmentsByName) {
    SegmentStore store;
    store.add_segment(make_segment(0));
    store.add_segment(make_segment(1));

    ASSERT_NE(store.find_segment("segment-0.ts"), nullptr);
    EXPECT_EQ(store.find_segment("segment-1.ts")->sequence, 1u);
    EXPECT_EQ(store.find_segment("does-not-exist.ts"), nullptr);
    EXPECT_EQ(store.segment_count(), 2u);
    EXPECT_EQ(store.stats().segment_misses, 1u);
}

TEST(SegmentStoreTest, RetainedSegmentCountIsBoundedByWindowPlusGrace) {
    SegmentStoreConfig config;
    config.live_window_segments = 3;
    config.retention_grace_segments = 2;
    SegmentStore store(config);

    for (std::uint64_t i = 0; i < 50; ++i) store.add_segment(make_segment(i));

    // Bounded cleanup: never more than window + grace, regardless of input.
    EXPECT_EQ(store.segment_count(), 5u);
    EXPECT_EQ(store.stats().segments_added, 50u);
    EXPECT_EQ(store.stats().segments_evicted, 45u);
    // The oldest are gone, the newest are kept.
    EXPECT_EQ(store.find_segment("segment-0.ts"), nullptr);
    EXPECT_NE(store.find_segment("segment-49.ts"), nullptr);
}

TEST(SegmentStoreTest, ByteCapEvictsEvenWhenTheCountIsWithinBounds) {
    SegmentStoreConfig config;
    config.live_window_segments = 100;
    config.retention_grace_segments = 100;
    config.max_total_bytes = 10 * 1024;
    SegmentStore store(config);

    for (std::uint64_t i = 0; i < 40; ++i) store.add_segment(make_segment(i, 1024));

    EXPECT_LE(store.stats().bytes_held, config.max_total_bytes);
    EXPECT_GT(store.stats().segments_evicted, 0u);
    EXPECT_GE(store.segment_count(), 1u);
}

TEST(SegmentStoreTest, AlwaysKeepsAtLeastOneSegmentEvenIfItExceedsTheByteCap) {
    SegmentStoreConfig config;
    config.max_total_bytes = 1024;
    SegmentStore store(config);
    store.add_segment(make_segment(0, 64 * 1024));

    EXPECT_EQ(store.segment_count(), 1u);
    EXPECT_NE(store.find_segment("segment-0.ts"), nullptr);
}

TEST(SegmentStoreTest, PlaylistAdvertisesOnlyTheLiveWindowButGraceSegmentsStayFetchable) {
    SegmentStoreConfig config;
    config.live_window_segments = 3;
    config.retention_grace_segments = 3;
    SegmentStore store(config);

    for (std::uint64_t i = 0; i < 6; ++i) store.add_segment(make_segment(i));

    const auto playlist = store.playlist();
    EXPECT_EQ(count_occurrences(playlist, ".ts"), 3u);
    // The newest three are advertised.
    EXPECT_NE(playlist.find("segment-5.ts"), std::string::npos);
    EXPECT_EQ(playlist.find("segment-2.ts"), std::string::npos);
    // But a player holding a slightly stale playlist can still fetch them —
    // that grace is what makes eviction safe rather than a burst of 404s.
    EXPECT_NE(store.find_segment("segment-2.ts"), nullptr);
    EXPECT_NE(store.find_segment("segment-0.ts"), nullptr);
}

TEST(SegmentStoreTest, MediaSequenceAdvancesAsSegmentsScrollOut) {
    SegmentStoreConfig config;
    config.live_window_segments = 3;
    config.retention_grace_segments = 0;
    SegmentStore store(config);

    for (std::uint64_t i = 0; i < 10; ++i) store.add_segment(make_segment(i));
    const auto playlist = store.playlist();
    EXPECT_NE(playlist.find("#EXT-X-MEDIA-SEQUENCE:7"), std::string::npos) << playlist;
}

TEST(SegmentStoreTest, DiscontinuitySequenceCountsDiscontinuitiesThatLeftTheWindow) {
    SegmentStoreConfig config;
    config.live_window_segments = 2;
    config.retention_grace_segments = 0;
    SegmentStore store(config);

    store.add_segment(make_segment(0));
    store.add_segment(make_segment(1, 1024, /*discontinuity=*/true));
    store.add_segment(make_segment(2));
    store.add_segment(make_segment(3));
    store.add_segment(make_segment(4));

    // Segment 1's discontinuity has scrolled out; a late-joining player must
    // still number its discontinuity sequence correctly.
    const auto playlist = store.playlist();
    EXPECT_NE(playlist.find("#EXT-X-DISCONTINUITY-SEQUENCE:1"), std::string::npos) << playlist;
}

TEST(SegmentStoreTest, MarkEndedAppendsEndlist) {
    SegmentStore store;
    store.add_segment(make_segment(0));
    EXPECT_EQ(store.playlist().find("#EXT-X-ENDLIST"), std::string::npos);

    store.mark_ended();
    EXPECT_NE(store.playlist().find("#EXT-X-ENDLIST"), std::string::npos);
}

TEST(SegmentStoreTest, ClearResetsAllState) {
    SegmentStore store;
    for (std::uint64_t i = 0; i < 5; ++i) store.add_segment(make_segment(i));
    store.mark_ended();
    store.clear();

    EXPECT_EQ(store.segment_count(), 0u);
    EXPECT_EQ(store.stats().bytes_held, 0u);
    EXPECT_EQ(store.playlist().find("#EXT-X-ENDLIST"), std::string::npos);
}

TEST(SegmentStoreTest, NullSegmentsAreIgnored) {
    SegmentStore store;
    store.add_segment(nullptr);
    EXPECT_EQ(store.segment_count(), 0u);
}

TEST(SegmentStoreTest, EvictedSegmentStaysValidForAViewerAlreadyHoldingIt) {
    SegmentStoreConfig config;
    config.live_window_segments = 1;
    config.retention_grace_segments = 0;
    SegmentStore store(config);

    store.add_segment(make_segment(0, 2048));
    auto held = store.find_segment("segment-0.ts");
    ASSERT_NE(held, nullptr);

    // Evict it out from under the "viewer".
    for (std::uint64_t i = 1; i < 5; ++i) store.add_segment(make_segment(i));
    EXPECT_EQ(store.find_segment("segment-0.ts"), nullptr);

    // The shared_ptr the viewer holds keeps the bytes alive and intact.
    EXPECT_EQ(held->data.size(), 2048u);
    EXPECT_EQ(held->data.view()[0], std::byte{0x47});
}

TEST(SegmentStoreTest, ConcurrentProducerAndViewersDoNotRaceOrTearSegments) {
    SegmentStoreConfig config;
    config.live_window_segments = 6;
    config.retention_grace_segments = 6;
    SegmentStore store(config);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> bad{0};

    // One producer, mimicking the media thread.
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < 400; ++i) {
            store.add_segment(make_segment(i, 512));
            std::this_thread::yield();
        }
        stop = true;
    });

    // Several "HTTP worker" readers fetching playlists and segments.
    std::vector<std::thread> viewers;
    for (int v = 0; v < 6; ++v) {
        viewers.emplace_back([&] {
            while (!stop.load()) {
                const auto playlist = store.playlist("/hls/live/demo/");
                if (playlist.rfind("#EXTM3U", 0) != 0) bad++;
                for (std::uint64_t i = 0; i < 400; i += 37) {
                    if (auto segment = store.find_segment("segment-" + std::to_string(i) + ".ts")) {
                        // Any torn/partial buffer would show up here.
                        if (segment->data.size() != 512) bad++;
                        reads++;
                    }
                }
            }
        });
    }

    producer.join();
    for (auto& t : viewers) t.join();

    EXPECT_EQ(bad.load(), 0u);
    EXPECT_GT(reads.load(), 0u);
    // The bound held throughout despite concurrent access.
    EXPECT_LE(store.segment_count(), config.live_window_segments + config.retention_grace_segments);
}
