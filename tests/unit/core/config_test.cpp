#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "rtmp_server/core/config.hpp"

namespace rtmp_server::core {
namespace {

class ConfigTest : public ::testing::Test {
protected:
    void write_config(const std::string& contents) {
        path_ = std::tmpnam(nullptr);
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
        "token_signing_secret: real-secret-value\n"
        "api_authentication_secret: another-real-secret\n");

    auto result = load_config(path_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().rtmp_port, 1936);
    EXPECT_EQ(result.value().api_port, 8081);
    EXPECT_EQ(result.value().handshake_timeout, std::chrono::milliseconds(7000));
}

TEST(ServerConfigValidate, RejectsZeroPorts) {
    ServerConfig cfg;
    cfg.token_signing_secret = "x";
    cfg.api_authentication_secret = "y";
    cfg.rtmp_port = 0;
    EXPECT_FALSE(cfg.validate().ok());
}

} // namespace
} // namespace rtmp_server::core
