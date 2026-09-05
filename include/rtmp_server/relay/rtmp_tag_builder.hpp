#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/h264/avc.hpp"

namespace rtmp_server::relay {

// Splits an Annex B (start-code prefixed) access unit into its NAL units,
// stripping the start codes. Needed because the native encoders
// (H264Encoder/HevcEncoder) emit Annex B — the same wire form MPEG-TS carries
// — but rebuilding an RTMP/FLV tag requires AVCC framing (4-byte length
// prefixes) instead, the one direction this codebase's H.264 helpers
// (media::h264::avcc_to_annexb) did not previously need.
//
// The returned spans alias `annexb` -- they are views, not copies. `annexb`
// must outlive every span in the result; in particular, do not pass a
// temporary (`split_annexb_nal_units(some_function_returning_by_value())`)
// and keep using the result past that full expression, or every span will
// dangle the moment the temporary is destroyed.
[[nodiscard]] std::vector<std::span<const std::byte>> split_annexb_nal_units(
    std::span<const std::byte> annexb);

// Builds RTMP FLV tag payloads (the byte layout published as a
// MessageTypeId::Video / MessageTypeId::Audio message body — NOT a full
// RtmpMessage; the caller sets chunk stream, message stream id and
// timestamp) from the encoder output a native transcode rendition produces.
// This is the exact inverse of RtmpTagConverter: where that class turns a
// publisher's FLV tags into Annex B/ADTS for decoding, this builds FLV tags
// back from freshly-encoded Annex B/ADTS so a rendition can be published
// onward over RTMP (transcoder tier remote dispatch: a transcoder node
// re-encodes a pulled source and pushes each rendition back to the origin as
// though a real encoder had sent it — docs/transcoder-dispatch.md).
//
// One instance belongs to one rendition's video (or audio) elementary stream
// and is not thread-safe.
class RtmpVideoTagBuilder {
public:
    // Builds the AVCDecoderConfigurationRecord tag from the first keyframe's
    // SPS/PPS. Must be called (and its result published) before any
    // build_frame() tag, exactly once per parameter-set change. Fails if the
    // access unit carries no SPS or no PPS.
    [[nodiscard]] core::Result<std::vector<std::byte>> build_sequence_header(
        std::span<const std::byte> keyframe_annexb);

    // Builds one AVCC-framed FLV video tag. `sequence_header_sent` must be
    // true (i.e. build_sequence_header() must have already been called and
    // published) or this returns InvalidStateTransition — matching the
    // ordering RtmpTagConverter enforces on the decode side, so a target that
    // receives this stream sees exactly the tag order a real encoder
    // produces.
    [[nodiscard]] core::Result<std::vector<std::byte>> build_frame(
        std::span<const std::byte> annexb, std::int64_t pts_90k, std::int64_t dts_90k,
        bool keyframe);

    [[nodiscard]] bool has_sequence_header() const noexcept { return config_.valid(); }

private:
    media::h264::AvcDecoderConfig config_;
};

class RtmpAudioTagBuilder {
public:
    // Builds the AudioSpecificConfig tag from the first ADTS frame's header.
    // Idempotent in effect (later calls simply republish the same config,
    // matching a real encoder's fixed sample rate/channel count).
    [[nodiscard]] core::Result<std::vector<std::byte>> build_sequence_header(
        std::span<const std::byte> adts_frame);

    // Builds one raw-AAC FLV audio tag from one ADTS frame (strips its 7-byte
    // header; the payload passes through unmodified, same passthrough
    // guarantee RtmpTagConverter documents for the decode direction).
    [[nodiscard]] core::Result<std::vector<std::byte>> build_frame(
        std::span<const std::byte> adts_frame);

    [[nodiscard]] bool has_sequence_header() const noexcept { return sequence_header_sent_; }

private:
    bool sequence_header_sent_ = false;
};

} // namespace rtmp_server::relay
