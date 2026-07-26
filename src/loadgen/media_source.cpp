#include "rtmp_server/loadgen/media_source.hpp"

#include <algorithm>

namespace rtmp_server::loadgen {
namespace {

constexpr std::byte b(unsigned value) { return static_cast<std::byte>(value & 0xFFu); }

void append(std::vector<std::byte>& out, std::initializer_list<unsigned> values) {
    for (unsigned v : values) out.push_back(b(v));
}

void append_be32(std::vector<std::byte>& out, std::uint32_t value) {
    append(out, {(value >> 24) & 0xFFu, (value >> 16) & 0xFFu, (value >> 8) & 0xFFu, value & 0xFFu});
}

// Samples per AAC frame is fixed by the format.
constexpr std::uint32_t kAacSamplesPerFrame = 1024;

// Byte offsets of the fixed header the verifier re-checks.
constexpr std::size_t kVideoTagHeaderSize = 5; // frametype/codec, avcpackettype, 3-byte CTS
constexpr std::size_t kAudioTagHeaderSize = 2; // soundformat byte, aacpackettype

// Deterministic pattern byte for position `i` of a frame tagged `tag`. A
// cheap xorshift-style mix: repeatable, and unlike a constant fill it detects
// a frame that is truncated, duplicated, or spliced with another frame's
// bytes at any offset.
constexpr std::byte pattern_byte(std::size_t i, std::uint8_t tag) {
    auto x = static_cast<std::uint32_t>(i * 2654435761u + tag * 40503u);
    x ^= x >> 13;
    x *= 1274126177u;
    x ^= x >> 16;
    return static_cast<std::byte>(x & 0xFFu);
}

std::vector<std::byte> sps_nal() {
    std::vector<std::byte> nal;
    // 0x67 = nal_ref_idc 3, type 7 (SPS); Baseline profile 66, level 3.0.
    append(nal, {0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xF0, 0x11, 0x7E, 0xF0, 0x10, 0x10});
    return nal;
}

std::vector<std::byte> pps_nal() {
    std::vector<std::byte> nal;
    append(nal, {0x68, 0xCE, 0x3C, 0x80}); // type 8 = PPS
    return nal;
}

} // namespace

MediaSource::MediaSource(MediaProfile profile, std::uint64_t seed) : profile_(profile), seed_(seed) {
    // Guard against a configuration that would divide by zero or generate
    // unbounded payloads. These are operator-supplied, not remote, but the
    // tool should still refuse to misbehave rather than crash.
    if (profile_.frames_per_second == 0) profile_.frames_per_second = 1;
    if (profile_.audio_sample_rate == 0) profile_.audio_sample_rate = 44'100;
    if (profile_.max_frame_bytes == 0) profile_.max_frame_bytes = 64 * 1024;
}

void MediaSource::fill_pattern(std::vector<std::byte>& out, std::size_t length, std::uint8_t tag) {
    out.reserve(out.size() + length);
    for (std::size_t i = 0; i < length; ++i) out.push_back(pattern_byte(i, tag));
}

std::size_t MediaSource::next_video_payload_size(bool keyframe) const {
    // Average bytes per video frame from the configured bitrate.
    const auto average =
        static_cast<double>(profile_.video_bitrate_bps) / 8.0 / static_cast<double>(profile_.frames_per_second);

    // A real GOP spends far more bits on the keyframe than on inter frames.
    // Model that split so the byte distribution on the wire (and therefore
    // per-viewer queue pressure and GOP cache occupancy) is realistic:
    // keyframe = 8x an inter frame, with the GOP's total bits preserved.
    const double gop = profile_.keyframe_interval_frames == 0
                           ? 1.0
                           : static_cast<double>(profile_.keyframe_interval_frames);
    const double inter_share = gop / (gop + 7.0); // solves k=8i, total preserved
    const double inter_bytes = average * inter_share;
    const double size = keyframe ? inter_bytes * 8.0 : inter_bytes;

    const auto clamped = static_cast<std::size_t>(std::max(1.0, size));
    return std::min(clamped, profile_.max_frame_bytes);
}

GeneratedFrame MediaSource::video_sequence_header() const {
    const auto sps = sps_nal();
    const auto pps = pps_nal();

    GeneratedFrame frame;
    frame.kind = FrameKind::VideoSequenceHeader;
    frame.timestamp_ms = 0;

    auto& out = frame.payload;
    append(out, {0x17, 0x00, 0x00, 0x00, 0x00}); // keyframe|AVC, seq header, CTS 0

    // AVCDecoderConfigurationRecord (ISO 14496-15).
    append(out, {0x01});    // configurationVersion
    out.push_back(sps[1]);  // AVCProfileIndication
    out.push_back(sps[2]);  // profile_compatibility
    out.push_back(sps[3]);  // AVCLevelIndication
    append(out, {0xFF});    // reserved + lengthSizeMinusOne = 3 (4-byte NAL lengths)
    append(out, {0xE1});    // reserved + numOfSequenceParameterSets = 1
    append(out, {static_cast<unsigned>((sps.size() >> 8) & 0xFFu), static_cast<unsigned>(sps.size() & 0xFFu)});
    out.insert(out.end(), sps.begin(), sps.end());
    append(out, {0x01});    // numOfPictureParameterSets = 1
    append(out, {static_cast<unsigned>((pps.size() >> 8) & 0xFFu), static_cast<unsigned>(pps.size() & 0xFFu)});
    out.insert(out.end(), pps.begin(), pps.end());
    return frame;
}

GeneratedFrame MediaSource::audio_sequence_header() const {
    GeneratedFrame frame;
    frame.kind = FrameKind::AudioSequenceHeader;
    frame.timestamp_ms = 0;
    // 0xAF = SoundFormat 10 (AAC), 44 kHz, 16-bit, stereo; 0x00 = seq header.
    // 0x12 0x10 = AudioSpecificConfig: AAC-LC, sample-rate index 4 (44.1 kHz),
    // 2 channels.
    append(frame.payload, {0xAF, 0x00, 0x12, 0x10});
    return frame;
}

std::vector<GeneratedFrame> MediaSource::frames_until(std::uint32_t up_to_ms) {
    std::vector<GeneratedFrame> out;

    const auto video_period_ms = 1000.0 / static_cast<double>(profile_.frames_per_second);
    const auto audio_period_ms =
        1000.0 * static_cast<double>(kAacSamplesPerFrame) / static_cast<double>(profile_.audio_sample_rate);

    const auto audio_frame_bytes = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<double>(profile_.audio_bitrate_bps) / 8.0 * audio_period_ms / 1000.0));

    // Merge the two timelines in timestamp order, exactly as a real encoder
    // muxes them, so the server sees interleaved A/V rather than bursts.
    for (;;) {
        const auto next_video_ms = static_cast<std::uint32_t>(static_cast<double>(next_video_index_) * video_period_ms);
        const auto next_audio_ms = static_cast<std::uint32_t>(static_cast<double>(next_audio_index_) * audio_period_ms);

        const bool video_due = next_video_ms <= up_to_ms;
        const bool audio_due = next_audio_ms <= up_to_ms;
        if (!video_due && !audio_due) break;

        const bool take_video = video_due && (!audio_due || next_video_ms <= next_audio_ms);

        GeneratedFrame frame;
        if (take_video) {
            const bool keyframe = profile_.keyframe_interval_frames != 0 &&
                                  (next_video_index_ % profile_.keyframe_interval_frames) == 0;
            frame.kind = keyframe ? FrameKind::VideoKeyframe : FrameKind::VideoInter;
            frame.timestamp_ms = next_video_ms;

            const std::size_t payload_size = next_video_payload_size(keyframe);
            auto& p = frame.payload;
            p.reserve(kVideoTagHeaderSize + 4 + 1 + payload_size);
            p.push_back(b(keyframe ? 0x17 : 0x27)); // FrameType 1/2, CodecID 7 (AVC)
            append(p, {0x01});                      // AVCPacketType 1 = NALU
            append(p, {0x00, 0x00, 0x00});          // composition time offset 0

            // One length-prefixed NAL: IDR (5) for keyframes, non-IDR (1).
            const auto nal_size = static_cast<std::uint32_t>(payload_size + 1);
            append_be32(p, nal_size);
            p.push_back(b(keyframe ? 0x65 : 0x41));
            fill_pattern(p, payload_size, keyframe ? 0x65 : 0x41);

            ++next_video_index_;
            ++video_frames_;
        } else {
            frame.kind = FrameKind::Audio;
            frame.timestamp_ms = next_audio_ms;

            auto& p = frame.payload;
            p.reserve(kAudioTagHeaderSize + audio_frame_bytes);
            append(p, {0xAF, 0x01}); // AAC 44 kHz 16-bit stereo, AACPacketType 1 = raw
            fill_pattern(p, audio_frame_bytes, 0xAF);

            ++next_audio_index_;
            ++audio_frames_;
        }

        bytes_generated_ += frame.payload.size();
        out.push_back(std::move(frame));
    }

    return out;
}

void MediaSource::restart() {
    next_video_index_ = 0;
    next_audio_index_ = 0;
}

bool MediaSource::verify_pattern(std::span<const std::byte> payload) noexcept {
    if (payload.empty()) return false;

    const auto first = static_cast<std::uint8_t>(payload[0]);

    // ---- audio ------------------------------------------------------------
    if (first == 0xAF) {
        if (payload.size() < kAudioTagHeaderSize) return false;
        const auto packet_type = static_cast<std::uint8_t>(payload[1]);
        if (packet_type == 0x00) return payload.size() == 4; // AudioSpecificConfig
        if (packet_type != 0x01) return false;
        for (std::size_t i = kAudioTagHeaderSize; i < payload.size(); ++i) {
            if (payload[i] != pattern_byte(i - kAudioTagHeaderSize, 0xAF)) return false;
        }
        return true;
    }

    // ---- video ------------------------------------------------------------
    if (first != 0x17 && first != 0x27) return false;
    if (payload.size() < kVideoTagHeaderSize) return false;

    const auto avc_packet_type = static_cast<std::uint8_t>(payload[1]);
    if (avc_packet_type == 0x00) {
        // Sequence header: verified structurally (configurationVersion and a
        // plausible length), not against the byte pattern — it carries real
        // SPS/PPS, not generated filler.
        return payload.size() > kVideoTagHeaderSize && static_cast<std::uint8_t>(payload[5]) == 0x01;
    }
    if (avc_packet_type != 0x01) return false;

    if (payload.size() < kVideoTagHeaderSize + 4 + 1) return false;
    std::uint32_t nal_size = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        nal_size = (nal_size << 8) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[5 + i]));
    }
    // The declared NAL length must exactly match the bytes that follow: this
    // is what catches truncation and mis-framing.
    if (payload.size() != kVideoTagHeaderSize + 4 + nal_size) return false;
    if (nal_size == 0) return false;

    const auto nal_header = static_cast<std::uint8_t>(payload[9]);
    const bool keyframe = first == 0x17;
    if (nal_header != (keyframe ? 0x65 : 0x41)) return false;

    const std::size_t body_offset = kVideoTagHeaderSize + 4 + 1;
    for (std::size_t i = body_offset; i < payload.size(); ++i) {
        if (payload[i] != pattern_byte(i - body_offset, nal_header)) return false;
    }
    return true;
}

} // namespace rtmp_server::loadgen
