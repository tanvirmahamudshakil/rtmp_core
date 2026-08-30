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
                        bool discontinuity = false,
                        std::chrono::milliseconds duration = 4000ms) {
    auto segment = std::make_shared<Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = duration;
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

TEST(SegmentStoreTest, NextSequenceContinuesAfterTheNewestRetainedSegment) {
    SegmentStoreConfig config;
    config.live_window_segments = 2;
    config.retention_grace_segments = 1;
    SegmentStore store(config);

    EXPECT_EQ(store.next_sequence(), 0u);
    for (std::uint64_t i = 40; i < 46; ++i) store.add_segment(make_segment(i));

    // Older segments have already been evicted, but a replacement producer
    // still resumes after the newest retained sequence.
    EXPECT_EQ(store.segment_count(), 3u);
    EXPECT_EQ(store.next_sequence(), 46u);
}

TEST(SegmentStoreTest, RepeatsLastGoodSegmentWhileAConfiguredSourceIsStalled) {
    SegmentStoreConfig config;
    config.repeat_last_segment_on_stall = true;
    SegmentStore store(config);

    auto original = make_segment(40, 2048, false, 20ms);
    store.add_segment(original);
    std::this_thread::sleep_for(35ms);

    const auto playlist = store.playlist();
    ASSERT_NE(playlist.find("segment-41.ts"), std::string::npos) << playlist;

    const auto fallback = store.find_segment("segment-41.ts");
    ASSERT_NE(fallback, nullptr);
    // A backfill copy is byte-identical to a segment the player already
    // decoded: it replays cleanly and carries no EXT-X-DISCONTINUITY. The
    // real break is signalled on the first live segment after recovery
    // (see RecoveryContinuesAfterFallbackWithoutReusingAPlaylistUrl).
    EXPECT_FALSE(fallback->discontinuity);
    EXPECT_EQ(playlist.find("#EXT-X-DISCONTINUITY"), std::string::npos) << playlist;
    EXPECT_EQ(fallback->data.size(), original->data.size());
    EXPECT_EQ(fallback->data.view().data(), original->data.view().data());
    EXPECT_EQ(store.stats().fallback_segments_added, 1u);
    EXPECT_EQ(store.stats().real_segments_added, 1u);
}

TEST(SegmentStoreTest, RecoveryContinuesAfterFallbackWithoutReusingAPlaylistUrl) {
    SegmentStoreConfig config;
    config.repeat_last_segment_on_stall = true;
    SegmentStore store(config);

    auto original = make_segment(7, 1024, false, 20ms);
    store.add_segment(original);
    std::this_thread::sleep_for(35ms);
    (void)store.playlist(); // Adds synthetic segment 8.

    // The producer allocated 8 before the fallback appeared. The store must
    // move it to 9 and signal the timestamp transition back to live media.
    auto recovered = make_segment(8, 3072, false, 20ms);
    store.add_segment(recovered);

    EXPECT_EQ(store.next_sequence(), 10u);
    EXPECT_NE(store.find_segment("segment-8.ts"), nullptr);
    const auto live = store.find_segment("segment-9.ts");
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(live->discontinuity);
    EXPECT_EQ(live->data.size(), 3072u);
}

TEST(SegmentStoreTest, SeamlessFallbackRecoveryOmitsTheDiscontinuityOnResume) {
    SegmentStoreConfig config;
    config.repeat_last_segment_on_stall = true;
    config.seamless_fallback_recovery = true; // re-encoded, re-anchored timeline
    SegmentStore store(config);

    auto original = make_segment(7, 1024, false, 20ms);
    store.add_segment(original);
    std::this_thread::sleep_for(35ms);
    (void)store.playlist(); // synthetic segment 8 (no discontinuity)

    auto recovered = make_segment(8, 3072, false, 20ms);
    store.add_segment(recovered);

    const auto backfill = store.find_segment("segment-8.ts");
    ASSERT_NE(backfill, nullptr);
    EXPECT_FALSE(backfill->discontinuity);
    const auto live = store.find_segment("segment-9.ts");
    ASSERT_NE(live, nullptr);
    // Re-numbered for URL monotonicity, but NOT marked discontinuous: the
    // producer's output timeline is continuous across the outage.
    EXPECT_FALSE(live->discontinuity);
    EXPECT_EQ(live->data.size(), 3072u);

    const auto playlist = store.playlist();
    EXPECT_EQ(playlist.find("#EXT-X-DISCONTINUITY"), std::string::npos) << playlist;
}

TEST(SegmentStoreTest, OrdinaryPublishersDoNotRepeatSegmentsByDefault) {
    SegmentStore store;
    auto segment = make_segment(0, 1024, false, 20ms);
    store.add_segment(segment);
    std::this_thread::sleep_for(35ms);

    (void)store.playlist();
    EXPECT_EQ(store.segment_count(), 1u);
    EXPECT_EQ(store.stats().fallback_segments_added, 0u);
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

    store.mark_live();
    EXPECT_EQ(store.playlist().find("#EXT-X-ENDLIST"), std::string::npos);
    EXPECT_NE(store.find_segment("segment-0.ts"), nullptr);
    EXPECT_EQ(store.next_sequence(), 1u);
}

TEST(SegmentStoreTest, ClearResetsAllState) {
    SegmentStore store;
    for (std::uint64_t i = 0; i < 5; ++i) store.add_segment(make_segment(i));
    store.mark_ended();
    store.clear();

    EXPECT_EQ(store.segment_count(), 0u);
    EXPECT_EQ(store.next_sequence(), 0u);
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
