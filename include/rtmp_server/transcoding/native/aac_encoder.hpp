#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/aac_decoder.hpp"
#include "rtmp_server/transcoding/native/aac_params.hpp"

namespace rtmp_server::transcoding::native {

// One encoded AAC access unit, ADTS-framed (self-describing 7-byte header +
// payload), ready for media::ts::TsMuxer::write_audio. `samples_per_channel`
// lets the caller advance the presentation clock exactly.
struct EncodedAudioFrame {
    std::vector<std::byte> adts;
    std::uint32_t samples_per_channel = 0;
};

// Wraps a libfdk-aac encoder emitting ADTS. PCM is buffered internally so the
// caller can push arbitrarily sized blocks; complete AAC frames are returned as
// enough samples accumulate. Sample rate/channels are fixed for the encoder's
// lifetime (they follow the decoded source; no resampling is performed).
class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    [[nodiscard]] core::Result<void> open(const AacParamSet& params);

    // Buffers `pcm` (interleaved S16 at the configured rate/channels) and
    // appends every complete AAC frame it makes possible to `out`.
    [[nodiscard]] core::Result<void> encode(const PcmBlock& pcm,
                                            std::vector<EncodedAudioFrame>& out);

    // Drains buffered PCM (zero-padding a final partial frame) at end of stream.
    [[nodiscard]] core::Result<void> flush(std::vector<EncodedAudioFrame>& out);

    [[nodiscard]] std::uint32_t frame_length() const noexcept { return frame_length_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }

private:
    [[nodiscard]] core::Result<void> open_with_aot(const AacParamSet& params, int aot);
    [[nodiscard]] core::Result<void> drain(std::vector<EncodedAudioFrame>& out, bool flushing);
    [[nodiscard]] core::Result<bool> encode_one(const std::int16_t* interleaved, int in_samples,
                                                EncodedAudioFrame& frame);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::int16_t> buffer_; // leftover interleaved PCM awaiting a full frame
    std::uint32_t frame_length_ = 1024;
    std::uint32_t channels_ = 2;
    std::uint32_t sample_rate_ = 44100;
};

} // namespace rtmp_server::transcoding::native
