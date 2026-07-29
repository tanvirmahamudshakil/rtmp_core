#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "rtmp_server/transcoding/preset.hpp"

namespace {

class TemporaryPresetFile {
public:
    explicit TemporaryPresetFile(std::string_view contents) {
        path_ = std::filesystem::temp_directory_path() /
                ("streamforge-transcoding-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++sequence_) + ".conf");
        std::ofstream output(path_);
        output << contents;
    }
    ~TemporaryPresetFile() { std::filesystem::remove(path_); }
    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    static inline std::uint64_t sequence_ = 0;
    std::filesystem::path path_;
};

TEST(TranscodingPreset, LoadsExactAndWildcardRulesWithExactPrecedence) {
    TemporaryPresetFile file(
        "football/*|mobile|{source}_mobile|default|h264|900000|main|60|854|480|letterbox|aac|96000|first|Mobile\n"
        "football/live2|hd|live2_hd|nvenc|h264|3500000|high|source|1280|720|letterbox|aac|128000|0|HD\n");

    auto catalogue = rtmp_server::transcoding::PresetCatalogue::load(file.string());
    ASSERT_TRUE(catalogue.ok()) << catalogue.error().message();

    auto exact = catalogue.value().match("football", "live2");
    ASSERT_EQ(exact.size(), 1U);
    EXPECT_EQ(exact.front().name, "hd");
    EXPECT_EQ(exact.front().backend, rtmp_server::transcoding::BackendKind::Nvenc);
    ASSERT_TRUE(exact.front().gpu_id.has_value());
    EXPECT_EQ(*exact.front().gpu_id, 0U);

    auto wildcard = catalogue.value().match("football", "live3");
    ASSERT_EQ(wildcard.size(), 1U);
    EXPECT_EQ(wildcard.front().name, "mobile");
    EXPECT_EQ(wildcard.front().outgoing_stream_name, "live3_mobile");
}

TEST(TranscodingPreset, RejectsSourceOutputLoop) {
    TemporaryPresetFile file(
        "football/live2|hd|live2|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first\n");
    auto catalogue = rtmp_server::transcoding::PresetCatalogue::load(file.string());
    ASSERT_FALSE(catalogue.ok());
    EXPECT_NE(catalogue.error().message().find("must not equal"), std::string::npos);
}

TEST(TranscodingPreset, RejectsUnboundedOrInvalidValues) {
    TemporaryPresetFile file(
        "football/live2|hd|out|default|h264|0|high|60|1280|720|letterbox|aac|128000|first\n");
    auto catalogue = rtmp_server::transcoding::PresetCatalogue::load(file.string());
    ASSERT_FALSE(catalogue.ok());
    EXPECT_NE(catalogue.error().message().find("video bitrate"), std::string::npos);
}

} // namespace
