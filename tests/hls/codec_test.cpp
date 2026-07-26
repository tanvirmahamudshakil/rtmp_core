#include <gtest/gtest.h>

#include <vector>

#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "test_media.hpp"

using namespace rtmp_server;
using namespace rtmp_server::hls_test;

namespace {

std::span<const std::byte> span_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

// Counts Annex B start codes (00 00 00 01) in a buffer.
std::size_t count_start_codes(const std::vector<std::byte>& data) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == std::byte{0} && data[i + 1] == std::byte{0} && data[i + 2] == std::byte{0} &&
            data[i + 3] == std::byte{1}) {
            ++count;
        }
    }
    return count;
}

} // namespace

// --- H.264 ----------------------------------------------------------------

TEST(AvcTest, ParsesFlvVideoTagHeader) {
    auto payload = avc_frame(/*keyframe=*/true, 32);
    auto parsed = media::h264::parse_video_tag(span_of(payload));
    ASSERT_TRUE(parsed.ok()) << parsed.error().message();
    EXPECT_TRUE(parsed.value().is_keyframe);
    EXPECT_EQ(parsed.value().avc_packet_type, media::h264::kAvcPacketTypeNalu);
    EXPECT_EQ(parsed.value().composition_time_ms, 0);
}

TEST(AvcTest, SignExtendsNegativeCompositionTime) {
    // CTS is a signed 24-bit field; -40 ms must not read as +16777176.
    auto payload = avc_frame(false, 16, -40);
    auto parsed = media::h264::parse_video_tag(span_of(payload));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().composition_time_ms, -40);
}

TEST(AvcTest, RejectsNonAvcCodecAndTruncatedPayloads) {
    std::vector<std::byte> non_avc{std::byte{0x12}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    EXPECT_FALSE(media::h264::parse_video_tag(span_of(non_avc)).ok());

    std::vector<std::byte> truncated{std::byte{0x17}, std::byte{0x01}};
    EXPECT_FALSE(media::h264::parse_video_tag(span_of(truncated)).ok());
}

TEST(AvcTest, ParsesDecoderConfigurationRecord) {
    auto header = avc_sequence_header();
    auto tag = media::h264::parse_video_tag(span_of(header));
    ASSERT_TRUE(tag.ok());

    auto config = media::h264::parse_decoder_config(tag.value().body);
    ASSERT_TRUE(config.ok()) << config.error().message();
    EXPECT_EQ(config.value().nalu_length_size, 4);
    ASSERT_EQ(config.value().sps.size(), 1u);
    ASSERT_EQ(config.value().pps.size(), 1u);
    EXPECT_EQ(config.value().sps[0], sps_nal());
    EXPECT_EQ(config.value().pps[0], pps_nal());
    EXPECT_TRUE(config.value().valid());
}

TEST(AvcTest, RejectsDecoderConfigWithLengthRunningPastTheBuffer) {
    // numOfSPS = 1 with a declared SPS length of 0xFFFF but no data.
    std::vector<std::byte> record;
    append(record, {0x01, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0xFF, 0xFF});
    auto config = media::h264::parse_decoder_config(span_of(record));
    ASSERT_FALSE(config.ok());
    EXPECT_EQ(config.error().code(), core::ErrorCode::MalformedChunk);
}

TEST(AvcTest, ConvertsAvccToAnnexBAndInsertsParameterSetsOnKeyframes) {
    auto header = avc_sequence_header();
    auto config = media::h264::parse_decoder_config(
        media::h264::parse_video_tag(span_of(header)).value().body);
    ASSERT_TRUE(config.ok());

    auto frame = avc_frame(/*keyframe=*/true, 32);
    auto tag = media::h264::parse_video_tag(span_of(frame));
    ASSERT_TRUE(tag.ok());

    std::vector<std::byte> out;
    ASSERT_TRUE(
        media::h264::avcc_to_annexb(tag.value().body, config.value(), /*insert=*/true, out).ok());

    // AUD + SPS + PPS + the slice = 4 start codes.
    EXPECT_EQ(count_start_codes(out), 4u);
    // The AUD is first.
    EXPECT_EQ(out[4], std::byte{0x09});
    // The 4-byte AVCC length prefix must be gone, replaced by a start code.
    EXPECT_EQ(out.size(), 6u + (4u + sps_nal().size()) + (4u + pps_nal().size()) + 4u + 33u);
}

TEST(AvcTest, OmitsParameterSetsOnNonKeyframes) {
    auto header = avc_sequence_header();
    auto config = media::h264::parse_decoder_config(
        media::h264::parse_video_tag(span_of(header)).value().body);
    auto frame = avc_frame(/*keyframe=*/false, 32);
    auto tag = media::h264::parse_video_tag(span_of(frame));

    std::vector<std::byte> out;
    ASSERT_TRUE(
        media::h264::avcc_to_annexb(tag.value().body, config.value(), /*insert=*/false, out).ok());
    // AUD + slice only.
    EXPECT_EQ(count_start_codes(out), 2u);
}

TEST(AvcTest, RejectsSampleWhoseNalLengthOverrunsTheBuffer) {
    auto header = avc_sequence_header();
    auto config = media::h264::parse_decoder_config(
        media::h264::parse_video_tag(span_of(header)).value().body);

    // Declares a 0x00FFFFFF-byte NAL inside a 5-byte sample.
    std::vector<std::byte> sample;
    append(sample, {0x00, 0xFF, 0xFF, 0xFF, 0x65});

    std::vector<std::byte> out;
    auto result = media::h264::avcc_to_annexb(span_of(sample), config.value(), false, out);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), core::ErrorCode::MalformedChunk);
}

// --- AAC ------------------------------------------------------------------

TEST(AacTest, ParsesAudioSpecificConfig) {
    auto header = aac_sequence_header(/*sample_rate_index=*/4, /*channels=*/2);
    auto tag = media::aac::parse_audio_tag(span_of(header));
    ASSERT_TRUE(tag.ok());
    EXPECT_EQ(tag.value().aac_packet_type, media::aac::kAacPacketTypeSequenceHeader);

    auto config = media::aac::parse_audio_specific_config(tag.value().body);
    ASSERT_TRUE(config.ok()) << config.error().message();
    EXPECT_EQ(config.value().object_type, 2);
    EXPECT_EQ(config.value().sampling_frequency_index, 4);
    EXPECT_EQ(config.value().channel_configuration, 2);
    EXPECT_EQ(config.value().sample_rate(), 44100u);
}

TEST(AacTest, ParsesAlternateSampleRatesAndChannelCounts) {
    auto header = aac_sequence_header(/*sample_rate_index=*/3, /*channels=*/1); // 48 kHz mono
    auto tag = media::aac::parse_audio_tag(span_of(header));
    auto config = media::aac::parse_audio_specific_config(tag.value().body);
    ASSERT_TRUE(config.ok());
    EXPECT_EQ(config.value().sample_rate(), 48000u);
    EXPECT_EQ(config.value().channel_configuration, 1);
}

TEST(AacTest, RejectsNonAacSoundFormat) {
    std::vector<std::byte> mp3{std::byte{0x2F}, std::byte{0x01}}; // SoundFormat 2 = MP3
    EXPECT_FALSE(media::aac::parse_audio_tag(span_of(mp3)).ok());
}

TEST(AacTest, RejectsReservedSamplingFrequencyIndex) {
    // Index 15 signals an explicit rate we deliberately do not support.
    std::vector<std::byte> config;
    append(config, {(2u << 3) | 0x07, 0x80});
    EXPECT_FALSE(media::aac::parse_audio_specific_config(span_of(config)).ok());
}

TEST(AacTest, BuildsAConformantAdtsHeader) {
    media::aac::AudioSpecificConfig config;
    config.object_type = 2;
    config.sampling_frequency_index = 4; // 44100
    config.channel_configuration = 2;

    std::vector<std::byte> out;
    const std::size_t payload = 379;
    media::aac::append_adts_header(out, config, payload);

    ASSERT_EQ(out.size(), media::aac::kAdtsHeaderSize);
    // Syncword 0xFFF.
    EXPECT_EQ(out[0], std::byte{0xFF});
    EXPECT_EQ(static_cast<unsigned>(out[1]) & 0xF0u, 0xF0u);
    // MPEG-4, layer 00, protection_absent 1.
    EXPECT_EQ(out[1], std::byte{0xF1});
    // profile = objectType-1 = 1 (AAC-LC).
    EXPECT_EQ((static_cast<unsigned>(out[2]) >> 6) & 0x03u, 1u);
    // sampling_frequency_index.
    EXPECT_EQ((static_cast<unsigned>(out[2]) >> 2) & 0x0Fu, 4u);
    // channel_configuration spans bit 0 of byte 2 and bits 7-6 of byte 3.
    const unsigned channels = ((static_cast<unsigned>(out[2]) & 0x01u) << 2) |
                              ((static_cast<unsigned>(out[3]) >> 6) & 0x03u);
    EXPECT_EQ(channels, 2u);

    // aac_frame_length is 13 bits spanning bytes 3-5 and includes the header.
    const unsigned length = ((static_cast<unsigned>(out[3]) & 0x03u) << 11) |
                            (static_cast<unsigned>(out[4]) << 3) |
                            ((static_cast<unsigned>(out[5]) >> 5) & 0x07u);
    EXPECT_EQ(length, payload + media::aac::kAdtsHeaderSize);

    // numberOfRawDataBlocksInFrame - 1 == 0.
    EXPECT_EQ(static_cast<unsigned>(out[6]) & 0x03u, 0u);
}
