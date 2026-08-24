#include <gtest/gtest.h>

#ifdef RTMP_NATIVE_TRANSCODE

#include <chrono>

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

} // namespace

#endif // RTMP_NATIVE_TRANSCODE
