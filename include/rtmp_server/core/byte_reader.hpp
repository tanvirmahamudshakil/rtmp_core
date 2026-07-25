#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::core {

// Bounds-checked cursor over an untrusted byte span. Every read returns
// std::nullopt instead of throwing or reading out of bounds — this is the
// primary defense against malformed RTMP/AMF0 input.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
    [[nodiscard]] bool has_remaining(std::size_t n) const noexcept { return remaining() >= n; }

    [[nodiscard]] std::optional<std::byte> read_u8() noexcept {
        if (!has_remaining(1)) return std::nullopt;
        return data_[position_++];
    }

    [[nodiscard]] std::optional<std::uint16_t> read_u16_be() noexcept {
        if (!has_remaining(2)) return std::nullopt;
        std::span<const std::byte, 2> slice(data_.data() + position_, 2);
        auto value = core::read_u16_be(slice);
        position_ += 2;
        return value;
    }

    [[nodiscard]] std::optional<std::uint32_t> read_u24_be() noexcept {
        if (!has_remaining(3)) return std::nullopt;
        std::span<const std::byte, 3> slice(data_.data() + position_, 3);
        auto value = core::read_u24_be(slice);
        position_ += 3;
        return value;
    }

    [[nodiscard]] std::optional<std::uint32_t> read_u32_be() noexcept {
        if (!has_remaining(4)) return std::nullopt;
        std::span<const std::byte, 4> slice(data_.data() + position_, 4);
        auto value = core::read_u32_be(slice);
        position_ += 4;
        return value;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> read_bytes(std::size_t n) noexcept {
        if (!has_remaining(n)) return std::nullopt;
        auto slice = data_.subspan(position_, n);
        position_ += n;
        return slice;
    }

    [[nodiscard]] bool skip(std::size_t n) noexcept {
        if (!has_remaining(n)) return false;
        position_ += n;
        return true;
    }

private:
    std::span<const std::byte> data_;
    std::size_t position_ = 0;
};

} // namespace rtmp_server::core
