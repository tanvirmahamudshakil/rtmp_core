#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/result.hpp"

// AAC helpers for HLS passthrough. RTMP delivers audio as FLV AUDIODATA: a
// 1-byte SoundFormat/Rate/Size/Type header, then for AAC (SoundFormat 10) a
// 1-byte AACPacketType — 0 = AudioSpecificConfig ("sequence header"),
// 1 = raw AAC frame.
//
// MPEG-TS carries AAC as ADTS, so each raw frame must be given a 7-byte ADTS
// header derived from the AudioSpecificConfig. No re-encoding happens here:
// the AAC payload bytes are passed through untouched.
namespace rtmp_server::media::aac {

inline constexpr std::uint8_t kSoundFormatAac = 10;
inline constexpr std::uint8_t kAacPacketTypeSequenceHeader = 0;
inline constexpr std::uint8_t kAacPacketTypeRaw = 1;
inline constexpr std::size_t kAdtsHeaderSize = 7; // without CRC

// MPEG-4 sampling-frequency index table (ISO/IEC 14496-3).
inline constexpr std::array<std::uint32_t, 13> kSamplingFrequencies = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

// Decoded AudioSpecificConfig — everything an ADTS header needs.
struct AudioSpecificConfig {
    std::uint8_t object_type = 2;            // 2 = AAC-LC
    std::uint8_t sampling_frequency_index = 4; // 4 = 44100 Hz
    std::uint8_t channel_configuration = 2;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept {
        return sampling_frequency_index < kSamplingFrequencies.size()
                   ? kSamplingFrequencies[sampling_frequency_index]
                   : 0;
    }
};

struct AudioTag {
    std::uint8_t sound_format = kSoundFormatAac;
    std::uint8_t aac_packet_type = kAacPacketTypeRaw;
    std::span<const std::byte> body; // AudioSpecificConfig or raw AAC frame
};

// Parses the FLV audio header. Fails on a truncated payload or a non-AAC
// SoundFormat (HLS passthrough requires AAC).
[[nodiscard]] core::Result<AudioTag> parse_audio_tag(std::span<const std::byte> payload);

// Parses the 2-or-more-byte AudioSpecificConfig bit layout:
// audioObjectType(5) samplingFrequencyIndex(4) channelConfiguration(4).
// Handles the escape value 31 (6 extra bits of object type).
[[nodiscard]] core::Result<AudioSpecificConfig> parse_audio_specific_config(std::span<const std::byte> config);

// Appends a 7-byte ADTS header for a frame of `aac_frame_length` payload
// bytes, followed by nothing else (the caller appends the payload).
void append_adts_header(std::vector<std::byte>& out, const AudioSpecificConfig& config,
                        std::size_t aac_frame_length);

} // namespace rtmp_server::media::aac
