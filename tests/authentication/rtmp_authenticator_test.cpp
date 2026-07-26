#include "rtmp_server/authentication/rtmp_authenticator.hpp"

#include <chrono>

#include <gtest/gtest.h>

using rtmp_server::authentication::AuthenticatorLimits;
using rtmp_server::authentication::RtmpAuthenticator;
using rtmp_server::management::StreamManager;

namespace {

StreamManager::Options test_options() {
    StreamManager::Options options;
    options.public_hostname = "localhost";
    options.rtmp_port = 1935;
    options.token_signing_secret = "unit-test-secret";
    return options;
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

TEST(RtmpAuthenticatorTest, ValidPublishKeyIsAccepted) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "alpha");
    ASSERT_TRUE(created.ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto validator = auth.key_validator();
    EXPECT_TRUE(validator("live", created.value().stream_key));
}

TEST(RtmpAuthenticatorTest, InvalidPublishKeyIsRejected) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto validator = auth.key_validator();
    EXPECT_FALSE(validator("live", "not-the-real-key"));
}

TEST(RtmpAuthenticatorTest, RotatedKeyInvalidatesThePreviousOne) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "alpha");
    ASSERT_TRUE(created.ok());
    std::string old_key = created.value().stream_key;

    auto rotated = manager.rotate_key("live", "alpha");
    ASSERT_TRUE(rotated.ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto validator = auth.key_validator();
    EXPECT_FALSE(validator("live", old_key));
    EXPECT_TRUE(validator("live", rotated.value()));
}

TEST(RtmpAuthenticatorTest, ResolverMapsPublishKeyToPublicStreamName) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "alpha");
    ASSERT_TRUE(created.ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto resolver = auth.stream_id_resolver();
    auto resolved = resolver("live", created.value().stream_key);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, "alpha");

    EXPECT_FALSE(resolver("live", "bogus-key").has_value());
}

TEST(RtmpAuthenticatorTest, PlaybackWithoutTokenIsAllowedWhenStreamEnabled) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto authorizer = auth.playback_authorizer();
    EXPECT_TRUE(authorizer("live", "alpha", "", "203.0.113.1"));
}

TEST(RtmpAuthenticatorTest, PlaybackWithValidTokenIsAllowed) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    std::int64_t expires = now_unix() + 3600;
    std::string token = manager.sign_playback_token("live", "alpha", expires);
    std::string query = "token=" + token + "&expires=" + std::to_string(expires);

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto authorizer = auth.playback_authorizer();
    EXPECT_TRUE(authorizer("live", "alpha", query, "203.0.113.1"));
}

TEST(RtmpAuthenticatorTest, PlaybackWithExpiredTokenIsRejected) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    std::int64_t expires = now_unix() - 10; // already expired
    std::string token = manager.sign_playback_token("live", "alpha", expires);
    std::string query = "token=" + token + "&expires=" + std::to_string(expires);

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto authorizer = auth.playback_authorizer();
    EXPECT_FALSE(authorizer("live", "alpha", query, "203.0.113.1"));
}

TEST(RtmpAuthenticatorTest, PlaybackWithModifiedTokenIsRejected) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    std::int64_t expires = now_unix() + 3600;
    std::string token = manager.sign_playback_token("live", "alpha", expires);
    token.back() = (token.back() == 'a') ? 'b' : 'a'; // tamper with the signature
    std::string query = "token=" + token + "&expires=" + std::to_string(expires);

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto authorizer = auth.playback_authorizer();
    EXPECT_FALSE(authorizer("live", "alpha", query, "203.0.113.1"));
}

TEST(RtmpAuthenticatorTest, PlaybackOnDisabledStreamIsRejected) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());
    ASSERT_TRUE(manager.set_enabled("live", "alpha", false).ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto authorizer = auth.playback_authorizer();
    EXPECT_FALSE(authorizer("live", "alpha", "", "203.0.113.1"));
}

TEST(RtmpAuthenticatorTest, PublishKeyForUnknownApplicationIsRejected) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    RtmpAuthenticator auth(manager, AuthenticatorLimits{});
    auto validator = auth.key_validator();
    // Publishing into a disabled application must fail; StreamManager has
    // no direct "disable application" API distinct from delete, so this
    // exercises the already-covered application-not-found path via a
    // never-created application name instead.
    EXPECT_FALSE(validator("nonexistent-app", "any-key"));
}

TEST(RtmpAuthenticatorTest, ViewerLimitIsEnforced) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());

    AuthenticatorLimits limits;
    limits.max_viewers_per_stream = 1;
    RtmpAuthenticator auth(manager, limits);
    auto authorizer = auth.playback_authorizer();

    EXPECT_TRUE(authorizer("live", "alpha", "", "203.0.113.1"));
    auth.on_viewer_attached("live", "alpha");
    EXPECT_FALSE(authorizer("live", "alpha", "", "203.0.113.2"));
    auth.on_viewer_detached("live", "alpha");
    EXPECT_TRUE(authorizer("live", "alpha", "", "203.0.113.3"));
}

TEST(RtmpAuthenticatorTest, PerIpConnectionLimitIsEnforced) {
    StreamManager manager(test_options());
    AuthenticatorLimits limits;
    limits.max_connections_per_ip = 2;
    RtmpAuthenticator auth(manager, limits);

    EXPECT_TRUE(auth.admit_connection("198.51.100.5"));
    EXPECT_TRUE(auth.admit_connection("198.51.100.5"));
    EXPECT_FALSE(auth.admit_connection("198.51.100.5"));

    auth.release_connection("198.51.100.5");
    EXPECT_TRUE(auth.admit_connection("198.51.100.5"));
}

TEST(RtmpAuthenticatorTest, RepeatedAuthFailuresLockOutTheIp) {
    StreamManager manager(test_options());
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "alpha").ok());
    ASSERT_TRUE(manager.set_enabled("live", "alpha", false).ok()); // every attempt fails

    AuthenticatorLimits limits;
    limits.max_auth_failures_per_ip = 3;
    limits.auth_failure_window = std::chrono::seconds(60);
    RtmpAuthenticator auth(manager, limits);
    auto authorizer = auth.playback_authorizer();

    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(authorizer("live", "alpha", "", "192.0.2.9"));
    }
    EXPECT_EQ(auth.auth_failure_count("192.0.2.9"), 3u);

    // Even a request that would otherwise succeed is now locked out.
    ASSERT_TRUE(manager.set_enabled("live", "alpha", true).ok());
    EXPECT_FALSE(authorizer("live", "alpha", "", "192.0.2.9"));
}
