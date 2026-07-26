#include "rtmp_server/media/aac/adts.hpp"

namespace rtmp_server::media::aac {

namespace {

core::Error malformed(std::string_view what) {
    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol, what);
}

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t i) {
    return static_cast<std::uint8_t>(data[i]);
}

} // namespace

core::Result<AudioTag> parse_audio_tag(std::span<const std::byte> payload) {
    if (payload.size() < 2) return malformed("FLV audio payload shorter than 2 bytes");

    const std::uint8_t header = byte_at(payload, 0);
    AudioTag tag;
    tag.sound_format = (header >> 4) & 0x0F;
    if (tag.sound_format != kSoundFormatAac) {
        return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                           "unsupported FLV sound format (HLS passthrough requires AAC)");
    }
    tag.aac_packet_type = byte_at(payload, 1);
    tag.body = payload.subspan(2);
    return tag;
}

core::Result<AudioSpecificConfig> parse_audio_specific_config(std::span<const std::byte> config) {
    if (config.size() < 2) return malformed("AudioSpecificConfig shorter than 2 bytes");

    const std::uint32_t b0 = byte_at(config, 0);
    const std::uint32_t b1 = byte_at(config, 1);

    AudioSpecificConfig out;
    std::uint32_t object_type = (b0 >> 3) & 0x1F;
    if (object_type == 31) {
        // Escape sequence: audioObjectTypeExt(6) follows, so every
        // subsequent field shifts by 6 bits.
        if (config.size() < 3) return malformed("truncated extended AudioSpecificConfig");
        const std::uint32_t b2 = byte_at(config, 2);
        const std::uint32_t bits = (b0 << 16) | (b1 << 8) | b2;
        object_type = 32 + ((bits >> 13) & 0x3F);
        out.sampling_frequency_index = static_cast<std::uint8_t>((bits >> 9) & 0x0F);
        out.channel_configuration = static_cast<std::uint8_t>((bits >> 5) & 0x0F);
    } else {
        out.sampling_frequency_index = static_cast<std::uint8_t>(((b0 & 0x07) << 1) | ((b1 >> 7) & 0x01));
        out.channel_configuration = static_cast<std::uint8_t>((b1 >> 3) & 0x0F);
    }

    if (out.sampling_frequency_index >= kSamplingFrequencies.size()) {
        // 15 means an explicit 24-bit rate follows; we do not support that
        // (no mainstream RTMP encoder emits it) and must not guess.
        return malformed("unsupported AAC sampling frequency index");
    }
    if (object_type == 0) return malformed("invalid AAC object type");

    out.object_type = static_cast<std::uint8_t>(object_type);
    return out;
}

void append_adts_header(std::vector<std::byte>& out, const AudioSpecificConfig& config,
                        std::size_t aac_frame_length) {
    const std::size_t total = aac_frame_length + kAdtsHeaderSize;
    // ADTS carries the frame length in 13 bits; longer frames cannot be
    // represented. Callers bound the payload before reaching here.
    const std::uint32_t length = static_cast<std::uint32_t>(total) & 0x1FFF;

    // ADTS profile is objectType - 1 (AAC-LC 2 -> profile 1).
    const std::uint8_t profile = static_cast<std::uint8_t>((config.object_type - 1) & 0x03);

    std::array<std::byte, kAdtsHeaderSize> h{};
    // syncword 0xFFF, MPEG-4, layer 0, protection_absent = 1 (no CRC)
    h[0] = std::byte{0xFF};
    h[1] = std::byte{0xF1};
    h[2] = static_cast<std::byte>((profile << 6) | ((config.sampling_frequency_index & 0x0F) << 2) |
                                  ((config.channel_configuration >> 2) & 0x01));
    h[3] = static_cast<std::byte>(((config.channel_configuration & 0x03) << 6) |
                                  static_cast<std::uint8_t>((length >> 11) & 0x03));
    h[4] = static_cast<std::byte>((length >> 3) & 0xFF);
    // 3 low bits of length, then buffer fullness 0x7FF (variable bitrate)
    h[5] = static_cast<std::byte>(((length & 0x07) << 5) | 0x1F);
    h[6] = std::byte{0xFC}; // remaining fullness bits + numberOfRawDataBlocks-1 = 0

    out.insert(out.end(), h.begin(), h.end());
}

} // namespace rtmp_server::media::aac
