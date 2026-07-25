#include "rtmp_server/management/url_builder.hpp"

#include <gtest/gtest.h>

namespace rtmp_server::management {
namespace {

TEST(UrlBuilderTest, PublishUrlUsesStreamKeyAsPathSegment) {
    EXPECT_EQ(build_publish_url("stream.example.com", 1935, "live", "sk_abc123"),
              "rtmp://stream.example.com:1935/live/sk_abc123");
}

TEST(UrlBuilderTest, PlaybackUrlUsesStreamNameAsPathSegment) {
    EXPECT_EQ(build_playback_url("stream.example.com", 1935, "live", "my-show"),
              "rtmp://stream.example.com:1935/live/my-show");
}

TEST(UrlBuilderTest, PlaybackUrlNeverContainsTheStreamKey) {
    // Regression guard for the exact security property docs/rtmp_promot.md
    // calls out: publish and playback path segments must differ.
    auto publish = build_publish_url("host", 1935, "live", "sk_secret");
    auto playback = build_playback_url("host", 1935, "live", "public-name");
    EXPECT_EQ(playback.find("sk_secret"), std::string::npos);
    EXPECT_NE(publish, playback);
}

TEST(UrlBuilderTest, AppendSignedTokenAddsTokenAndExpiresQueryParams) {
    auto url = append_signed_token("rtmp://host:1935/live/name", "deadbeef", 1735689600);
    EXPECT_EQ(url, "rtmp://host:1935/live/name?token=deadbeef&expires=1735689600");
}

} // namespace
} // namespace rtmp_server::management
