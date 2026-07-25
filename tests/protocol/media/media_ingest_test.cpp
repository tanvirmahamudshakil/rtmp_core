#include "rtmp_server/protocol/media/media_ingest.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_value.hpp"

namespace rtmp_server::protocol::media {
namespace {

using amf0::Amf0Value;
using chunk::MessageTypeId;
using chunk::RtmpMessage;

std::vector<std::byte> bytes(std::initializer_list<int> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (int v : vals) out.push_back(static_cast<std::byte>(v));
    return out;
}

RtmpMessage make_message(MessageTypeId type, std::vector<std::byte> payload, std::uint32_t timestamp = 0,
                          std::uint32_t message_stream_id = 1) {
    RtmpMessage msg;
    msg.chunk_stream_id = type == MessageTypeId::Audio ? 4 : 6;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(type);
    msg.timestamp = timestamp;
    msg.payload = std::move(payload);
    return msg;
}

// A real, minimal-but-valid AVCDecoderConfigurationRecord: version=1,
// profile=0x64 (High), profile_compat=0x00, level=0x1F, lengthSizeMinusOne
// nibble -> 4-byte NALU lengths, 1 SPS (7 bytes), 1 PPS (4 bytes). SPS/PPS
// payloads are representative NAL bytes (start-code-less, as AVC config
// records carry them), not required to be semantically valid H.264 for this
// component (it only parses the container framing, not SPS bit fields).
std::vector<std::byte> avc_sequence_header_payload() {
    return bytes({
        0x17, 0x00, 0x00, 0x00, 0x00,             // video tag: keyframe/AVC, AVCPacketType=0, composition time=0
        0x01,                                     // configurationVersion
        0x64, 0x00, 0x1F,                          // profile, profile_compat, level
        0xFF,                                      // reserved(6)+lengthSizeMinusOne(2) = 4-byte lengths
        0xE1,                                      // reserved(3)+numSPS(5) = 1
        0x00, 0x07,                                // SPS length = 7
        0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9, 0x40,  // SPS bytes
        0x01,                                      // numPPS = 1
        0x00, 0x04,                                // PPS length = 4
        0x68, 0xEA, 0xC3, 0xCD,                    // PPS bytes
    });
}

// A real AVC NALU video tag carrying an IDR (keyframe): frame type=1
// (key)/codec=7 (AVC), AVCPacketType=1 (NALU), composition time=0, then one
// 4-byte-length-prefixed NALU whose first byte's low 5 bits (0x05) mark it
// as an IDR slice per H.264 NAL unit type semantics.
std::vector<std::byte> avc_keyframe_nalu_payload() {
    return bytes({
        0x17, 0x01, 0x00, 0x00, 0x00,             // keyframe/AVC, AVCPacketType=1, composition time=0
        0x00, 0x00, 0x00, 0x05,                    // NALU length = 5
        0x65, 0x88, 0x84, 0x00, 0x10,              // NALU: nal_unit_type=5 (IDR slice) + payload
    });
}

std::vector<std::byte> avc_interframe_nalu_payload() {
    return bytes({
        0x27, 0x01, 0x00, 0x00, 0x00,             // inter-frame/AVC, AVCPacketType=1, composition time=0
        0x00, 0x00, 0x00, 0x02,                    // NALU length = 2
        0x41, 0x9A,                                // NALU: nal_unit_type=1 (non-IDR slice)
    });
}

// A real AAC-LC, 44100Hz, stereo AudioSpecificConfig: 0x12 0x10 is the
// well-known two-byte ASC for these parameters (audioObjectType=2,
// samplingFrequencyIndex=4, channelConfiguration=2), as commonly produced by
// ffmpeg/OBS AAC encoders.
std::vector<std::byte> aac_sequence_header_payload() {
    return bytes({
        0xAF, 0x00, // sound format=AAC(10)/44kHz/16-bit/stereo, AACPacketType=0 (sequence header)
        0x12, 0x10, // AudioSpecificConfig
    });
}

std::vector<std::byte> aac_raw_frame_payload() {
    return bytes({0xAF, 0x01, 0xDE, 0xAD, 0xBE, 0xEF});
}

RtmpMessage make_metadata_message(std::vector<Amf0Value> values, std::uint32_t message_stream_id = 1) {
    RtmpMessage msg;
    msg.chunk_stream_id = 4;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    for (const auto& v : values) amf0::encode(v, msg.payload);
    return msg;
}

TEST(MediaIngestTest, AacSequenceHeaderIsRetainedAndDecoded) {
    MediaIngest ingest;
    auto result = ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, aac_sequence_header_payload(), 0));
    ASSERT_TRUE(result.ok());

    const auto* state = ingest.find("stream1");
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->aac_sequence_header.has_value());
    EXPECT_EQ(state->aac_sequence_header->object_type, 2);
    EXPECT_EQ(state->aac_sequence_header->sampling_frequency_index, 4);
    EXPECT_EQ(state->aac_sequence_header->sampling_frequency, 44100u);
    EXPECT_EQ(state->aac_sequence_header->channel_configuration, 2);
    EXPECT_EQ(state->audio_codec, AudioCodec::Aac);
    EXPECT_EQ(state->stats.audio_message_count, 1u);
}

TEST(MediaIngestTest, RawAacFrameDoesNotOverwriteRetainedSequenceHeader) {
    MediaIngest ingest;
    ASSERT_TRUE(ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, aac_sequence_header_payload())).ok());
    ASSERT_TRUE(ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, aac_raw_frame_payload(), 33)).ok());

    const auto* state = ingest.find("stream1");
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->aac_sequence_header.has_value());
    EXPECT_EQ(state->stats.audio_message_count, 2u);
    EXPECT_EQ(state->stats.last_audio_timestamp, 33u);
}

TEST(MediaIngestTest, AvcSequenceHeaderRetainsSpsAndPps) {
    MediaIngest ingest;
    auto result = ingest.on_video_message("stream1", make_message(MessageTypeId::Video, avc_sequence_header_payload(), 0));
    ASSERT_TRUE(result.ok());

    const auto* state = ingest.find("stream1");
    ASSERT_NE(state, nullptr);
    ASSERT_TRUE(state->avc_sequence_header.has_value());
    ASSERT_EQ(state->avc_sequence_header->sps_list.size(), 1u);
    ASSERT_EQ(state->avc_sequence_header->pps_list.size(), 1u);
    EXPECT_EQ(state->avc_sequence_header->sps_list[0].size(), 7u);
    EXPECT_EQ(state->avc_sequence_header->pps_list[0].size(), 4u);
    EXPECT_EQ(state->avc_sequence_header->profile, 0x64);
    EXPECT_EQ(state->avc_sequence_header->level, 0x1F);
    EXPECT_EQ(state->avc_sequence_header->nalu_length_size, 4u);
    EXPECT_EQ(state->video_codec, VideoCodec::Avc);
    // The sequence-header video tag itself has frame type nibble 1 (key), so
    // it should also register as a keyframe.
    EXPECT_TRUE(state->seen_keyframe);
}

TEST(MediaIngestTest, KeyframeIsDetectedFromNaluFrameTypeNibble) {
    MediaIngest ingest;
    ASSERT_TRUE(ingest.on_video_message("stream1", make_message(MessageTypeId::Video, avc_interframe_nalu_payload(), 10)).ok());
    EXPECT_FALSE(ingest.find("stream1")->seen_keyframe);

    ASSERT_TRUE(ingest.on_video_message("stream1", make_message(MessageTypeId::Video, avc_keyframe_nalu_payload(), 43)).ok());
    const auto* state = ingest.find("stream1");
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->seen_keyframe);
    EXPECT_EQ(state->stats.keyframe_count, 1u);
    EXPECT_EQ(state->stats.last_keyframe_timestamp, 43u);
    EXPECT_EQ(state->stats.video_message_count, 2u);
}

TEST(MediaIngestTest, MetadataOnMetaDataIsCounted) {
    MediaIngest ingest;
    std::vector<Amf0Value> values = {
        Amf0Value::string("@setDataFrame"),
        Amf0Value::string("onMetaData"),
        Amf0Value::ecma_array({
            {"width", Amf0Value::number(1280)},
            {"height", Amf0Value::number(720)},
            {"videocodecid", Amf0Value::number(7)},
            {"audiocodecid", Amf0Value::number(10)},
        }),
    };
    auto result = ingest.on_metadata_message("stream1", make_metadata_message(values));
    ASSERT_TRUE(result.ok());

    const auto* state = ingest.find("stream1");
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->stats.metadata_message_count, 1u);
}

TEST(MediaIngestTest, PlainOnMetaDataWithoutSetDataFrameWrapperIsCounted) {
    MediaIngest ingest;
    std::vector<Amf0Value> values = {
        Amf0Value::string("onMetaData"),
        Amf0Value::ecma_array({{"duration", Amf0Value::number(0)}}),
    };
    auto result = ingest.on_metadata_message("stream1", make_metadata_message(values));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(ingest.find("stream1")->stats.metadata_message_count, 1u);
}

TEST(MediaIngestTest, RejectsEmptyAudioPayload) {
    MediaIngest ingest;
    auto result = ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, {}));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(ingest.find("stream1")->stats.rejected_message_count, 1u);
}

TEST(MediaIngestTest, RejectsEmptyVideoPayload) {
    MediaIngest ingest;
    auto result = ingest.on_video_message("stream1", make_message(MessageTypeId::Video, {}));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(ingest.find("stream1")->stats.rejected_message_count, 1u);
}

TEST(MediaIngestTest, RejectsTruncatedAvcSequenceHeader) {
    MediaIngest ingest;
    // Valid video-tag framing (keyframe/AVC/seq-header) but the
    // AVCDecoderConfigurationRecord body is cut short (only 3 bytes instead
    // of the required 6+).
    auto payload = bytes({0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00});
    auto result = ingest.on_video_message("stream1", make_message(MessageTypeId::Video, payload));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(ingest.find("stream1")->stats.rejected_message_count, 1u);
    EXPECT_FALSE(ingest.find("stream1")->avc_sequence_header.has_value());
}

TEST(MediaIngestTest, RejectsAvcSequenceHeaderWithTruncatedSpsLength) {
    MediaIngest ingest;
    // numSPS says 1, but the payload ends right after the length field claims
    // more SPS bytes than are actually present.
    auto payload = bytes({
        0x17, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x64, 0x00, 0x1F, 0xFF,
        0xE1,       // numSPS = 1
        0x00, 0x0A, // SPS length = 10, but only 2 bytes follow
        0x67, 0x64,
    });
    auto result = ingest.on_video_message("stream1", make_message(MessageTypeId::Video, payload));
    EXPECT_FALSE(result.ok());
}

TEST(MediaIngestTest, RejectsTruncatedAacSequenceHeader) {
    MediaIngest ingest;
    auto payload = bytes({0xAF, 0x00, 0x12}); // ASC needs at least 2 bytes, only 1 present
    auto result = ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, payload));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(ingest.find("stream1")->stats.rejected_message_count, 1u);
}

TEST(MediaIngestTest, RejectsAudioTagMissingAacPacketTypeByte) {
    MediaIngest ingest;
    auto payload = bytes({0xAF}); // sound-format byte only, no AACPacketType
    auto result = ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, payload));
    EXPECT_FALSE(result.ok());
}

TEST(MediaIngestTest, RejectsMalformedMetadataPayload) {
    MediaIngest ingest;
    RtmpMessage msg;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Data);
    msg.payload = bytes({0xFF, 0xFF, 0xFF}); // not valid AMF0 at all
    auto result = ingest.on_metadata_message("stream1", msg);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(ingest.find("stream1")->stats.rejected_message_count, 1u);
}

TEST(MediaIngestTest, RemoveStreamDropsRetainedState) {
    MediaIngest ingest;
    ASSERT_TRUE(ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, aac_sequence_header_payload())).ok());
    ASSERT_NE(ingest.find("stream1"), nullptr);
    ingest.remove_stream("stream1");
    EXPECT_EQ(ingest.find("stream1"), nullptr);
    EXPECT_EQ(ingest.stream_count(), 0u);
}

TEST(MediaIngestTest, StatsTrackBytesAndCountsPerStream) {
    MediaIngest ingest;
    ASSERT_TRUE(ingest.on_video_message("stream1", make_message(MessageTypeId::Video, avc_keyframe_nalu_payload(), 0)).ok());
    ASSERT_TRUE(ingest.on_video_message("stream1", make_message(MessageTypeId::Video, avc_interframe_nalu_payload(), 33)).ok());
    ASSERT_TRUE(ingest.on_audio_message("stream1", make_message(MessageTypeId::Audio, aac_raw_frame_payload(), 20)).ok());

    const auto* state = ingest.find("stream1");
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->stats.video_message_count, 2u);
    EXPECT_EQ(state->stats.audio_message_count, 1u);
    EXPECT_EQ(state->stats.video_bytes, avc_keyframe_nalu_payload().size() + avc_interframe_nalu_payload().size());
    EXPECT_EQ(state->stats.audio_bytes, aac_raw_frame_payload().size());
}

} // namespace
} // namespace rtmp_server::protocol::media
