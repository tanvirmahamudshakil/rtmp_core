#include "rtmp_server/management/stream_manager.hpp"

#include <gtest/gtest.h>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::management {
namespace {

using protocol::commands::LiveFanout;
using protocol::commands::StreamRegistry;

StreamManager::Options test_options() {
    StreamManager::Options options;
    options.public_hostname = "stream.example.com";
    options.rtmp_port = 1935;
    options.token_signing_secret = "test-signing-secret";
    return options;
}

class StreamManagerTest : public ::testing::Test {
protected:
    StreamManager manager{test_options()};
};

TEST_F(StreamManagerTest, CreateApplicationSucceedsOnceAndRejectsDuplicate) {
    EXPECT_TRUE(manager.create_application("live").ok());
    auto dup = manager.create_application("live");
    ASSERT_FALSE(dup.ok());
    EXPECT_EQ(dup.error().code(), core::ErrorCode::Conflict);
}

TEST_F(StreamManagerTest, CreateStreamFailsWithoutAnApplication) {
    auto result = manager.create_stream("live", "my-show");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::NotFound);
}

TEST_F(StreamManagerTest, ApiCreatesStreamsWithSecureUrls) {
    ASSERT_TRUE(manager.create_application("live").ok());

    auto result = manager.create_stream("live", "my-show", /*recording_enabled=*/true);
    ASSERT_TRUE(result.ok());
    const auto& created = result.value();

    EXPECT_EQ(created.stream.application, "live");
    EXPECT_EQ(created.stream.name, "my-show");
    EXPECT_TRUE(created.stream.enabled);
    EXPECT_TRUE(created.stream.recording_enabled);
    EXPECT_FALSE(created.stream_key.empty());
    EXPECT_EQ(created.publish_url, "rtmp://stream.example.com:1935/live/" + created.stream_key);
    EXPECT_EQ(created.playback_url, "rtmp://stream.example.com:1935/live/my-show");

    // The raw key must actually work as a publish credential.
    EXPECT_TRUE(manager.validate_publish_key("live", created.stream_key));

    // Duplicate stream name in the same application is rejected.
    auto dup = manager.create_stream("live", "my-show");
    ASSERT_FALSE(dup.ok());
    EXPECT_EQ(dup.error().code(), core::ErrorCode::Conflict);
}

TEST_F(StreamManagerTest, KeyRotationInvalidatesTheOldKeyAndActivatesTheNewOne) {
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "my-show").value();
    ASSERT_TRUE(manager.validate_publish_key("live", created.stream_key));

    auto rotated = manager.rotate_key("live", "my-show");
    ASSERT_TRUE(rotated.ok());
    EXPECT_NE(rotated.value(), created.stream_key);

    EXPECT_FALSE(manager.validate_publish_key("live", created.stream_key));
    EXPECT_TRUE(manager.validate_publish_key("live", rotated.value()));
}

TEST_F(StreamManagerTest, DisabledStreamRejectsPublishKeyEvenThoughItIsCorrect) {
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "my-show").value();
    ASSERT_TRUE(manager.validate_publish_key("live", created.stream_key));

    ASSERT_TRUE(manager.set_enabled("live", "my-show", false).ok());

    EXPECT_FALSE(manager.validate_publish_key("live", created.stream_key));

    auto stream = manager.find_stream("live", "my-show");
    ASSERT_TRUE(stream.has_value());
    EXPECT_FALSE(stream->enabled);
}

TEST_F(StreamManagerTest, TokenExpiryIsEnforcedThroughTheManager) {
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());

    auto token = manager.sign_playback_token("live", "my-show", /*expires_at_unix=*/1000);
    EXPECT_TRUE(manager.verify_playback_token("live", "my-show", token, 1000, /*now_unix=*/500).ok());

    auto expired = manager.verify_playback_token("live", "my-show", token, 1000, /*now_unix=*/1500);
    ASSERT_FALSE(expired.ok());
    EXPECT_EQ(expired.error().code(), core::ErrorCode::ExpiredToken);
}

TEST_F(StreamManagerTest, PlaybackTokenForADisabledStreamIsRejected) {
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());
    ASSERT_TRUE(manager.set_enabled("live", "my-show", false).ok());

    auto token = manager.sign_playback_token("live", "my-show", 2000000000);
    auto result = manager.verify_playback_token("live", "my-show", token, 2000000000, 1000000000);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::Unauthorized);
}

TEST_F(StreamManagerTest, PublisherCanBeDisconnectedByApi) {
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "my-show").value();

    StreamRegistry registry;
    ASSERT_TRUE(registry.register_publisher("live", created.stream_key, /*connection_id=*/42, /*stream_id=*/1));

    std::vector<std::uint64_t> disconnected;
    manager.set_publisher_disconnect_handler([&](std::uint64_t id) { disconnected.push_back(id); });

    auto result = manager.disconnect_publisher("live", "my-show", registry);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(disconnected.size(), 1u);
    EXPECT_EQ(disconnected[0], 42u);
}

TEST_F(StreamManagerTest, DisconnectPublisherFailsWhenStreamIsNotCurrentlyLive) {
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());

    StreamRegistry registry;
    auto result = manager.disconnect_publisher("live", "my-show", registry);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::NotFound);
}

TEST_F(StreamManagerTest, ViewerSessionsCanBeDisconnectedByApi) {
    ASSERT_TRUE(manager.create_application("live").ok());
    auto created = manager.create_stream("live", "my-show").value();

    StreamRegistry registry;
    ASSERT_TRUE(registry.register_publisher("live", created.stream_key, /*connection_id=*/42, /*stream_id=*/1));

    std::vector<std::string> disconnected_keys;
    manager.set_viewer_disconnect_handler(
        [&](std::string_view key) { disconnected_keys.emplace_back(key); });

    auto result = manager.disconnect_viewers("live", "my-show", registry);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(disconnected_keys.size(), 1u);
    EXPECT_EQ(disconnected_keys[0], created.stream_key);
}

TEST_F(StreamManagerTest, LiveStateReflectsPublishStatusAndViewerCount) {
    ASSERT_TRUE(manager.create_application("live").ok());
    auto live_created = manager.create_stream("live", "on-air").value();
    ASSERT_TRUE(manager.create_stream("live", "offline").ok());

    StreamRegistry registry;
    LiveFanout fanout;
    ASSERT_TRUE(registry.register_publisher("live", live_created.stream_key, 42, 1));
    fanout.subscribe(live_created.stream_key, /*subscriber_id=*/1, nullptr);

    auto states = manager.live_state(registry, fanout);
    ASSERT_EQ(states.size(), 2u);

    for (const auto& state : states) {
        if (state.name == "on-air") {
            EXPECT_TRUE(state.is_live);
        } else {
            EXPECT_EQ(state.name, "offline");
            EXPECT_FALSE(state.is_live);
            EXPECT_EQ(state.viewer_count, 0u);
        }
    }
}

TEST_F(StreamManagerTest, DeleteStreamRemovesItFromListStreams) {
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());
    ASSERT_EQ(manager.list_streams("live").size(), 1u);

    ASSERT_TRUE(manager.delete_stream("live", "my-show").ok());
    EXPECT_EQ(manager.list_streams("live").size(), 0u);
    EXPECT_FALSE(manager.find_stream("live", "my-show").has_value());
}

TEST_F(StreamManagerTest, DeleteApplicationFailsWhileStreamsExist) {
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());

    auto result = manager.delete_application("live");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::Conflict);

    ASSERT_TRUE(manager.delete_stream("live", "my-show").ok());
    EXPECT_TRUE(manager.delete_application("live").ok());
}

} // namespace
} // namespace rtmp_server::management
