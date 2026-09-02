#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "rtmp_server/control/hls_http_handler.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/hls/segmenter.hpp"
#include "test_media.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls;
using namespace rtmp_server::hls_test;

namespace {

std::vector<std::string> lines_of(const std::string& playlist) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < playlist.size()) {
        const auto nl = playlist.find('\n', pos);
        lines.push_back(playlist.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

std::size_t count_lines_starting_with(const std::string& playlist, const std::string& prefix) {
    std::size_t count = 0;
    for (const auto& line : lines_of(playlist)) {
        if (line.rfind(prefix, 0) == 0) ++count;
    }
    return count;
}

bool has_line_starting_with(const std::string& playlist, const std::string& prefix) {
    return count_lines_starting_with(playlist, prefix) > 0;
}

std::string line_starting_with(const std::string& playlist, const std::string& prefix) {
    for (const auto& line : lines_of(playlist)) {
        if (line.rfind(prefix, 0) == 0) return line;
    }
    return {};
}

SegmentPtr make_segment(std::uint64_t sequence, std::size_t part_count,
                        std::chrono::milliseconds duration = std::chrono::milliseconds(4000)) {
    auto segment = std::make_shared<Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = duration;
    segment->data = core::SharedBuffer::copy_from(std::vector<std::byte>(188, std::byte{0}));
    for (std::size_t i = 0; i < part_count; ++i) {
        auto part = std::make_shared<Part>();
        part->segment_sequence = sequence;
        part->index = static_cast<std::uint32_t>(i);
        part->name = "segment-" + std::to_string(sequence) + "." + std::to_string(i) + ".ts";
        part->duration = duration / static_cast<int>(part_count);
        part->independent = (i == 0);
        part->data = core::SharedBuffer::copy_from(std::vector<std::byte>(188, std::byte{0}));
        segment->parts.push_back(std::move(part));
    }
    return segment;
}

} // namespace

// ---------------------------------------------------------------------------
// Playlist rendering
// ---------------------------------------------------------------------------

TEST(LowLatencyPlaylistTest, PartTagsAndServerControlAppearOnlyInLowLatencyMode) {
    std::vector<SegmentPtr> segments{make_segment(1, 4), make_segment(2, 4)};

    MediaPlaylistOptions plain;
    plain.target_duration_seconds = 4;
    const auto plain_body = build_media_playlist(segments, plain);
    EXPECT_FALSE(has_line_starting_with(plain_body, "#EXT-X-PART"));
    EXPECT_NE(line_starting_with(plain_body, "#EXT-X-SERVER-CONTROL").find("CAN-BLOCK-RELOAD=NO"),
              std::string::npos);

    MediaPlaylistOptions low;
    low.target_duration_seconds = 4;
    low.low_latency = true;
    low.part_target_seconds = 1.0;
    const auto low_body = build_media_playlist(segments, low);
    EXPECT_EQ(count_lines_starting_with(low_body, "#EXT-X-PART:"), 8u);
    EXPECT_TRUE(has_line_starting_with(low_body, "#EXT-X-PART-INF:PART-TARGET=1.000"));
    EXPECT_NE(line_starting_with(low_body, "#EXT-X-SERVER-CONTROL").find("CAN-BLOCK-RELOAD=YES"),
              std::string::npos);
    // EXT-X-PART requires version 9; emitting it under a lower declared
    // version is a playlist a strict player rejects outright.
    EXPECT_EQ(line_starting_with(low_body, "#EXT-X-VERSION"), "#EXT-X-VERSION:9");
}

TEST(LowLatencyPlaylistTest, PartHoldBackIsClampedToThreePartTargets) {
    std::vector<SegmentPtr> segments{make_segment(1, 2)};
    MediaPlaylistOptions options;
    options.low_latency = true;
    options.part_target_seconds = 0.5;
    options.part_hold_back_seconds = 0.2; // below the RFC 8216bis floor
    const auto body = build_media_playlist(segments, options);
    EXPECT_NE(line_starting_with(body, "#EXT-X-SERVER-CONTROL").find("PART-HOLD-BACK=1.500"),
              std::string::npos);
}

TEST(LowLatencyPlaylistTest, OnlyTheNewestSegmentsKeepTheirPartTags) {
    std::vector<SegmentPtr> segments;
    for (std::uint64_t i = 1; i <= 5; ++i) segments.push_back(make_segment(i, 2));

    MediaPlaylistOptions options;
    options.low_latency = true;
    options.part_target_seconds = 2.0;
    options.part_window_segments = 2;
    const auto body = build_media_playlist(segments, options);
    // 2 segments x 2 parts. Older segments are fetched whole, so repeating
    // their parts would only inflate a body refetched several times a second.
    EXPECT_EQ(count_lines_starting_with(body, "#EXT-X-PART:"), 4u);
    EXPECT_NE(body.find("segment-4.0.ts"), std::string::npos);
    EXPECT_EQ(body.find("segment-1.0.ts"), std::string::npos);
}

TEST(LowLatencyPlaylistTest, IndependentPartsAreMarkedAndOthersAreNot) {
    std::vector<SegmentPtr> segments{make_segment(1, 3)};
    MediaPlaylistOptions options;
    options.low_latency = true;
    options.part_target_seconds = 1.0;
    const auto body = build_media_playlist(segments, options);

    const auto lines = lines_of(body);
    std::vector<std::string> parts;
    for (const auto& line : lines) {
        if (line.rfind("#EXT-X-PART:", 0) == 0) parts.push_back(line);
    }
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_NE(parts[0].find("INDEPENDENT=YES"), std::string::npos);
    EXPECT_EQ(parts[1].find("INDEPENDENT=YES"), std::string::npos);
    EXPECT_EQ(parts[2].find("INDEPENDENT=YES"), std::string::npos);
}

TEST(LowLatencyPlaylistTest, OpenPartsAndPreloadHintFollowTheLastCompleteSegment) {
    std::vector<SegmentPtr> segments{make_segment(1, 2)};
    auto open = make_segment(2, 2); // only used for its parts
    std::vector<PartPtr> open_parts = open->parts;

    MediaPlaylistOptions options;
    options.low_latency = true;
    options.part_target_seconds = 2.0;
    options.open_parts = open_parts;
    options.preload_hint_uri = "segment-2.2.ts";
    const auto body = build_media_playlist(segments, options);

    const auto lines = lines_of(body);
    std::size_t segment_line = 0;
    std::size_t open_part_line = 0;
    std::size_t hint_line = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == "segment-1.ts") segment_line = i;
        if (lines[i].find("segment-2.1.ts") != std::string::npos) open_part_line = i;
        if (lines[i].rfind("#EXT-X-PRELOAD-HINT:", 0) == 0) hint_line = i;
    }
    ASSERT_GT(segment_line, 0u);
    // The open segment's parts describe media after every complete segment,
    // and the hint names the one part that does not exist yet, so both must
    // come last and in that order.
    EXPECT_GT(open_part_line, segment_line);
    EXPECT_GT(hint_line, open_part_line);
    EXPECT_NE(lines[hint_line].find("TYPE=PART"), std::string::npos);
    EXPECT_NE(lines[hint_line].find("segment-2.2.ts"), std::string::npos);
}

// ---------------------------------------------------------------------------
// EXT-X-KEY rendering
// ---------------------------------------------------------------------------

TEST(EncryptedPlaylistTest, KeyTagIsEmittedOnceAndAgainOnlyWhenTheKeyChanges) {
    auto key_a = std::make_shared<EncryptionKeyInfo>();
    key_a->method = "AES-128";
    key_a->uri = "key-aaa.bin";
    key_a->iv_hex = "0x000000000000000000000000000000001";
    auto key_b = std::make_shared<EncryptionKeyInfo>(*key_a);
    key_b->uri = "key-bbb.bin";

    std::vector<SegmentPtr> segments;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        auto segment = std::const_pointer_cast<Segment>(make_segment(i, 0));
        segment->key = (i <= 2) ? key_a : key_b;
        segments.push_back(segment);
    }

    MediaPlaylistOptions options;
    const auto body = build_media_playlist(segments, options);
    // One tag per distinct key: EXT-X-KEY applies to every following segment
    // until the next one, so repeating it per segment is pure bloat, and
    // omitting the second would make segments 3-4 undecryptable.
    EXPECT_EQ(count_lines_starting_with(body, "#EXT-X-KEY:"), 2u);
    EXPECT_LT(body.find("key-aaa.bin"), body.find("key-bbb.bin"));
    EXPECT_LT(body.find("key-aaa.bin"), body.find("segment-1.ts"));
    EXPECT_LT(body.find("key-bbb.bin"), body.find("segment-3.ts"));
}

TEST(EncryptedPlaylistTest, ReturningToClearMediaEmitsMethodNone) {
    auto key = std::make_shared<EncryptionKeyInfo>();
    key->method = "AES-128";
    key->uri = "key-aaa.bin";

    std::vector<SegmentPtr> segments;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        auto segment = std::const_pointer_cast<Segment>(make_segment(i, 0));
        if (i < 3) segment->key = key;
        segments.push_back(segment);
    }

    MediaPlaylistOptions options;
    const auto body = build_media_playlist(segments, options);
    // Without METHOD=NONE the player would keep trying to decrypt the clear
    // segment with the previous key.
    EXPECT_NE(body.find("#EXT-X-KEY:METHOD=NONE"), std::string::npos);
    EXPECT_LT(body.find("#EXT-X-KEY:METHOD=NONE"), body.find("segment-3.ts"));
}

// ---------------------------------------------------------------------------
// Trick play
// ---------------------------------------------------------------------------

TEST(IframePlaylistTest, ListsByteRangesAndSkipsSegmentsWithoutAnIframePrefix) {
    std::vector<SegmentPtr> segments;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        auto segment = std::const_pointer_cast<Segment>(make_segment(i, 0));
        segment->iframe_prefix_bytes = (i == 2) ? 0 : 1128; // segment 2 never got a keyframe
        segments.push_back(segment);
    }

    MediaPlaylistOptions options;
    options.iframes_only = true;
    const auto body = build_media_playlist(segments, options);

    EXPECT_TRUE(has_line_starting_with(body, "#EXT-X-I-FRAMES-ONLY"));
    EXPECT_EQ(count_lines_starting_with(body, "#EXT-X-BYTERANGE:1128@0"), 2u);
    EXPECT_NE(body.find("segment-1.ts"), std::string::npos);
    EXPECT_EQ(body.find("segment-2.ts"), std::string::npos);
    // EXT-X-I-FRAMES-ONLY requires version 4 at minimum.
    EXPECT_EQ(line_starting_with(body, "#EXT-X-VERSION"), "#EXT-X-VERSION:4");
}

TEST(IframePlaylistTest, MasterPlaylistAdvertisesTrickPlayVariantsSeparately) {
    std::vector<Rendition> renditions;
    Rendition low;
    low.uri = "../low/index.m3u8";
    low.bandwidth = 800000;
    low.codecs = "avc1.64001f";
    low.width = 640;
    low.height = 360;
    low.iframe_uri = "../low/iframe.m3u8";
    renditions.push_back(low);

    Rendition high;
    high.uri = "../high/index.m3u8";
    high.bandwidth = 3000000;
    renditions.push_back(high); // no trick-play playlist declared

    const auto body = build_master_playlist(renditions);
    EXPECT_EQ(count_lines_starting_with(body, "#EXT-X-STREAM-INF:"), 2u);
    EXPECT_EQ(count_lines_starting_with(body, "#EXT-X-I-FRAME-STREAM-INF:"), 1u);
    // The trick-play variant carries its URI as an attribute, not on the
    // following line, and must not advertise a frame rate.
    const auto iframe_line = line_starting_with(body, "#EXT-X-I-FRAME-STREAM-INF:");
    EXPECT_NE(iframe_line.find("URI=\"../low/iframe.m3u8\""), std::string::npos);
    EXPECT_EQ(iframe_line.find("FRAME-RATE"), std::string::npos);
    EXPECT_EQ(line_starting_with(body, "#EXT-X-VERSION"), "#EXT-X-VERSION:4");
}

// ---------------------------------------------------------------------------
// SegmentStore: parts, live edge, blocking-reload readiness
// ---------------------------------------------------------------------------

namespace {

PartPtr make_part(std::uint64_t sequence, std::uint32_t index, bool independent = false) {
    auto part = std::make_shared<Part>();
    part->segment_sequence = sequence;
    part->index = index;
    part->name = "segment-" + std::to_string(sequence) + "." + std::to_string(index) + ".ts";
    part->duration = std::chrono::milliseconds(300);
    part->independent = independent;
    part->data = core::SharedBuffer::copy_from(std::vector<std::byte>(376, std::byte{1}));
    return part;
}

SegmentStoreConfig low_latency_store_config() {
    SegmentStoreConfig config;
    config.low_latency = true;
    config.part_target_duration = std::chrono::milliseconds(300);
    config.live_window_segments = 3;
    config.retention_grace_segments = 1;
    config.target_duration_seconds = 2;
    return config;
}

} // namespace

TEST(SegmentStorePartsTest, PartsAreResolvableByNameBeforeAndAfterTheirSegmentCompletes) {
    SegmentStore store(low_latency_store_config());
    store.add_part(make_part(0, 0, true));
    store.add_part(make_part(0, 1));

    ASSERT_NE(store.find_part("segment-0.0.ts"), nullptr);
    ASSERT_NE(store.find_part("segment-0.1.ts"), nullptr);
    EXPECT_EQ(store.find_part("segment-0.2.ts"), nullptr);

    auto segment = std::const_pointer_cast<Segment>(make_segment(0, 0));
    store.add_segment(segment);
    // A player mid-fetch must not be 404ed the instant its segment completes.
    EXPECT_NE(store.find_part("segment-0.0.ts"), nullptr);
    EXPECT_NE(store.find_segment("segment-0.ts"), nullptr);
}

TEST(SegmentStorePartsTest, EvictingASegmentAlsoEvictsItsParts) {
    auto config = low_latency_store_config();
    config.live_window_segments = 1;
    config.retention_grace_segments = 0;
    SegmentStore store(config);

    store.add_part(make_part(0, 0, true));
    store.add_segment(make_segment(0, 0));
    ASSERT_NE(store.find_part("segment-0.0.ts"), nullptr);

    store.add_part(make_part(1, 0, true));
    store.add_segment(make_segment(1, 0));
    // Segment 0 has scrolled out, so keeping its parts would be memory held
    // for media no playlist references any more.
    EXPECT_EQ(store.find_segment("segment-0.ts"), nullptr);
    EXPECT_EQ(store.find_part("segment-0.0.ts"), nullptr);
    EXPECT_NE(store.find_part("segment-1.0.ts"), nullptr);
    EXPECT_EQ(store.stats().parts_evicted, 1u);
}

TEST(SegmentStorePartsTest, PartsAreIgnoredWhenLowLatencyIsOff) {
    SegmentStore store({}); // low_latency defaults to false
    store.add_part(make_part(0, 0, true));
    EXPECT_EQ(store.find_part("segment-0.0.ts"), nullptr);
    EXPECT_EQ(store.stats().parts_added, 0u);
}

TEST(SegmentStorePartsTest, LiveEdgeTracksTheNewestPublishedPart) {
    SegmentStore store(low_latency_store_config());
    EXPECT_FALSE(store.live_edge().has_media);

    store.add_segment(make_segment(0, 0));
    auto edge = store.live_edge();
    EXPECT_TRUE(edge.has_media);
    EXPECT_EQ(edge.sequence, 0u);
    EXPECT_EQ(edge.part_index, -1);

    store.add_part(make_part(1, 0, true));
    store.add_part(make_part(1, 1));
    edge = store.live_edge();
    // The open segment is the real live edge once it has published a part.
    EXPECT_EQ(edge.sequence, 1u);
    EXPECT_EQ(edge.part_index, 1);
}

TEST(SegmentStorePartsTest, BlockingReloadReadinessFollowsTheLiveEdge) {
    SegmentStore store(low_latency_store_config());
    // Nothing published yet: every position must wait, including 0.
    EXPECT_FALSE(store.has_reached(0, -1));

    store.add_segment(make_segment(0, 0));
    EXPECT_TRUE(store.has_reached(0, -1));
    EXPECT_FALSE(store.has_reached(1, -1));
    EXPECT_FALSE(store.has_reached(1, 0));

    store.add_part(make_part(1, 0, true));
    EXPECT_TRUE(store.has_reached(1, 0));
    EXPECT_FALSE(store.has_reached(1, 1));
    // A request naming only the segment is not satisfied by its parts: the
    // segment itself does not exist yet.
    EXPECT_FALSE(store.has_reached(1, -1));

    store.add_part(make_part(1, 1));
    EXPECT_TRUE(store.has_reached(1, 1));
    store.add_segment(make_segment(1, 0));
    EXPECT_TRUE(store.has_reached(1, -1));
}

TEST(SegmentStorePartsTest, AnEndedStreamSatisfiesEveryPendingPosition) {
    SegmentStore store(low_latency_store_config());
    store.add_segment(make_segment(0, 0));
    EXPECT_FALSE(store.has_reached(99, 5));
    store.mark_ended();
    // Holding a request open for media that will never arrive just burns the
    // request budget and then answers anyway.
    EXPECT_TRUE(store.has_reached(99, 5));
}

TEST(SegmentStorePartsTest, UpdateNotifierFiresForEveryPublishAndForTheEnd) {
    SegmentStore store(low_latency_store_config());
    int notifications = 0;
    store.set_update_notifier([&notifications] { ++notifications; });

    store.add_part(make_part(0, 0, true));
    store.add_part(make_part(0, 1));
    store.add_segment(make_segment(0, 0));
    store.mark_ended();
    EXPECT_EQ(notifications, 4);

    store.set_update_notifier({});
    store.add_segment(make_segment(1, 0));
    EXPECT_EQ(notifications, 4);
}

TEST(SegmentStorePartsTest, PlaylistCarriesOpenPartsAndAPreloadHint) {
    SegmentStore store(low_latency_store_config());
    store.add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));
    store.add_part(make_part(1, 0, true));
    store.add_part(make_part(1, 1));

    const auto body = store.playlist({});
    EXPECT_TRUE(has_line_starting_with(body, "#EXT-X-PART-INF:"));
    EXPECT_EQ(count_lines_starting_with(body, "#EXT-X-PART:"), 2u);
    // The hint must name the very next part, so the player's request for it
    // is already in flight when the encoder produces it.
    EXPECT_NE(body.find("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"segment-1.2.ts\""), std::string::npos);
}

TEST(SegmentStorePartsTest, EncryptedSegmentsAndPartsShareOneKeyAndDropTheIframeRange) {
    EncryptionConfig encryption;
    encryption.enabled = true;
    encryption.key_uri_template = "key-{kid}.bin";
    auto encryptor = std::make_shared<SegmentEncryptor>(encryption);

    SegmentStore store(low_latency_store_config());
    store.set_encryptor(encryptor);

    auto plain = std::const_pointer_cast<Segment>(make_segment(0, 0));
    plain->iframe_prefix_bytes = 188;
    const auto plain_size = plain->data.size();
    store.add_segment(plain);

    const auto stored = store.find_segment("segment-0.ts");
    ASSERT_NE(stored, nullptr);
    ASSERT_NE(stored->key, nullptr);
    EXPECT_EQ(stored->key->method, "AES-128");
    EXPECT_EQ(stored->data.size(), (plain_size / kAesBlockBytes + 1) * kAesBlockBytes);
    // The byte range would point into ciphertext, which no player can use as
    // an I-frame, so trick play is dropped rather than made wrong.
    EXPECT_EQ(stored->iframe_prefix_bytes, 0u);

    store.add_part(make_part(1, 0, true));
    const auto part = store.find_part("segment-1.0.ts");
    ASSERT_NE(part, nullptr);
    EXPECT_EQ(part->data.size(), (376u / kAesBlockBytes + 1) * kAesBlockBytes);
}

// ---------------------------------------------------------------------------
// Segmenter: part production
// ---------------------------------------------------------------------------

TEST(SegmenterPartsTest, NoPartsAreProducedWhenTheTargetIsZero) {
    std::vector<PartPtr> parts;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(2000);
    Segmenter segmenter([](SegmentPtr) {}, config);
    segmenter.set_part_callback([&parts](PartPtr part) { parts.push_back(std::move(part)); });

    segmenter.on_video(video_message(0, avc_sequence_header()));
    for (std::uint32_t ts = 0; ts < 3000; ts += 100) {
        segmenter.on_video(video_message(ts, avc_frame(ts % 1000 == 0)));
    }
    segmenter.finalize();
    EXPECT_TRUE(parts.empty());
}

TEST(SegmenterPartsTest, PartsAreCutAtTheTargetAndNumberedWithinTheirSegment) {
    std::vector<SegmentPtr> segments;
    std::vector<PartPtr> parts;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(2000);
    config.part_target_duration = std::chrono::milliseconds(500);
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, config);
    segmenter.set_part_callback([&parts](PartPtr part) { parts.push_back(std::move(part)); });

    segmenter.on_video(video_message(0, avc_sequence_header()));
    // 4 s of 10 fps video with a keyframe every 2 s: two segments, each cut
    // into 500 ms parts.
    for (std::uint32_t ts = 0; ts <= 4000; ts += 100) {
        segmenter.on_video(video_message(ts, avc_frame(ts % 2000 == 0)));
    }
    segmenter.finalize();

    ASSERT_GE(segments.size(), 2u);
    ASSERT_FALSE(parts.empty());
    EXPECT_EQ(segmenter.stats().parts_produced, parts.size());

    // Part indices restart at 0 in every segment, and every part names the
    // segment it belongs to — the playlist URI depends on both.
    std::uint64_t current_sequence = parts.front()->segment_sequence;
    std::uint32_t expected_index = 0;
    for (const auto& part : parts) {
        if (part->segment_sequence != current_sequence) {
            current_sequence = part->segment_sequence;
            expected_index = 0;
        }
        EXPECT_EQ(part->index, expected_index);
        EXPECT_EQ(part->name, "segment-" + std::to_string(part->segment_sequence) + "." +
                                  std::to_string(part->index) + ".ts");
        EXPECT_GT(part->size_bytes(), 0u);
        ++expected_index;
    }

    // Exactly one independent part per segment: the one that opens with the
    // keyframe. Marking a mid-GOP part independent sends a joining player
    // into an undecodable slice.
    for (const auto& segment : segments) {
        if (segment->parts.empty()) continue;
        std::size_t independent = 0;
        for (const auto& part : segment->parts) independent += part->independent ? 1 : 0;
        EXPECT_EQ(independent, 1u);
        EXPECT_TRUE(segment->parts.front()->independent);
    }
}

TEST(SegmenterPartsTest, PartsConcatenateBackIntoTheirSegmentExactly) {
    std::vector<SegmentPtr> segments;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(1000);
    config.part_target_duration = std::chrono::milliseconds(200);
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, config);
    segmenter.set_part_callback([](PartPtr) {});

    segmenter.on_video(video_message(0, avc_sequence_header()));
    for (std::uint32_t ts = 0; ts <= 2000; ts += 100) {
        segmenter.on_video(video_message(ts, avc_frame(ts % 1000 == 0)));
    }
    segmenter.finalize();

    ASSERT_FALSE(segments.empty());
    for (const auto& segment : segments) {
        if (segment->parts.empty()) continue;
        // A part must be a byte-exact slice of its segment: a player that
        // fetched every part has the segment, and one that fetched the
        // segment can skip the parts.
        std::vector<std::byte> rebuilt;
        for (const auto& part : segment->parts) {
            const auto view = part->data.view();
            rebuilt.insert(rebuilt.end(), view.begin(), view.end());
        }
        const auto whole = segment->data.view();
        ASSERT_EQ(rebuilt.size(), whole.size());
        EXPECT_TRUE(std::equal(rebuilt.begin(), rebuilt.end(), whole.begin()));
    }
}

TEST(SegmenterPartsTest, TheIframePrefixCoversTheProgramTablesAndTheFirstKeyframe) {
    std::vector<SegmentPtr> segments;
    SegmenterConfig config;
    config.target_duration = std::chrono::milliseconds(1000);
    Segmenter segmenter([&segments](SegmentPtr s) { segments.push_back(std::move(s)); }, config);

    segmenter.on_video(video_message(0, avc_sequence_header()));
    for (std::uint32_t ts = 0; ts <= 2000; ts += 100) {
        segmenter.on_video(video_message(ts, avc_frame(ts % 1000 == 0)));
    }
    segmenter.finalize();

    ASSERT_FALSE(segments.empty());
    const auto& first = segments.front();
    // Non-zero (a keyframe was located), starts at 0 so the PAT/PMT are
    // included, and shorter than the whole segment.
    EXPECT_GT(first->iframe_prefix_bytes, 2u * media::ts::kPacketSize);
    EXPECT_LT(first->iframe_prefix_bytes, first->size_bytes());
    EXPECT_EQ(first->iframe_prefix_bytes % media::ts::kPacketSize, 0u);
}

// ---------------------------------------------------------------------------
// HTTP delivery: parts, keys, trick play, blocking reload
// ---------------------------------------------------------------------------

namespace {

using rtmp_server::control::HlsHttpHandler;
using rtmp_server::control::HlsHttpOptions;
using rtmp_server::control::HttpRequest;
using rtmp_server::control::HttpResponse;

HttpRequest get(const std::string& path, const std::string& query = {}) {
    HttpRequest request;
    request.method = "GET";
    request.path = path;
    request.query = query;
    request.http_version = "HTTP/1.1";
    request.client_ip = "127.0.0.1";
    return request;
}

HlsHttpOptions low_latency_http_options() {
    HlsHttpOptions options;
    options.enable_low_latency = true;
    options.blocking_reload_timeout = std::chrono::milliseconds(200);
    // Playback sessions add a redirect hop that has nothing to do with what
    // these tests exercise.
    options.enable_playback_sessions = false;
    return options;
}

} // namespace

TEST(LowLatencyHttpTest, PartialSegmentsAreServedFromTheirOwnUri) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);

    store->add_part(make_part(0, 0, true));
    const auto response = handler.handle(get("/hls/live/demo/segment-0.0.ts"));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.payload_size(), 376u);
    EXPECT_STREQ(response.content_type.c_str(), rtmp_server::control::kContentTypeMpegTs);

    // A part that has not been produced is a 404 that must not be cached, or
    // the cache would keep answering 404 after the part exists.
    const auto missing = handler.handle(get("/hls/live/demo/segment-0.9.ts"));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.headers.at("Cache-Control"), "no-store");
}

TEST(LowLatencyHttpTest, TrickPlayPlaylistIsServedAndCanBeDisabled) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    auto segment = std::const_pointer_cast<Segment>(make_segment(0, 0));
    segment->iframe_prefix_bytes = 188;
    store->add_segment(segment);

    HlsHttpHandler enabled(low_latency_http_options());
    enabled.register_stream("live", "demo", store);
    const auto response = enabled.handle(get("/hls/live/demo/iframe.m3u8"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("#EXT-X-I-FRAMES-ONLY"), std::string::npos);
    EXPECT_NE(response.body.find("#EXT-X-BYTERANGE:188@0"), std::string::npos);

    auto options = low_latency_http_options();
    options.enable_iframe_playlists = false;
    HlsHttpHandler disabled(options);
    disabled.register_stream("live", "demo", store);
    EXPECT_EQ(disabled.handle(get("/hls/live/demo/iframe.m3u8")).status, 404);
}

TEST(LowLatencyHttpTest, ContentKeyIsServedOnlyForAKnownIdAndIsNeverCacheable) {
    EncryptionConfig encryption;
    encryption.enabled = true;
    encryption.key_uri_template = "key-{kid}.bin";
    auto encryptor = std::make_shared<SegmentEncryptor>(encryption);

    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    store->set_encryptor(encryptor);

    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);
    handler.set_encryptor("live", "demo", encryptor);

    store->add_segment(make_segment(0, 0));
    const auto stored = store->find_segment("segment-0.ts");
    ASSERT_NE(stored, nullptr);
    ASSERT_NE(stored->key, nullptr);

    const auto response = handler.handle(get("/hls/live/demo/" + stored->key->uri));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body.size(), kAesKeyBytes);
    // The key is the authorisation boundary; a shared cache holding it would
    // hand it to every viewer that never passed the gate.
    EXPECT_EQ(response.headers.at("Cache-Control"), "private, no-store");

    EXPECT_EQ(handler.handle(get("/hls/live/demo/key-00000000000000000000000000000000.bin")).status,
              404);
}

TEST(LowLatencyHttpTest, KeyRequestOnAnUnencryptedStreamIs404) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/key-abc.bin")).status, 404);
}

TEST(LowLatencyHttpTest, BlockingReloadIsHeldUntilTheLiveEdgeReachesTheRequestedPart) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);

    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));

    // Ask for a part that does not exist yet: the handler must defer rather
    // than answer with a playlist the player already has.
    const auto pending = handler.handle(get("/hls/live/demo/index.m3u8", "_HLS_msn=1&_HLS_part=1"));
    ASSERT_NE(pending.deferred, nullptr);
    EXPECT_FALSE(pending.deferred->resolved());

    std::string body;
    pending.deferred->attach([&body](HttpResponse response) { body = std::move(response.body); });
    EXPECT_TRUE(body.empty());

    store->add_part(make_part(1, 0, true));
    EXPECT_TRUE(body.empty()) << "part 0 does not satisfy a request for part 1";

    store->add_part(make_part(1, 1));
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("segment-1.1.ts"), std::string::npos);
    EXPECT_TRUE(pending.deferred->resolved());
}

TEST(LowLatencyHttpTest, BlockingReloadForAnAlreadyReachedPositionAnswersImmediately) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);

    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));
    const auto response = handler.handle(get("/hls/live/demo/index.m3u8", "_HLS_msn=0"));
    EXPECT_EQ(response.deferred, nullptr);
    EXPECT_EQ(response.status, 200);
}

TEST(LowLatencyHttpTest, BlockingParametersAreNotCopiedOntoChildUris) {
    auto options = low_latency_http_options();
    options.propagate_query_to_playlist_uris = true;
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(options);
    handler.register_stream("live", "demo", store);

    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));
    const auto pending =
        handler.handle(get("/hls/live/demo/index.m3u8", "token=abc&_HLS_msn=1&_HLS_part=0"));
    ASSERT_NE(pending.deferred, nullptr);

    std::string body;
    pending.deferred->attach([&body](HttpResponse response) { body = std::move(response.body); });
    store->add_part(make_part(1, 0, true));
    ASSERT_FALSE(body.empty());

    // A segment URL carrying a live-edge position would be a distinct cache
    // object per polling player, for no benefit.
    EXPECT_NE(body.find("segment-0.ts?token=abc"), std::string::npos);
    EXPECT_EQ(body.find("_HLS_msn"), std::string::npos);
    EXPECT_EQ(body.find("_HLS_part"), std::string::npos);
}

TEST(LowLatencyHttpTest, BlockingReloadIsIgnoredWhenLowLatencyIsOff) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpOptions options; // enable_low_latency defaults to false
    options.enable_playback_sessions = false;
    HlsHttpHandler handler(options);
    handler.register_stream("live", "demo", store);

    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));
    const auto response = handler.handle(get("/hls/live/demo/index.m3u8", "_HLS_msn=99"));
    EXPECT_EQ(response.deferred, nullptr);
    EXPECT_EQ(response.status, 200);
}

TEST(LowLatencyHttpTest, AMalformedBlockingParameterIsAnsweredRatherThanParkedForever) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);
    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));

    for (const char* query : {"_HLS_msn=abc", "_HLS_msn=", "_HLS_msn=-1"}) {
        const auto response = handler.handle(get("/hls/live/demo/index.m3u8", query));
        EXPECT_EQ(response.deferred, nullptr) << query;
        EXPECT_EQ(response.status, 200) << query;
    }
}

TEST(LowLatencyHttpTest, ParkedRequestsAreReleasedWhenTheStreamEndsOrIsUnregistered) {
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(low_latency_http_options());
    handler.register_stream("live", "demo", store);
    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));

    const auto pending = handler.handle(get("/hls/live/demo/index.m3u8", "_HLS_msn=5"));
    ASSERT_NE(pending.deferred, nullptr);
    bool answered = false;
    pending.deferred->attach([&answered](HttpResponse) { answered = true; });

    // Ending the stream means the live edge will never advance again, so a
    // request waiting on it must be released rather than time out.
    store->mark_ended();
    EXPECT_TRUE(answered);

    store->mark_live();
    const auto second = handler.handle(get("/hls/live/demo/index.m3u8", "_HLS_msn=6"));
    ASSERT_NE(second.deferred, nullptr);
    int status = 0;
    second.deferred->attach([&status](HttpResponse response) { status = response.status; });
    handler.unregister_stream("live", "demo");
    EXPECT_EQ(status, 404);
}

TEST(LowLatencyHttpTest, ParkedRequestsAreBoundedPerStream) {
    auto options = low_latency_http_options();
    options.max_blocked_requests_per_stream = 2;
    auto store = std::make_shared<SegmentStore>(low_latency_store_config());
    HlsHttpHandler handler(options);
    handler.register_stream("live", "demo", store);
    store->add_segment(make_segment(0, 0, std::chrono::milliseconds(2000)));

    std::vector<HttpResponse> responses;
    for (int i = 0; i < 4; ++i) {
        responses.push_back(handler.handle(get("/hls/live/demo/index.m3u8", "_HLS_msn=9")));
    }
    // A parked request costs a socket; past the ceiling the origin answers
    // now rather than letting one stream's blocked requests grow unbounded.
    EXPECT_NE(responses[0].deferred, nullptr);
    EXPECT_NE(responses[1].deferred, nullptr);
    EXPECT_EQ(responses[2].deferred, nullptr);
    EXPECT_EQ(responses[2].status, 200);
    EXPECT_EQ(responses[3].deferred, nullptr);
}
