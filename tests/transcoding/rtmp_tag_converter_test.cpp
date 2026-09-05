#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "rtmp_server/transcoding/native/rtmp_tag_converter.hpp"

namespace {

using rtmp_server::core::ErrorCode;
using rtmp_server::transcoding::native::RtmpTagConverter;
using rtmp_server::transcoding::native::SourceVideoCodec;

void append(std::vector<std::byte>& out, std::initializer_list<int> bytes) {
    for (const int byte : bytes) out.push_back(static_cast<std::byte>(byte));
}

// An AVCDecoderConfigurationRecord with one SPS and one PPS, wrapped in the
// FLV video tag a publisher sends first.
std::vector<std::byte> avc_sequence_header() {
    std::vector<std::byte> tag;
    append(tag, {0x17, 0x00, 0x00, 0x00, 0x00}); // keyframe/AVC, sequence header, CTS 0
    append(tag, {0x01, 0x64, 0x00, 0x1F});       // version, profile, compat, level
    append(tag, {0xFF});                          // lengthSizeMinusOne = 3 (4-byte NAL lengths)
    append(tag, {0xE1, 0x00, 0x03, 0x67, 0x64, 0x1F}); // 1 SPS, 3 bytes
    append(tag, {0x01, 0x00, 0x02, 0x68, 0xEE});       // 1 PPS, 2 bytes
    return tag;
}

// One AVCC-framed picture: a 4-byte length followed by that many NAL bytes.
std::vector<std::byte> avc_frame(bool keyframe, std::int32_t composition_time_ms) {
    std::vector<std::byte> tag;
    append(tag, {keyframe ? 0x17 : 0x27, 0x01});
    append(tag, {(composition_time_ms >> 16) & 0xFF, (composition_time_ms >> 8) & 0xFF,
                 composition_time_ms & 0xFF});
    append(tag, {0x00, 0x00, 0x00, 0x02, 0x65, 0x88}); // length 2, IDR slice
    return tag;
}

std::vector<std::byte> aac_sequence_header() {
    std::vector<std::byte> tag;
    append(tag, {0xAF, 0x00}); // AAC, sequence header
    append(tag, {0x12, 0x10}); // AAC-LC, 44100 Hz, stereo
    return tag;
}

std::vector<std::byte> aac_frame() {
    std::vector<std::byte> tag;
    append(tag, {0xAF, 0x01});
    append(tag, {0x21, 0x00, 0x03});
    return tag;
}

TEST(RtmpTagConverterTest, SequenceHeaderYieldsNoUnitButArmsTheConverter) {
    RtmpTagConverter converter;
    auto result = converter.convert_video(avc_sequence_header(), 0);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().has_value());
    EXPECT_TRUE(converter.has_video_config());
    EXPECT_TRUE(converter.has_video_codec());
    EXPECT_EQ(converter.video_codec(), SourceVideoCodec::H264);
}

// A frame before its configuration record cannot be converted at all; the
// caller has to know that rather than receive an empty unit.
TEST(RtmpTagConverterTest, RejectsAFrameThatArrivesBeforeItsSequenceHeader) {
    RtmpTagConverter converter;
    auto result = converter.convert_video(avc_frame(true, 0), 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidStateTransition);
}

TEST(RtmpTagConverterTest, ConvertsAvccToAnnexBWithParameterSetsOnAKeyframe) {
    RtmpTagConverter converter;
    ASSERT_TRUE(converter.convert_video(avc_sequence_header(), 0));

    auto result = converter.convert_video(avc_frame(true, 0), 1000);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().has_value());
    const auto& unit = *result.value();
    EXPECT_TRUE(unit.keyframe);

    // Start code, then the SPS and PPS the record carried, so a decoder or a
    // TS demuxer can join at this keyframe.
    ASSERT_GE(unit.annexb.size(), 4u);
    EXPECT_EQ(unit.annexb[0], std::byte{0x00});
    EXPECT_EQ(unit.annexb[3], std::byte{0x01});
    const std::vector<std::byte> bytes(unit.annexb.begin(), unit.annexb.end());
    const auto contains = [&bytes](std::byte value) {
        return std::find(bytes.begin(), bytes.end(), value) != bytes.end();
    };
    EXPECT_TRUE(contains(std::byte{0x67})); // SPS
    EXPECT_TRUE(contains(std::byte{0x68})); // PPS
    EXPECT_TRUE(contains(std::byte{0x65})); // the slice itself
}

// RTMP timestamps are milliseconds; the transcoder's clock is 90 kHz, and the
// composition-time offset is what separates PTS from DTS for B-frames.
TEST(RtmpTagConverterTest, ScalesTimestampsAndAppliesCompositionTime) {
    RtmpTagConverter converter;
    ASSERT_TRUE(converter.convert_video(avc_sequence_header(), 0));

    auto result = converter.convert_video(avc_frame(false, 40), 1000);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value()->dts_90k, 90'000);
    EXPECT_EQ(result.value()->pts_90k, 90'000 + 40 * 90);
}

TEST(RtmpTagConverterTest, HandlesANegativeCompositionTimeOffset) {
    RtmpTagConverter converter;
    ASSERT_TRUE(converter.convert_video(avc_sequence_header(), 0));

    auto result = converter.convert_video(avc_frame(false, -40), 1000);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value()->pts_90k, 90'000 - 40 * 90);
}

TEST(RtmpTagConverterTest, RestoresTheAdtsHeaderOnRawAacFrames) {
    RtmpTagConverter converter;
    auto header = converter.convert_audio(aac_sequence_header(), 0);
    ASSERT_TRUE(header);
    EXPECT_FALSE(header.value().has_value());
    EXPECT_TRUE(converter.has_audio_config());

    auto result = converter.convert_audio(aac_frame(), 2000);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().has_value());
    const auto& unit = *result.value();
    ASSERT_GE(unit.adts.size(), 7u + 3u);
    EXPECT_EQ(unit.adts[0], std::byte{0xFF}); // ADTS syncword
    EXPECT_EQ(static_cast<std::uint8_t>(unit.adts[1]) & 0xF0, 0xF0);
    EXPECT_EQ(unit.pts_90k, 180'000);
    // The AAC payload itself is passed through untouched.
    EXPECT_EQ(unit.adts[7], std::byte{0x21});
}

TEST(RtmpTagConverterTest, RejectsAnAudioFrameBeforeItsAudioSpecificConfig) {
    RtmpTagConverter converter;
    auto result = converter.convert_audio(aac_frame(), 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidStateTransition);
}

TEST(RtmpTagConverterTest, RejectsAnEmptyVideoPayload) {
    RtmpTagConverter converter;
    auto result = converter.convert_video({}, 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::MalformedChunk);
}

// End-of-sequence carries no picture and must not be reported as one.
TEST(RtmpTagConverterTest, IgnoresEndOfSequenceTags) {
    RtmpTagConverter converter;
    ASSERT_TRUE(converter.convert_video(avc_sequence_header(), 0));

    std::vector<std::byte> tag;
    append(tag, {0x17, 0x02, 0x00, 0x00, 0x00});
    auto result = converter.convert_video(tag, 5000);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().has_value());
}

} // namespace
