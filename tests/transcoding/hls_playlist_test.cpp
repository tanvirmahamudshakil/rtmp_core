#include <gtest/gtest.h>

#include "rtmp_server/transcoding/native/hls_playlist.hpp"

using namespace rtmp_server::transcoding::native;

TEST(HlsPlaylist, ResolvesRelativeAbsoluteAndFullUrls) {
    const std::string base = "https://cdn.example.com/live/stream/index.m3u8";
    EXPECT_EQ(resolve_url(base, "seg1.ts"), "https://cdn.example.com/live/stream/seg1.ts");
    EXPECT_EQ(resolve_url(base, "/other/seg2.ts"), "https://cdn.example.com/other/seg2.ts");
    EXPECT_EQ(resolve_url(base, "https://other.cdn/seg3.ts"), "https://other.cdn/seg3.ts");
}

TEST(HlsPlaylist, DetectsAndParsesMasterPlaylist) {
    const std::string text =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\n"
        "low/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\n"
        "high/index.m3u8\n";
    EXPECT_TRUE(is_master_playlist(text));

    const auto variants = parse_master_playlist(text, "https://host/live/master.m3u8");
    ASSERT_EQ(variants.size(), 2U);
    EXPECT_EQ(variants[0].bandwidth, 800000U);
    EXPECT_EQ(variants[0].width, 640U);
    EXPECT_EQ(variants[0].height, 360U);
    EXPECT_EQ(variants[1].uri, "https://host/live/high/index.m3u8");

    EXPECT_EQ(select_variant(variants, 0), "https://host/live/high/index.m3u8");        // highest
    EXPECT_EQ(select_variant(variants, 1000000), "https://host/live/low/index.m3u8");   // under cap
    EXPECT_EQ(select_variant(variants, 500000), "https://host/live/high/index.m3u8");   // none under -> highest
}

TEST(HlsPlaylist, ParsesLiveMediaPlaylist) {
    const std::string text =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXTINF:4.000,\n"
        "seg10.ts\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:3.960,\n"
        "seg11.ts\n";
    EXPECT_FALSE(is_master_playlist(text));

    const auto playlist = parse_media_playlist(text, "https://host/live/index.m3u8");
    EXPECT_EQ(playlist.target_duration, 4.0);
    EXPECT_EQ(playlist.media_sequence, 10U);
    EXPECT_FALSE(playlist.endlist); // live: no ENDLIST
    ASSERT_EQ(playlist.segments.size(), 2U);
    EXPECT_EQ(playlist.segments[0].uri, "https://host/live/seg10.ts");
    EXPECT_EQ(playlist.segments[0].sequence, 10U);
    EXPECT_NEAR(playlist.segments[0].duration, 4.0, 1e-6);
    EXPECT_FALSE(playlist.segments[0].discontinuity);
    EXPECT_EQ(playlist.segments[1].sequence, 11U);
    EXPECT_TRUE(playlist.segments[1].discontinuity);
}

TEST(HlsPlaylist, DetectsVodEndlist) {
    const std::string text =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6.0,\na.ts\n#EXT-X-ENDLIST\n";
    const auto playlist = parse_media_playlist(text, "https://host/vod/index.m3u8");
    EXPECT_TRUE(playlist.endlist);
    ASSERT_EQ(playlist.segments.size(), 1U);
    EXPECT_EQ(playlist.segments[0].uri, "https://host/vod/a.ts");
}
