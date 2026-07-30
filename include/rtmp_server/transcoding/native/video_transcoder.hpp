#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"
#include "rtmp_server/transcoding/native/h264_decoder.hpp"
#include "rtmp_server/transcoding/native/hevc_encoder.hpp"
#include "rtmp_server/transcoding/native/hevc_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"
#include "rtmp_server/transcoding/native/scaler.hpp"

namespace rtmp_server::transcoding::native {

struct VideoTranscoderConfig {
    Preset preset;
    HevcQualityOptions quality;
    // Assumed source frame rate, used for encoder rate-control setup. The GOP
    // length in frames is derived from preset.keyframe_interval independently,
    // so mild fps mismatch only affects lookahead sizing, not segment cadence.
    std::uint32_t fps_num = 30;
    std::uint32_t fps_den = 1;
};

// A complete CPU H.264 -> HEVC transcode for one source video stream:
//   AVCC sample -> Annex B -> openh264 decode -> libyuv scale -> x265 encode.
// Fed with FLV video payloads (sequence header first, then samples); emits
// HEVC access units ready for the TS muxer. The encoder is built lazily on the
// first decoded frame, once the true source resolution is known, so FitMode
// values that depend on the source aspect ratio resolve correctly.
class NativeVideoTranscoder {
public:
    explicit NativeVideoTranscoder(VideoTranscoderConfig config);

    // Accepts an AVCDecoderConfigurationRecord (FLV AVCPacketType 0). Resets
    // the decoder's parameter sets. Must be called before the first sample.
    [[nodiscard]] core::Result<void> set_decoder_config(std::span<const std::byte> record);

    // Accepts one AVCC sample (FLV AVCPacketType 1) and appends any resulting
    // HEVC access units to `out`. `dts_ms` is the RTMP timestamp; `cts_ms` is
    // the FLV composition-time offset (PTS - DTS).
    [[nodiscard]] core::Result<void> transcode_sample(std::span<const std::byte> avcc_sample,
                                                      std::int64_t dts_ms, std::int32_t cts_ms,
                                                      bool keyframe,
                                                      std::vector<EncodedAccessUnit>& out);

    // Drains the encoder at end of stream.
    [[nodiscard]] core::Result<void> finish(std::vector<EncodedAccessUnit>& out);

    [[nodiscard]] bool encoder_ready() const noexcept { return encoder_open_; }

private:
    [[nodiscard]] core::Result<void> ensure_encoder(std::uint32_t src_w, std::uint32_t src_h);

    VideoTranscoderConfig config_;
    media::h264::AvcDecoderConfig decoder_config_;
    H264Decoder decoder_;
    Scaler scaler_;
    HevcEncoder encoder_;
    YuvFrame decoded_;
    YuvFrame scaled_;
    bool decoder_open_ = false;
    bool encoder_open_ = false;
};

} // namespace rtmp_server::transcoding::native
