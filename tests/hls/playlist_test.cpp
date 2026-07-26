#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "rtmp_server/hls/playlist.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls;

// ---------------------------------------------------------------------------
// RFC 8216 playlist validator.
//
// ffmpeg/ffprobe are NOT installed on this host (verified with
// `which ffmpeg ffprobe` — see docs/phase-6-report.md), so instead of a
// player-based check this parses the generated playlist and asserts the
// structural rules RFC 8216 actually imposes. It is deliberately strict:
// it fails on rule violations rather than merely looking for substrings.
// ---------------------------------------------------------------------------
namespace {

struct ValidationResult {
    bool valid = true;
    std::vector<std::string> errors;
    std::uint32_t target_duration = 0;
    std::uint64_t media_sequence = 0;
    std::uint64_t discontinuity_sequence = 0;
    std::size_t segment_count = 0;
    std::size_t discontinuity_count = 0;
    bool has_endlist = false;
    std::vector<double> durations;
    std::vector<std::string> uris;

    void fail(std::string message) {
        valid = false;
        errors.push_back(std::move(message));
    }
};

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

ValidationResult validate_media_playlist(const std::string& text) {
    ValidationResult result;
    const auto lines = split_lines(text);

    if (lines.empty() || lines[0] != "#EXTM3U") {
        result.fail("playlist must begin with #EXTM3U (RFC 8216 4.3.1.1)");
        return result;
    }

    bool seen_target_duration = false;
    bool seen_version = false;
    bool pending_extinf = false;
    double pending_duration = 0.0;
    bool pending_discontinuity = false;
    bool endlist_seen = false;

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;

        if (endlist_seen && line[0] != '#') {
            result.fail("segment URI appears after #EXT-X-ENDLIST");
        }

        if (starts_with(line, "#EXT-X-TARGETDURATION:")) {
            if (seen_target_duration) result.fail("duplicate #EXT-X-TARGETDURATION");
            seen_target_duration = true;
            result.target_duration =
                static_cast<std::uint32_t>(std::strtoul(line.substr(22).c_str(), nullptr, 10));
            if (result.target_duration == 0) result.fail("#EXT-X-TARGETDURATION must be > 0");
        } else if (starts_with(line, "#EXT-X-VERSION:")) {
            seen_version = true;
        } else if (starts_with(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            result.media_sequence = std::strtoull(line.substr(22).c_str(), nullptr, 10);
        } else if (starts_with(line, "#EXT-X-DISCONTINUITY-SEQUENCE:")) {
            result.discontinuity_sequence = std::strtoull(line.substr(30).c_str(), nullptr, 10);
        } else if (line == "#EXT-X-DISCONTINUITY") {
            if (pending_discontinuity) result.fail("two consecutive #EXT-X-DISCONTINUITY tags");
            pending_discontinuity = true;
            result.discontinuity_count += 1;
        } else if (starts_with(line, "#EXTINF:")) {
            if (pending_extinf) result.fail("#EXTINF not followed by a URI");
            pending_extinf = true;
            const std::string value = line.substr(8);
            // RFC 8216 4.3.2.1: EXTINF value must be terminated by a comma.
            if (value.find(',') == std::string::npos) {
                result.fail("#EXTINF must contain a comma after the duration");
            }
            pending_duration = std::strtod(value.c_str(), nullptr);
            if (pending_duration < 0) result.fail("negative #EXTINF duration");
        } else if (line == "#EXT-X-ENDLIST") {
            endlist_seen = true;
            result.has_endlist = true;
        } else if (line[0] == '#') {
            // Unknown tags/comments are legal and ignored.
        } else {
            // A URI line.
            if (!pending_extinf) {
                result.fail("segment URI '" + line + "' not preceded by #EXTINF");
            }
            result.segment_count += 1;
            result.durations.push_back(pending_duration);
            result.uris.push_back(line);
            pending_extinf = false;
            pending_discontinuity = false;
        }
    }

    if (pending_extinf) result.fail("trailing #EXTINF with no URI");
    if (!seen_target_duration) result.fail("missing required #EXT-X-TARGETDURATION");
    if (!seen_version) result.fail("missing #EXT-X-VERSION");

    // RFC 8216 4.3.3.1: every EXTINF duration, rounded to the nearest
    // integer, must be <= EXT-X-TARGETDURATION.
    for (double d : result.durations) {
        if (static_cast<std::uint32_t>(std::llround(d)) > result.target_duration) {
            result.fail("segment duration " + std::to_string(d) + " exceeds TARGETDURATION " +
                        std::to_string(result.target_duration));
        }
    }
    return result;
}

SegmentPtr make_segment(std::uint64_t sequence, std::chrono::milliseconds duration,
                        bool discontinuity = false, std::size_t bytes = 1024) {
    auto segment = std::make_shared<Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = duration;
    segment->discontinuity = discontinuity;
    segment->data = core::SharedBuffer::adopt(std::vector<std::byte>(bytes, std::byte{0x47}));
    return segment;
}

std::vector<SegmentPtr> make_segments(std::size_t count, std::chrono::milliseconds duration) {
    std::vector<SegmentPtr> segments;
    for (std::size_t i = 0; i < count; ++i) {
        segments.push_back(make_segment(i, duration));
    }
    return segments;
}

} // namespace

using namespace std::chrono_literals;

TEST(PlaylistTest, GeneratedLivePlaylistIsRfc8216Valid) {
    auto segments = make_segments(5, 4000ms);
    MediaPlaylistOptions options;
    options.target_duration_seconds = 4;

    const auto text = build_media_playlist(segments, options);
    auto validation = validate_media_playlist(text);

    ASSERT_TRUE(validation.valid) << text << "\nerrors: " << validation.errors.front();
    EXPECT_EQ(validation.segment_count, 5u);
    EXPECT_EQ(validation.target_duration, 4u);
    EXPECT_EQ(validation.media_sequence, 0u);
    EXPECT_FALSE(validation.has_endlist);
}

TEST(PlaylistTest, ValidatorRejectsAKnownBadPlaylist) {
    // Guards the guard: the validator must actually catch violations,
    // otherwise the tests above prove nothing.
    auto missing_target = validate_media_playlist("#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.000,\na.ts\n");
    EXPECT_FALSE(missing_target.valid);

    auto orphan_uri = validate_media_playlist(
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\nno-extinf.ts\n");
    EXPECT_FALSE(orphan_uri.valid);

    auto too_long = validate_media_playlist(
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:9.000,\na.ts\n");
    EXPECT_FALSE(too_long.valid);

    auto no_comma =
        validate_media_playlist("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0\na.ts\n");
    EXPECT_FALSE(no_comma.valid);
}

TEST(PlaylistTest, TargetDurationIsRaisedToCoverAnOverlongSegment) {
    // A publisher with a long GOP can overshoot the configured target; the
    // playlist must stay valid rather than advertising a too-small target.
    std::vector<SegmentPtr> segments{make_segment(0, 4000ms), make_segment(1, 9500ms)};
    MediaPlaylistOptions options;
    options.target_duration_seconds = 4;

    const auto text = build_media_playlist(segments, options);
    auto validation = validate_media_playlist(text);
    ASSERT_TRUE(validation.valid) << text;
    EXPECT_GE(validation.target_duration, 10u);
}

TEST(PlaylistTest, MediaSequenceTracksTheFirstSegmentInTheWindow) {
    std::vector<SegmentPtr> segments{make_segment(42, 4000ms), make_segment(43, 4000ms)};
    auto validation = validate_media_playlist(build_media_playlist(segments, {}));
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(validation.media_sequence, 42u);
}

TEST(PlaylistTest, DiscontinuityTagPrecedesTheSegmentItAppliesTo) {
    std::vector<SegmentPtr> segments{make_segment(0, 4000ms), make_segment(1, 4000ms, true),
                                     make_segment(2, 4000ms)};
    const auto text = build_media_playlist(segments, {});
    auto validation = validate_media_playlist(text);
    ASSERT_TRUE(validation.valid) << text;
    EXPECT_EQ(validation.discontinuity_count, 1u);

    // The tag must appear immediately before segment-1.ts, not after it.
    const auto tag = text.find("#EXT-X-DISCONTINUITY\n");
    const auto uri = text.find("segment-1.ts");
    ASSERT_NE(tag, std::string::npos);
    ASSERT_NE(uri, std::string::npos);
    EXPECT_LT(tag, uri);
    // And after segment-0.ts.
    EXPECT_GT(tag, text.find("segment-0.ts"));
}

TEST(PlaylistTest, DiscontinuitySequenceIsEmittedOnlyWhenNonZero) {
    auto segments = make_segments(2, 4000ms);
    EXPECT_EQ(build_media_playlist(segments, {}).find("#EXT-X-DISCONTINUITY-SEQUENCE"),
              std::string::npos);

    MediaPlaylistOptions options;
    options.discontinuity_sequence = 7;
    auto validation = validate_media_playlist(build_media_playlist(segments, options));
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(validation.discontinuity_sequence, 7u);
}

TEST(PlaylistTest, EndlistIsAppendedForAFinishedStream) {
    auto segments = make_segments(3, 4000ms);
    MediaPlaylistOptions options;
    options.ended = true;
    const auto text = build_media_playlist(segments, options);
    auto validation = validate_media_playlist(text);
    ASSERT_TRUE(validation.valid) << text;
    EXPECT_TRUE(validation.has_endlist);
    // ENDLIST must be the final tag.
    EXPECT_NE(text.rfind("#EXT-X-ENDLIST"), std::string::npos);
    EXPECT_GT(text.rfind("#EXT-X-ENDLIST"), text.rfind(".ts"));
}

TEST(PlaylistTest, SegmentUriPrefixIsApplied) {
    auto segments = make_segments(2, 4000ms);
    MediaPlaylistOptions options;
    options.segment_uri_prefix = "/hls/live/demo/";
    auto validation = validate_media_playlist(build_media_playlist(segments, options));
    ASSERT_TRUE(validation.valid);
    ASSERT_EQ(validation.uris.size(), 2u);
    EXPECT_EQ(validation.uris[0], "/hls/live/demo/segment-0.ts");
}

TEST(PlaylistTest, DurationsUseThreeDecimalsAndMatchTheSegments) {
    std::vector<SegmentPtr> segments{make_segment(0, 3937ms), make_segment(1, 4063ms)};
    const auto text = build_media_playlist(segments, {});
    EXPECT_NE(text.find("#EXTINF:3.937,"), std::string::npos);
    EXPECT_NE(text.find("#EXTINF:4.063,"), std::string::npos);
}

TEST(PlaylistTest, EmptySegmentListStillProducesAStructurallyValidPlaylist) {
    const auto text = build_media_playlist({}, {});
    auto validation = validate_media_playlist(text);
    EXPECT_TRUE(validation.valid) << text;
    EXPECT_EQ(validation.segment_count, 0u);
    EXPECT_GT(validation.target_duration, 0u);
}

TEST(PlaylistTest, NullSegmentPointersAreSkippedNotDereferenced) {
    std::vector<SegmentPtr> segments{make_segment(0, 4000ms), nullptr, make_segment(2, 4000ms)};
    auto validation = validate_media_playlist(build_media_playlist(segments, {}));
    ASSERT_TRUE(validation.valid);
    EXPECT_EQ(validation.segment_count, 2u);
}

// --- Master playlist ------------------------------------------------------

TEST(MasterPlaylistTest, ListsRenditionsLowestBandwidthFirstWithRequiredAttributes) {
    std::vector<Rendition> renditions;
    Rendition high;
    high.uri = "high/index.m3u8";
    high.bandwidth = 5'000'000;
    high.codecs = "avc1.64001f,mp4a.40.2";
    high.width = 1920;
    high.height = 1080;
    high.frame_rate = 30.0;
    high.name = "1080p";
    Rendition low;
    low.uri = "low/index.m3u8";
    low.bandwidth = 800'000;
    low.codecs = "avc1.42c01e,mp4a.40.2";
    low.width = 640;
    low.height = 360;
    low.name = "360p";
    renditions.push_back(high);
    renditions.push_back(low);

    const auto text = build_master_playlist(renditions);
    ASSERT_TRUE(text.rfind("#EXTM3U", 0) == 0);

    const auto low_pos = text.find("low/index.m3u8");
    const auto high_pos = text.find("high/index.m3u8");
    ASSERT_NE(low_pos, std::string::npos);
    ASSERT_NE(high_pos, std::string::npos);
    EXPECT_LT(low_pos, high_pos) << "renditions must be ordered lowest bandwidth first";

    // BANDWIDTH is the only required EXT-X-STREAM-INF attribute.
    EXPECT_NE(text.find("BANDWIDTH=800000"), std::string::npos);
    EXPECT_NE(text.find("RESOLUTION=1920x1080"), std::string::npos);
    EXPECT_NE(text.find("CODECS=\"avc1.64001f,mp4a.40.2\""), std::string::npos);
    EXPECT_NE(text.find("FRAME-RATE=30.000"), std::string::npos);

    // Every EXT-X-STREAM-INF must be followed by a URI line.
    const auto lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i], "#EXT-X-STREAM-INF:")) {
            ASSERT_LT(i + 1, lines.size());
            EXPECT_FALSE(lines[i + 1].empty());
            EXPECT_NE(lines[i + 1][0], '#');
        }
    }
}

TEST(MasterPlaylistTest, OptionalAttributesAreOmittedWhenUnset) {
    std::vector<Rendition> renditions;
    Rendition r;
    r.uri = "only/index.m3u8";
    r.bandwidth = 1'000'000;
    renditions.push_back(r);

    const auto text = build_master_playlist(renditions);
    EXPECT_NE(text.find("BANDWIDTH=1000000"), std::string::npos);
    EXPECT_EQ(text.find("RESOLUTION="), std::string::npos);
    EXPECT_EQ(text.find("CODECS="), std::string::npos);
    EXPECT_EQ(text.find("FRAME-RATE="), std::string::npos);
    EXPECT_EQ(text.find("AVERAGE-BANDWIDTH="), std::string::npos);
}
