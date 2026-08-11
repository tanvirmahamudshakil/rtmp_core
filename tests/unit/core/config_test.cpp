#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include <unistd.h>
#include <string>

#include "rtmp_server/core/config.hpp"

namespace rtmp_server::core {
namespace {

class ConfigTest : public ::testing::Test {
protected:
    void write_config(const std::string& contents) {
        // mkstemp rather than tmpnam: tmpnam returns a name, not a
        // reservation, so another process can win the race between the name
        // being handed out and this test opening it.
        char tmpl[] = "/tmp/rtmp_server_config_testXXXXXX";
        const int fd = ::mkstemp(tmpl);
        ASSERT_GE(fd, 0);
        ::close(fd);
        path_ = tmpl;
        std::ofstream out(path_);
        out << contents;
    }

    void TearDown() override {
        if (!path_.empty()) std::remove(path_.c_str());
    }

    std::string path_;
};

TEST_F(ConfigTest, MissingFileReturnsError) {
    auto result = load_config("/nonexistent/path/server.yaml");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::MissingConfiguration);
}

TEST_F(ConfigTest, RejectsDefaultChangeMeSecrets) {
    write_config(
        "rtmp_port: 1935\n"
        "api_port: 8080\n"
        "token_signing_secret: \"CHANGE_ME\"\n"
        "api_authentication_secret: \"CHANGE_ME\"\n");

    auto result = load_config(path_);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidConfiguration);
}

TEST_F(ConfigTest, LoadsValidConfigWithOverrides) {
    write_config(
        "rtmp_port: 1936\n"
        "api_port: 8081\n"
        "handshake_timeout: 7s\n"
        "hls_high_scale_mode: false\n"
        "token_signing_secret: 4f3c1d9a8b7e6520f1a2b3c4d5e6f708\n"
        "api_authentication_secret: 9a8b7c6d5e4f30211f2e3d4c5b6a7988\n");

    auto result = load_config(path_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().rtmp_port, 1936);
    EXPECT_EQ(result.value().api_port, 8081);
    EXPECT_EQ(result.value().handshake_timeout, std::chrono::milliseconds(7000));
    EXPECT_FALSE(result.value().hls_high_scale_mode);
}

TEST(ServerConfigValidate, RejectsZeroPorts) {
    ServerConfig cfg;
    cfg.token_signing_secret = "x";
    cfg.api_authentication_secret = "y";
    cfg.rtmp_port = 0;
    EXPECT_FALSE(cfg.validate().ok());
}

// --- Phase 8 release gates -----------------------------------------------
//
// "Required configuration is missing" and "unsupported insecure defaults are
// used" must both fail startup rather than warn, so a misconfigured
// deployment can never reach a listening state.

namespace {

// A config that passes validate(), so each test below can change exactly one
// field and attribute the failure to that field.
ServerConfig valid_config() {
    ServerConfig cfg;
    cfg.token_signing_secret = "4f3c1d9a8b7e6520f1a2b3c4d5e6f708";
    cfg.api_authentication_secret = "9a8b7c6d5e4f30211f2e3d4c5b6a7988";
    return cfg;
}

} // namespace

TEST(ServerConfigValidate, BaselineConfigIsValid) {
    EXPECT_TRUE(valid_config().validate().ok());
}

TEST(ServerConfigValidate, RejectsInvalidIoUringBatchSizes) {
    auto cfg = valid_config();
    cfg.completion_batch_size = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.completion_batch_size = cfg.ring_queue_depth + 1;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.submission_batch_size = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.submission_batch_size = cfg.ring_queue_depth + 1;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsSecretsShorterThanTheMinimum) {
    auto cfg = valid_config();
    cfg.token_signing_secret = std::string(kMinSecretLength - 1, 'a');
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.api_authentication_secret = "short";
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsWellKnownPlaceholderSecrets) {
    for (const auto* placeholder : {"CHANGE_ME", "changeme", "secret", "password", "TODO", "REPLACE_ME"}) {
        auto cfg = valid_config();
        cfg.token_signing_secret = placeholder;
        EXPECT_FALSE(cfg.validate().ok()) << placeholder;
    }
}

TEST(ServerConfigValidate, RejectsZeroEntropySecrets) {
    auto cfg = valid_config();
    cfg.token_signing_secret = std::string(kMinSecretLength + 8, 'a');
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsReusingOneSecretForBothPurposes) {
    auto cfg = valid_config();
    cfg.api_authentication_secret = cfg.token_signing_secret;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsUnboundedOrZeroMessageSize) {
    auto cfg = valid_config();
    cfg.maximum_rtmp_message_size = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.maximum_rtmp_message_size = kMaxSupportedRtmpMessageSize + 1;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.maximum_rtmp_message_size = kMaxSupportedRtmpMessageSize;
    EXPECT_TRUE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsUnboundedRemoteControlledQueues) {
    // A zero bound reads as "unlimited", which is the exact failure mode
    // docs/v2_promot.md section 3.5 exists to prevent.
    auto cfg = valid_config();
    cfg.gop_cache_max_bytes = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.gop_cache_max_packets = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.subscriber_queue_max_bytes = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.subscriber_queue_max_packets = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.maximum_publishers = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.maximum_viewers_per_stream = 0;
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsNonPositiveTimeouts) {
    auto cfg = valid_config();
    cfg.idle_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.handshake_timeout = std::chrono::milliseconds{-1};
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RejectsMissingRequiredPaths) {
    auto cfg = valid_config();
    cfg.recording_enabled = true;
    cfg.recording_directory.clear();
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.database_connection.clear();
    EXPECT_FALSE(cfg.validate().ok());
}

TEST(ServerConfigValidate, RecordingDirectoryOnlyRequiredWhenRecordingIsEnabled) {
    auto cfg = valid_config();
    cfg.recording_enabled = false;
    cfg.recording_directory.clear();
    EXPECT_TRUE(cfg.validate().ok());
}

TEST(ServerConfigValidate, TranscodingRequiresBoundedWorkerConfiguration) {
    auto cfg = valid_config();
    cfg.transcoding_enabled = true;
    cfg.transcoding_preset_file.clear();
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.transcoding_enabled = true;
    cfg.transcoding_ffmpeg_path.clear();
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.transcoding_max_active_jobs = 0;
    EXPECT_FALSE(cfg.validate().ok());

    cfg = valid_config();
    cfg.transcoding_max_outputs_per_job = 33;
    EXPECT_FALSE(cfg.validate().ok());
}

} // namespace
} // namespace rtmp_server::core
