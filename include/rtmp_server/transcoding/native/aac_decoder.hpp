#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::transcoding::native {

// One decoded block of interleaved signed-16-bit PCM.
struct PcmBlock {
    std::vector<std::int16_t> samples; // interleaved, size = frames * channels
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    [[nodiscard]] std::uint32_t frames() const noexcept {
        return channels ? static_cast<std::uint32_t>(samples.size() / channels) : 0;
    }
};

// Wraps a libfdk-aac decoder configured from an AudioSpecificConfig (the FLV
// AAC sequence header). Input is a raw AAC access unit (FLV AACPacketType 1),
// output is interleaved S16 PCM. libfdk-aac is the highest-quality open AAC
// implementation, and using it for both ends keeps the transcode transparent.
class AacDecoder {
public:
    AacDecoder();
    ~AacDecoder();
    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    // Configures the decoder from the raw AudioSpecificConfig bytes (for RTMP/FLV
    // sources, which deliver the config out of band).
    [[nodiscard]] core::Result<void> configure(std::span<const std::byte> audio_specific_config);

    // Configures the decoder for self-describing ADTS input (an HLS/TS source),
    // where each frame carries its own header and no out-of-band config exists.
    [[nodiscard]] core::Result<void> configure_adts();

    // Decodes all complete AAC frames in the supplied access unit/PES. On
    // success `produced` is true when a PCM block was written to `out` (rarely
    // a frame yields nothing until the decoder is primed).
    [[nodiscard]] core::Result<void> decode(std::span<const std::byte> aac_frame, PcmBlock& out,
                                            bool& produced);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rtmp_server::transcoding::native
