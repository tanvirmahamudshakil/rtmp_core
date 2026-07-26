#include "rtmp_server/protocol/media/media_ingest.hpp"

#include <array>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"

namespace rtmp_server::protocol::media {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;
using core::Result;

namespace {

Error malformed(std::string_view message) {
    return Error(ErrorCode::MalformedChunk, ErrorCategory::Protocol, message);
}

// AAC sampling frequency table indexed by the 4-bit samplingFrequencyIndex
// field of AudioSpecificConfig (ISO 14496-3 Table 1.16). Index 15
// ("escape", explicit frequency follows) and reserved indices 13/14 resolve
// to 0 (unknown) rather than being rejected outright — a stream using them
// is unusual but not malformed on its own.
constexpr std::array<std::uint32_t, 13> kAacSampleRates = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 11025, 8000, 7350,
};

std::uint32_t aac_sample_rate_for_index(std::uint8_t index) {
    if (index < kAacSampleRates.size()) return kAacSampleRates[index];
    return 0;
}

} // namespace

Result<AvcSequenceHeader> parse_avc_sequence_header(std::span<const std::byte> payload) {
    // AVCDecoderConfigurationRecord (ISO 14496-15 5.2.4.1):
    //   configurationVersion(1) AVCProfileIndication(1) profile_compatibility(1)
    //   AVCLevelIndication(1) reserved(6)+lengthSizeMinusOne(2)(1)
    //   reserved(3)+numOfSequenceParameterSets(5)(1)
    //   { sps_length(2) sps_bytes }*  numOfPictureParameterSets(1)
    //   { pps_length(2) pps_bytes }*
    if (payload.size() < 6) {
        return malformed("AVCDecoderConfigurationRecord shorter than the fixed 6-byte header");
    }

    AvcSequenceHeader header;
    header.profile = static_cast<std::uint8_t>(payload[1]);
    header.profile_compatibility = static_cast<std::uint8_t>(payload[2]);
    header.level = static_cast<std::uint8_t>(payload[3]);
    header.nalu_length_size = static_cast<std::uint8_t>((static_cast<std::uint8_t>(payload[4]) & 0x03) + 1);

    std::size_t offset = 5;
    auto read_u16 = [&](std::size_t at) -> std::uint16_t {
        return static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[at]) << 8) |
                                           static_cast<std::uint8_t>(payload[at + 1]));
    };

    std::uint8_t num_sps = static_cast<std::uint8_t>(payload[offset]) & 0x1F;
    offset += 1;
    for (std::uint8_t i = 0; i < num_sps; ++i) {
        if (offset + 2 > payload.size()) return malformed("truncated SPS length field");
        std::uint16_t len = read_u16(offset);
        offset += 2;
        if (offset + len > payload.size()) return malformed("truncated SPS payload");
        header.sps_list.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                      payload.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    }

    if (offset + 1 > payload.size()) return malformed("truncated numOfPictureParameterSets field");
    std::uint8_t num_pps = static_cast<std::uint8_t>(payload[offset]);
    offset += 1;
    for (std::uint8_t i = 0; i < num_pps; ++i) {
        if (offset + 2 > payload.size()) return malformed("truncated PPS length field");
        std::uint16_t len = read_u16(offset);
        offset += 2;
        if (offset + len > payload.size()) return malformed("truncated PPS payload");
        header.pps_list.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                      payload.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    }

    if (header.sps_list.empty() || header.pps_list.empty()) {
        return malformed("AVCDecoderConfigurationRecord carries no SPS/PPS");
    }

    return header;
}

Result<AacSequenceHeader> parse_aac_sequence_header(std::span<const std::byte> payload) {
    // AudioSpecificConfig (ISO 14496-3 1.6.2.1), simple (non-extended) form:
    //   audioObjectType(5) samplingFrequencyIndex(4) channelConfiguration(4) ...
    // i.e. at least 2 bytes: bits [AAAAABBBB][CCCC...].
    if (payload.size() < 2) {
        return malformed("AudioSpecificConfig shorter than the minimum 2 bytes");
    }

    auto b0 = static_cast<std::uint8_t>(payload[0]);
    auto b1 = static_cast<std::uint8_t>(payload[1]);

    AacSequenceHeader header;
    header.object_type = static_cast<std::uint8_t>(b0 >> 3);
    header.sampling_frequency_index = static_cast<std::uint8_t>(((b0 & 0x07) << 1) | (b1 >> 7));
    header.channel_configuration = static_cast<std::uint8_t>((b1 >> 3) & 0x0F);
    header.sampling_frequency = aac_sample_rate_for_index(header.sampling_frequency_index);
    header.raw.assign(payload.begin(), payload.end());

    if (header.object_type == 0) {
        return malformed("AudioSpecificConfig audioObjectType is 0 (reserved)");
    }

    return header;
}

std::optional<VideoTagInfo> classify_video_tag(std::span<const std::byte> payload) {
    if (payload.empty()) return std::nullopt;
    auto tag = static_cast<std::uint8_t>(payload[0]);
    VideoTagInfo info;
    info.frame_type = static_cast<VideoFrameType>(tag >> 4);
    info.codec = static_cast<VideoCodec>(tag & 0x0F);
    if (info.codec == VideoCodec::Avc && payload.size() > 1) {
        info.avc_packet_type = static_cast<AvcPacketType>(payload[1]);
    }
    return info;
}

std::optional<AudioTagInfo> classify_audio_tag(std::span<const std::byte> payload) {
    if (payload.empty()) return std::nullopt;
    auto tag = static_cast<std::uint8_t>(payload[0]);
    AudioTagInfo info;
    info.codec = static_cast<AudioCodec>(tag >> 4);
    if (info.codec == AudioCodec::Aac && payload.size() > 1) {
        info.aac_packet_type = static_cast<AacPacketType>(payload[1]);
    }
    return info;
}

StreamMediaState& MediaIngest::state_for(std::string_view stream_key) {
    auto [it, inserted] = streams_.try_emplace(std::string(stream_key));
    return it->second;
}

const StreamMediaState* MediaIngest::find(std::string_view stream_key) const {
    auto it = streams_.find(std::string(stream_key));
    return it == streams_.end() ? nullptr : &it->second;
}

void MediaIngest::remove_stream(std::string_view stream_key) { streams_.erase(std::string(stream_key)); }

Result<void> MediaIngest::on_audio_message(std::string_view stream_key, const chunk::RtmpMessage& message) {
    auto& state = state_for(stream_key);

    if (message.payload.empty()) {
        state.stats.rejected_message_count++;
        return malformed("audio message has an empty payload");
    }

    auto info = classify_audio_tag(message.payload);
    auto codec = info->codec;
    state.audio_codec = codec;

    state.stats.audio_message_count++;
    state.stats.audio_bytes += message.payload.size();
    state.stats.last_audio_timestamp = message.timestamp;

    if (codec != AudioCodec::Aac) {
        // Non-AAC codecs have no sequence header this phase understands;
        // still counted as a valid, ingested audio message.
        return {};
    }

    if (message.payload.size() < 2) {
        state.stats.rejected_message_count++;
        return malformed("AAC audio tag missing the AACPacketType byte");
    }

    auto packet_type = *info->aac_packet_type;
    if (packet_type != AacPacketType::SequenceHeader) {
        return {}; // raw AAC frame, nothing to retain
    }

    std::span<const std::byte> config_data(message.payload.data() + 2, message.payload.size() - 2);
    auto parsed = parse_aac_sequence_header(config_data);
    if (!parsed) {
        state.stats.rejected_message_count++;
        return parsed.error();
    }

    state.aac_sequence_header = std::move(parsed).value();
    return {};
}

Result<void> MediaIngest::on_video_message(std::string_view stream_key, const chunk::RtmpMessage& message) {
    auto& state = state_for(stream_key);

    if (message.payload.empty()) {
        state.stats.rejected_message_count++;
        return malformed("video message has an empty payload");
    }

    auto info = classify_video_tag(message.payload);
    auto frame_type = info->frame_type;
    auto codec = info->codec;
    state.video_codec = codec;

    state.stats.video_message_count++;
    state.stats.video_bytes += message.payload.size();
    state.stats.last_video_timestamp = message.timestamp;

    bool is_keyframe = frame_type == VideoFrameType::KeyFrame || frame_type == VideoFrameType::GeneratedKeyFrame;
    if (is_keyframe) {
        state.seen_keyframe = true;
        state.stats.keyframe_count++;
        state.stats.last_keyframe_timestamp = message.timestamp;
    }

    if (codec != VideoCodec::Avc) {
        return {}; // no sequence header this phase understands for other codecs
    }

    if (message.payload.size() < 5) {
        state.stats.rejected_message_count++;
        return malformed("AVC video tag missing AVCPacketType/composition-time header");
    }

    auto packet_type = *info->avc_packet_type;
    // Bytes [2,5) are the 24-bit composition time offset — not used by this
    // phase (Playback/timestamp reconciliation is a later concern) but its
    // presence is what the >=5 length check above validates.
    if (packet_type != AvcPacketType::SequenceHeader) {
        return {}; // NALU or end-of-sequence, nothing to retain
    }

    std::span<const std::byte> config_data(message.payload.data() + 5, message.payload.size() - 5);
    auto parsed = parse_avc_sequence_header(config_data);
    if (!parsed) {
        state.stats.rejected_message_count++;
        return parsed.error();
    }

    state.avc_sequence_header = std::move(parsed).value();
    return {};
}

Result<void> MediaIngest::on_metadata_message(std::string_view stream_key, const chunk::RtmpMessage& message) {
    auto& state = state_for(stream_key);

    auto decoded = amf0::decode_all(message.payload);
    if (!decoded || decoded.value().empty() || !decoded.value().front().is_string()) {
        state.stats.rejected_message_count++;
        return malformed("metadata message is not a valid AMF0 (name, ...) sequence");
    }

    const auto& values = decoded.value();
    const std::string& name = values.front().as_string();
    // OBS/FMLE send `@setDataFrame` "onMetaData" {...} (3 values: the
    // wrapper marker, the actual event name, then the info object); plain
    // encoders may send just `onMetaData` {...} (2 values). Either shape
    // counts as metadata being received/parsed for the acceptance
    // criterion; the info object itself isn't required to have any
    // particular field, so nothing further to validate.
    bool is_set_data_frame = name == "@setDataFrame";
    bool is_on_metadata = name == "onMetaData";
    if (!is_set_data_frame && !is_on_metadata) {
        // Not a recognized metadata event, but still a validly-decoded AMF0
        // Data message — do not reject, just don't count it as metadata.
        return {};
    }
    if (is_set_data_frame && values.size() < 2) {
        state.stats.rejected_message_count++;
        return malformed("@setDataFrame missing the wrapped event name");
    }

    state.stats.metadata_message_count++;
    return {};
}

} // namespace rtmp_server::protocol::media
