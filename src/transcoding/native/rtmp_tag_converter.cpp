#include "rtmp_server/transcoding/native/rtmp_tag_converter.hpp"

#include "rtmp_server/protocol/media/media_ingest.hpp"

namespace rtmp_server::transcoding::native {
namespace {

namespace protocol_media = rtmp_server::protocol::media;

core::Error malformed(std::string message) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                       std::move(message));
}

core::Error out_of_order(std::string message) {
    return core::Error(core::ErrorCode::InvalidStateTransition, core::ErrorCategory::Protocol,
                       std::move(message));
}

// Sign-extends the 24-bit composition-time offset both the classic AVC tag and
// the Enhanced RTMP CodedFrames tag carry.
std::int32_t composition_time(std::span<const std::byte> payload, std::size_t offset) {
    std::int32_t cts = (static_cast<std::int32_t>(payload[offset]) << 16) |
                       (static_cast<std::int32_t>(payload[offset + 1]) << 8) |
                       static_cast<std::int32_t>(payload[offset + 2]);
    if (cts & 0x00800000) cts |= static_cast<std::int32_t>(0xFF000000u);
    return cts;
}

} // namespace

bool RtmpTagConverter::has_video_config() const noexcept {
    return video_codec_ == SourceVideoCodec::Hevc ? (hevc_config_ && hevc_config_->valid())
                                                  : avc_config_.valid();
}

core::Result<std::optional<ConvertedVideoUnit>> RtmpTagConverter::convert_video(
    std::span<const std::byte> payload, std::uint32_t timestamp) {
    auto info = protocol_media::classify_video_tag(payload);
    if (!info) return malformed("empty RTMP video payload");

    // The decoder is chosen once and never switched mid-stream, so the codec
    // is latched from the first tag that identifies one.
    const auto codec = info->codec == protocol_media::VideoCodec::Hevc ? SourceVideoCodec::Hevc
                                                                      : SourceVideoCodec::H264;
    if (!video_codec_known_) {
        video_codec_ = codec;
        video_codec_known_ = true;
    }

    const bool keyframe = info->frame_type == protocol_media::VideoFrameType::KeyFrame ||
                          info->frame_type == protocol_media::VideoFrameType::GeneratedKeyFrame;
    if (info->codec == protocol_media::VideoCodec::Hevc) {
        return convert_hevc(payload, timestamp, info->enhanced, keyframe);
    }
    return convert_avc(payload, timestamp);
}

core::Result<std::optional<ConvertedVideoUnit>> RtmpTagConverter::convert_avc(
    std::span<const std::byte> payload, std::uint32_t timestamp) {
    auto tag = media::h264::parse_video_tag(payload);
    if (!tag) return tag.error();

    if (tag.value().avc_packet_type == media::h264::kAvcPacketTypeSequenceHeader) {
        auto config = media::h264::parse_decoder_config(tag.value().body);
        if (!config) return config.error();
        avc_config_ = std::move(config).value();
        return std::optional<ConvertedVideoUnit>{};
    }
    if (tag.value().avc_packet_type == media::h264::kAvcPacketTypeEndOfSequence) {
        return std::optional<ConvertedVideoUnit>{};
    }
    if (tag.value().avc_packet_type != media::h264::kAvcPacketTypeNalu) {
        return malformed("unsupported RTMP AVC packet type");
    }
    if (!avc_config_.valid()) {
        return out_of_order("RTMP video frame arrived before its AVC sequence header");
    }

    video_buffer_.clear();
    auto converted = media::h264::avcc_to_annexb(tag.value().body, avc_config_,
                                                 tag.value().is_keyframe, video_buffer_);
    if (!converted) return converted.error();

    ConvertedVideoUnit unit;
    unit.dts_90k = static_cast<std::int64_t>(video_clock_.unwrap(timestamp) * 90);
    unit.pts_90k = unit.dts_90k + static_cast<std::int64_t>(tag.value().composition_time_ms) * 90;
    unit.keyframe = tag.value().is_keyframe;
    unit.annexb = video_buffer_;
    return std::optional<ConvertedVideoUnit>{unit};
}

core::Result<std::optional<ConvertedVideoUnit>> RtmpTagConverter::convert_hevc(
    std::span<const std::byte> payload, std::uint32_t timestamp, bool enhanced, bool keyframe) {
    std::span<const std::byte> body;
    std::int32_t composition_time_ms = 0;
    bool is_sequence_start = false;
    bool is_coded_frame = false;

    if (enhanced) {
        auto info = protocol_media::classify_video_tag(payload);
        if (!info || !info->ex_packet_type) {
            return malformed("Enhanced RTMP video tag missing its PacketType");
        }
        switch (*info->ex_packet_type) {
            case protocol_media::ExVideoPacketType::SequenceStart:
                if (payload.size() < 5) return malformed("Enhanced RTMP HEVC sequence-start tag too short");
                is_sequence_start = true;
                body = payload.subspan(5);
                break;
            case protocol_media::ExVideoPacketType::CodedFrames:
                if (payload.size() < 8) return malformed("Enhanced RTMP HEVC coded-frame tag too short");
                composition_time_ms = composition_time(payload, 5);
                is_coded_frame = true;
                body = payload.subspan(8);
                break;
            case protocol_media::ExVideoPacketType::CodedFramesX:
                if (payload.size() < 5) return malformed("Enhanced RTMP HEVC coded-frame-x tag too short");
                is_coded_frame = true;
                body = payload.subspan(5);
                break;
            case protocol_media::ExVideoPacketType::SequenceEnd:
                return std::optional<ConvertedVideoUnit>{};
            default:
                // Metadata / MPEG2TSSequenceStart: nothing this pipeline needs.
                return std::optional<ConvertedVideoUnit>{};
        }
    } else {
        // Legacy CodecID 12: identical layout to a classic AVC tag.
        if (payload.size() < 5) return malformed("legacy HEVC video tag too short");
        composition_time_ms = composition_time(payload, 2);
        body = payload.subspan(5);
        const auto packet_type = static_cast<std::uint8_t>(payload[1]);
        if (packet_type == media::h264::kAvcPacketTypeSequenceHeader) {
            is_sequence_start = true;
        } else if (packet_type == media::h264::kAvcPacketTypeEndOfSequence) {
            return std::optional<ConvertedVideoUnit>{};
        } else if (packet_type == media::h264::kAvcPacketTypeNalu) {
            is_coded_frame = true;
        } else {
            return malformed("unsupported legacy HEVC packet type");
        }
    }

    if (is_sequence_start) {
        auto config = media::hevc::parse_decoder_config(body);
        if (!config) return config.error();
        hevc_config_ = std::move(config).value();
        return std::optional<ConvertedVideoUnit>{};
    }
    if (!is_coded_frame) return std::optional<ConvertedVideoUnit>{};
    if (!hevc_config_ || !hevc_config_->valid()) {
        return out_of_order("RTMP HEVC video frame arrived before its sequence header");
    }

    video_buffer_.clear();
    auto converted = media::hevc::hvcc_to_annexb(body, *hevc_config_, keyframe, video_buffer_);
    if (!converted) return converted.error();

    ConvertedVideoUnit unit;
    unit.dts_90k = static_cast<std::int64_t>(video_clock_.unwrap(timestamp) * 90);
    unit.pts_90k = unit.dts_90k + static_cast<std::int64_t>(composition_time_ms) * 90;
    unit.keyframe = keyframe;
    unit.annexb = video_buffer_;
    return std::optional<ConvertedVideoUnit>{unit};
}

core::Result<std::optional<ConvertedAudioUnit>> RtmpTagConverter::convert_audio(
    std::span<const std::byte> payload, std::uint32_t timestamp) {
    auto tag = media::aac::parse_audio_tag(payload);
    if (!tag) return tag.error();

    if (tag.value().aac_packet_type == media::aac::kAacPacketTypeSequenceHeader) {
        auto config = media::aac::parse_audio_specific_config(tag.value().body);
        if (!config) return config.error();
        audio_config_ = std::move(config).value();
        return std::optional<ConvertedAudioUnit>{};
    }
    if (tag.value().aac_packet_type != media::aac::kAacPacketTypeRaw) {
        return malformed("unsupported RTMP AAC packet type");
    }
    if (!audio_config_) {
        return out_of_order("RTMP audio frame arrived before its AAC sequence header");
    }

    audio_buffer_.clear();
    audio_buffer_.reserve(media::aac::kAdtsHeaderSize + tag.value().body.size());
    media::aac::append_adts_header(audio_buffer_, *audio_config_, tag.value().body.size());
    audio_buffer_.insert(audio_buffer_.end(), tag.value().body.begin(), tag.value().body.end());

    ConvertedAudioUnit unit;
    unit.pts_90k = static_cast<std::int64_t>(audio_clock_.unwrap(timestamp) * 90);
    unit.adts = audio_buffer_;
    return std::optional<ConvertedAudioUnit>{unit};
}

} // namespace rtmp_server::transcoding::native
