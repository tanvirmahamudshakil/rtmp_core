#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/aac_decoder.hpp"
#include "rtmp_server/transcoding/native/aac_encoder.hpp"
#include "rtmp_server/transcoding/native/aac_params.hpp"
#include "rtmp_server/transcoding/preset.hpp"

namespace rtmp_server::transcoding::native {

// One ADTS-framed AAC access unit with a 90 kHz presentation timestamp, matching
// media::ts::TsMuxer::write_audio.
struct AudioAccessUnit {
    std::vector<std::byte> adts;
    std::int64_t pts_90k = 0;
};

// A complete AAC -> AAC audio transcode for one source stream: fdk decode ->
// (optional profile/bitrate change) -> fdk encode. The source is decoded to PCM
// and re-encoded at the preset's target bitrate/profile, so a rendition ladder
// can drop to 96k HE-AAC for mobile while the source stays high-rate. No
// resampling is done — the encoder follows the source sample rate — so decode
// and encode frame sizes line up without a rate converter.
//
// Output timestamps are derived from the running output sample count (anchored
// to the first frame's timestamp), which stays exact across the encoder's
// priming delay rather than copying possibly-jittery input timestamps.
class NativeAudioTranscoder {
public:
    NativeAudioTranscoder(Preset preset, AacQualityOptions quality);

    // FLV AACPacketType 0: the AudioSpecificConfig. Must precede the first frame.
    [[nodiscard]] core::Result<void> set_sequence_header(std::span<const std::byte> asc);

    // FLV AACPacketType 1: one raw AAC frame at RTMP timestamp `dts_ms`.
    [[nodiscard]] core::Result<void> transcode_frame(std::span<const std::byte> aac_frame,
                                                     std::int64_t dts_ms,
                                                     std::vector<AudioAccessUnit>& out);

    [[nodiscard]] core::Result<void> finish(std::vector<AudioAccessUnit>& out);

    [[nodiscard]] bool encoder_ready() const noexcept { return encoder_open_; }

private:
    [[nodiscard]] core::Result<void> ensure_encoder(std::uint32_t sample_rate,
                                                    std::uint32_t channels);
    void emit(const std::vector<EncodedAudioFrame>& frames, std::vector<AudioAccessUnit>& out);

    Preset preset_;
    AacQualityOptions quality_;
    AacDecoder decoder_;
    AacEncoder encoder_;
    PcmBlock pcm_;
    bool configured_ = false;
    bool encoder_open_ = false;
    bool base_pts_set_ = false;
    std::int64_t base_pts_90k_ = 0;
    std::uint64_t emitted_samples_ = 0; // per-channel output samples emitted so far
    std::uint32_t sample_rate_ = 0;
};

} // namespace rtmp_server::transcoding::native
