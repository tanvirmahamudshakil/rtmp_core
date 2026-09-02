#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Minimal MSB-first bit reader plus the RBSP unescaping every H.264/H.265
// parameter-set parser needs. Split out of the codec helpers because both
// media::h264 and media::hevc need exactly this and nothing more; keeping it
// header-only avoids a third translation unit for ~80 lines of shifting.
//
// Every accessor is bounds-checked and saturates rather than reading past the
// buffer: parameter sets arrive from the network, so a truncated one must
// yield a parse failure, never an out-of-bounds read.
namespace rtmp_server::media::bitstream {

// Removes H.264/H.265 emulation-prevention bytes (0x00 0x00 0x03 -> 0x00
// 0x00). `nal` must already have its start code stripped.
[[nodiscard]] inline std::vector<std::byte> unescape_rbsp(std::span<const std::byte> nal) {
    std::vector<std::byte> out;
    out.reserve(nal.size());
    std::size_t zeros = 0;
    for (const std::byte b : nal) {
        if (zeros >= 2 && b == std::byte{0x03}) {
            zeros = 0;
            continue; // emulation prevention byte
        }
        out.push_back(b);
        zeros = (b == std::byte{0x00}) ? zeros + 1 : 0;
    }
    return out;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] bool overrun() const noexcept { return overrun_; }
    [[nodiscard]] std::size_t bits_left() const noexcept {
        const std::size_t total = data_.size() * 8;
        return position_ >= total ? 0 : total - position_;
    }

    // Reads `count` (0..32) bits MSB-first. Sets the overrun flag and
    // returns 0 once the buffer is exhausted, so a caller can parse
    // optimistically and check overrun() once at the end.
    std::uint32_t u(std::uint32_t count) noexcept {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            value = (value << 1) | bit();
        }
        return value;
    }

    bool flag() noexcept { return bit() != 0; }

    // Unsigned Exp-Golomb (ue(v)). Leading-zero runs are capped at 32 so a
    // corrupt bitstream cannot spin here.
    std::uint32_t ue() noexcept {
        std::uint32_t leading_zeros = 0;
        while (leading_zeros < 32 && bit() == 0 && !overrun_) ++leading_zeros;
        if (leading_zeros >= 32 || overrun_) {
            overrun_ = true;
            return 0;
        }
        return ((1u << leading_zeros) - 1u) + u(leading_zeros);
    }

    // Signed Exp-Golomb (se(v)).
    std::int32_t se() noexcept {
        const std::uint32_t code = ue();
        const std::int32_t magnitude = static_cast<std::int32_t>((code + 1u) / 2u);
        return (code % 2u == 0u) ? -magnitude : magnitude;
    }

    void skip(std::uint32_t count) noexcept {
        for (std::uint32_t i = 0; i < count; ++i) (void)bit();
    }

private:
    std::uint32_t bit() noexcept {
        if (position_ >= data_.size() * 8) {
            overrun_ = true;
            return 0;
        }
        const std::size_t index = position_ >> 3;
        const std::uint32_t shift = 7u - static_cast<std::uint32_t>(position_ & 7u);
        ++position_;
        return (static_cast<std::uint32_t>(data_[index]) >> shift) & 1u;
    }

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    bool overrun_ = false;
};

} // namespace rtmp_server::media::bitstream
