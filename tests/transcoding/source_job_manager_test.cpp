#include <gtest/gtest.h>

#ifdef RTMP_NATIVE_TRANSCODE

#include <chrono>
#include <string>

#include "rtmp_server/transcoding/native/source_job_manager.hpp"

namespace {

using namespace std::chrono_literals;
using rtmp_server::transcoding::native::SourceJobConfig;
using rtmp_server::transcoding::native::SourceJobManager;
using rtmp_server::transcoding::native::SourceJobOptions;

SourceJobConfig config_with_delay(std::uint32_t seconds) {
    SourceJobConfig config;
    config.application = "live";
    config.name = "restream";
    config.source_url = "https://example.invalid/index.m3u8";
    config.restart_delay_seconds = seconds;
    return config;
}

TEST(SourceJobBackoffTest, FirstRetryUsesTheConfiguredDelay) {
    EXPECT_EQ(SourceJobManager::restart_delay_for(config_with_delay(5), SourceJobOptions{}, 0), 5s);
}

TEST(SourceJobBackoffTest, DoublesPerConsecutiveFailureUpToTheCap) {
    const auto config = config_with_delay(5);
    SourceJobOptions options; // cap 60s, growth stops after 5 attempts
    EXPECT_EQ(SourceJobManager::restart_delay_for(config, options, 1), 10s);
    EXPECT_EQ(SourceJobManager::restart_delay_for(config, options, 2), 20s);
    EXPECT_EQ(SourceJobManager::restart_delay_for(config, options, 3), 40s);
    EXPECT_EQ(SourceJobManager::restart_delay_for(config, options, 4), 60s); // 80s, clamped
}

// The point of the backoff is patience, not surrender: a source that has been
// unreachable for hours must still be retried, so that it comes back on its
// own the moment the upstream returns.
TEST(SourceJobBackoffTest, NeverStopsRetryingAfterManyFailures) {
    const auto config = config_with_delay(5);
    SourceJobOptions options;
    EXPECT_EQ(SourceJobManager::restart_delay_for(config, options, 50), 60s);
    EXPECT_EQ(SourceJobManager::restart_delay_for(config, options, 100000), 60s);
}

TEST(SourceJobBackoffTest, NeverReturnsLessThanOneSecondForAZeroDelayConfig) {
    EXPECT_EQ(SourceJobManager::restart_delay_for(config_with_delay(0), SourceJobOptions{}, 0), 1s);
}

// A configured delay above the cap is honoured rather than shortened: the
// operator asked for a slower retry than the default ceiling.
TEST(SourceJobBackoffTest, ConfiguredDelayAboveTheCapIsNotShortened) {
    SourceJobOptions options;
    options.restart_backoff_cap_seconds = 60;
    EXPECT_EQ(SourceJobManager::restart_delay_for(config_with_delay(300), options, 0), 300s);
    EXPECT_EQ(SourceJobManager::restart_delay_for(config_with_delay(300), options, 4), 300s);
}

using rtmp_server::transcoding::native::parse_source_job_renditions;

// "app/src | preset | output | default | <vcodec> | <vbitrate> | high | source
//  | <w> | <h> | match-source | <acodec> | <abitrate> | first | desc"
constexpr const char* kCopyRule =
    "live/cam|copy|restream_src|default|passthrough|0|high|source|||match-source|passthrough|0|first|copy";
constexpr const char* kEncodedRule =
    "live/cam|480p|restream_480p|default|h264|900000|main|60|854|480|letterbox|aac|96000|first|Mobile";

TEST(SourceJobPassthroughTest, CopyRuleYieldsOnePassthroughRendition) {
    auto parsed = parse_source_job_renditions(kCopyRule);
    ASSERT_TRUE(parsed) << parsed.error().message();
    ASSERT_EQ(parsed.value().size(), 1u);
    EXPECT_TRUE(parsed.value().front().passthrough);
}

TEST(SourceJobPassthroughTest, EncodedRuleIsNotMarkedPassthrough) {
    auto parsed = parse_source_job_renditions(kEncodedRule);
    ASSERT_TRUE(parsed) << parsed.error().message();
    ASSERT_EQ(parsed.value().size(), 1u);
    EXPECT_FALSE(parsed.value().front().passthrough);
}

TEST(SourceJobPassthroughTest, PassthroughVideoWithReencodedAudioIsRejected) {
    const std::string rule =
        "live/cam|copy|restream_src|default|passthrough|0|high|source|||match-source|aac|96000|first|copy";
    auto parsed = parse_source_job_renditions(rule);
    EXPECT_FALSE(parsed);
}

TEST(SourceJobPassthroughTest, PassthroughCannotCoexistWithAnEncodedRung) {
    const std::string rules = std::string(kCopyRule) + "\n" + kEncodedRule;
    auto parsed = parse_source_job_renditions(rules);
    EXPECT_FALSE(parsed);
}

} // namespace

#endif // RTMP_NATIVE_TRANSCODE
