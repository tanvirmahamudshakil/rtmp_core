#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "rtmp_server/relay/rtmp_tag_builder.hpp"
#include "rtmp_server/transcoding/native/rtmp_tag_converter.hpp"

namespace {

using rtmp_server::core::ErrorCode;
using rtmp_server::relay::RtmpAudioTagBuilder;
using rtmp_server::relay::RtmpVideoTagBuilder;
using rtmp_server::relay::split_annexb_nal_units;
using rtmp_server::transcoding::native::RtmpTagConverter;

void append(std::vector<std::byte>& out, std::initializer_list<int> bytes) {
    for (const int byte : bytes) out.push_back(static_cast<std::byte>(byte));
}

std::vector<std::byte> annexb_keyframe() {
    std::vector<std::byte> out;
    append(out, {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1F, 0xAC}); // SPS (profile 0x64, level 0x1F)
    append(out, {0, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80});       // PPS
    append(out, {0, 0, 1, 0x65, 0x88, 0x84, 0x00});          // IDR slice, 3-byte start code
    return out;
}

std::vector<std::byte> annexb_interframe() {
    std::vector<std::byte> out;
    append(out, {0, 0, 0, 1, 0x41, 0x9A, 0x02, 0x11});
    return out;
}

// A 7-byte ADTS header (no CRC): AAC-LC (profile 01), 44.1 kHz (index 4),
// stereo (channel config 2), frame length filled in below.
std::vector<std::byte> adts_frame(std::span<const std::byte> payload, bool sequence_first = false) {
    (void)sequence_first;
    std::vector<std::byte> out(7, std::byte{0});
    const std::size_t frame_length = 7 + payload.size();
    out[0] = std::byte{0xFF};
    out[1] = std::byte{0xF1}; // MPEG-4, no CRC
    out[2] = static_cast<std::byte>((1 << 6) | (4 << 2) | (0 << 1) | (2 >> 2)); // profile=1(LC), freq=4, chan hi bit
    out[3] = static_cast<std::byte>(((2 & 0x3) << 6) | ((frame_length >> 11) & 0x3));
    out[4] = static_cast<std::byte>((frame_length >> 3) & 0xFF);
    out[5] = static_cast<std::byte>(((frame_length & 0x7) << 5) | 0x1F);
    out[6] = std::byte{0xFC};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

TEST(SplitAnnexbNalUnitsTest, SplitsThreeAndFourByteStartCodes) {
    // Named, not a temporary: split_annexb_nal_units returns spans that alias
    // this buffer (see its header comment), so it must outlive them.
    const auto annexb = annexb_keyframe();
    const auto units = split_annexb_nal_units(annexb);
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(static_cast<std::uint8_t>(units[0][0]) & 0x1F, 7u); // SPS
    EXPECT_EQ(static_cast<std::uint8_t>(units[1][0]) & 0x1F, 8u); // PPS
    EXPECT_EQ(static_cast<std::uint8_t>(units[2][0]) & 0x1F, 5u); // IDR slice
}

TEST(SplitAnnexbNalUnitsTest, ReturnsEmptyForBytesWithNoStartCode) {
    std::vector<std::byte> junk = {std::byte{1}, std::byte{2}, std::byte{3}};
    EXPECT_TRUE(split_annexb_nal_units(junk).empty());
}

// Pins the documented lifetime contract: the returned spans alias the input
// buffer, so a caller must keep it alive for as long as it reads the result.
// This test passes a named buffer (correct usage) and reads it inside the
// buffer's own scope -- the wrong-usage shape (passing a temporary and
// reading the result afterward) is exactly what the header comment warns
// against and is deliberately not reproduced here: it is undefined behaviour,
// not a defined error the class can catch.
TEST(SplitAnnexbNalUnitsTest, ContentStaysValidForAsLongAsTheSourceBufferDoes) {
    const auto annexb = annexb_keyframe();
    const auto units = split_annexb_nal_units(annexb);
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(static_cast<std::uint8_t>(units[0][0]) & 0x1F, 7u);
    EXPECT_EQ(static_cast<std::uint8_t>(units[2][0]) & 0x1F, 5u);
}

TEST(RtmpVideoTagBuilderTest, RejectsAFrameBeforeASequenceHeaderIsBuilt) {
    RtmpVideoTagBuilder builder;
    auto result = builder.build_frame(annexb_interframe(), 90'000, 90'000, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidStateTransition);
}

TEST(RtmpVideoTagBuilderTest, RejectsAKeyframeWithNoParameterSets) {
    RtmpVideoTagBuilder builder;
    auto result = builder.build_sequence_header(annexb_interframe());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::MalformedChunk);
}

// The whole point of this builder: round-trip through the codebase's own
// decode path (RtmpTagConverter) and get back the identical NAL bytes and
// timestamps that went in.
TEST(RtmpVideoTagBuilderTest, RoundTripsThroughRtmpTagConverter) {
    RtmpVideoTagBuilder builder;
    auto header = builder.build_sequence_header(annexb_keyframe());
    ASSERT_TRUE(header) << header.error().message();
    EXPECT_TRUE(builder.has_sequence_header());

    RtmpTagConverter converter;
    ASSERT_TRUE(converter.convert_video(header.value(), 0));

    auto frame = builder.build_frame(annexb_keyframe(), 90'000, 90'000, true);
    ASSERT_TRUE(frame) << frame.error().message();

    auto decoded = converter.convert_video(frame.value(), 1000);
    ASSERT_TRUE(decoded) << decoded.error().message();
    ASSERT_TRUE(decoded.value().has_value());
    const auto& unit = *decoded.value();
    EXPECT_TRUE(unit.keyframe);
    EXPECT_EQ(unit.dts_90k, 90'000);
    EXPECT_EQ(unit.pts_90k, 90'000);

    // Annex B out (SPS+PPS+slice on a keyframe) must contain the same slice
    // payload the builder was given -- the round trip must not corrupt or
    // drop the picture data.
    const std::vector<std::byte> bytes(unit.annexb.begin(), unit.annexb.end());
    const std::array<std::byte, 2> marker = {std::byte{0x88}, std::byte{0x84}};
    const auto contains_slice_marker =
        std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) != bytes.end();
    EXPECT_TRUE(contains_slice_marker);
}

TEST(RtmpVideoTagBuilderTest, AppliesCompositionTimeFromPtsMinusDts) {
    RtmpVideoTagBuilder builder;
    ASSERT_TRUE(builder.build_sequence_header(annexb_keyframe()));

    RtmpTagConverter converter;
    auto header = builder.build_sequence_header(annexb_keyframe());
    ASSERT_TRUE(converter.convert_video(header.value(), 0));

    auto frame = builder.build_frame(annexb_interframe(), 90'000 + 40 * 90, 90'000, false);
    ASSERT_TRUE(frame);
    // 1000 ms == the frame's own dts_90k (90000/90) -- the RTMP message
    // timestamp a real caller sets is the DTS on the wire; only the
    // composition-time delta is carried inside the tag body itself.
    auto decoded = converter.convert_video(frame.value(), 1000);
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value().has_value());
    EXPECT_EQ(decoded.value()->dts_90k, 90'000);
    EXPECT_EQ(decoded.value()->pts_90k, 90'000 + 40 * 90);
    EXPECT_FALSE(decoded.value()->keyframe);
}

TEST(RtmpAudioTagBuilderTest, RejectsAFrameBeforeASequenceHeaderIsBuilt) {
    RtmpAudioTagBuilder builder;
    std::vector<std::byte> payload = {std::byte{0x21}, std::byte{0x00}, std::byte{0x03}};
    auto result = builder.build_frame(adts_frame(payload));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidStateTransition);
}

TEST(RtmpAudioTagBuilderTest, RoundTripsThroughRtmpTagConverter) {
    std::vector<std::byte> payload = {std::byte{0x21}, std::byte{0x00}, std::byte{0x03}, std::byte{0x77}};
    const auto adts = adts_frame(payload);

    RtmpAudioTagBuilder builder;
    auto header = builder.build_sequence_header(adts);
    ASSERT_TRUE(header) << header.error().message();
    EXPECT_TRUE(builder.has_sequence_header());

    RtmpTagConverter converter;
    ASSERT_TRUE(converter.convert_audio(header.value(), 0));
    EXPECT_TRUE(converter.has_audio_config());

    auto frame = builder.build_frame(adts);
    ASSERT_TRUE(frame) << frame.error().message();

    auto decoded = converter.convert_audio(frame.value(), 2000);
    ASSERT_TRUE(decoded) << decoded.error().message();
    ASSERT_TRUE(decoded.value().has_value());
    EXPECT_EQ(decoded.value()->pts_90k, 180'000);

    // The ADTS header AacDecoder cares about (sample rate/channel config) must
    // survive the AVC-style tag -> AudioSpecificConfig -> ADTS-back round
    // trip unchanged, and the raw payload bytes must pass through untouched.
    ASSERT_GE(decoded.value()->adts.size(), 7u + payload.size());
    EXPECT_EQ(decoded.value()->adts[0], std::byte{0xFF});
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(decoded.value()->adts[7 + i], payload[i]);
    }
}

TEST(RtmpAudioTagBuilderTest, RejectsATooShortAdtsFrame) {
    RtmpAudioTagBuilder builder;
    std::vector<std::byte> too_short = {std::byte{0xFF}, std::byte{0xF1}};
    auto result = builder.build_sequence_header(too_short);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::MalformedChunk);
}

} // namespace
