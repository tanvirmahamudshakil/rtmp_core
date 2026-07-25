#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/core/byte_order.hpp"

namespace rtmp_server::core {

// Growable output buffer for encoding protocol messages (chunk headers,
// AMF0, FLV tags). Not bounds-checked against a fixed capacity by design —
// callers writing to the network still go through BufferPool-backed
// SharedBuffer, this is the assembly step before that hand-off.
class ByteWriter {
public:
    [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

    void write_u8(std::byte value) { buffer_.push_back(value); }

    void write_u16_be(std::uint16_t value) {
        std::array<std::byte, 2> tmp{};
        core::write_u16_be(tmp, value);
        buffer_.insert(buffer_.end(), tmp.begin(), tmp.end());
    }

    void write_u24_be(std::uint32_t value) {
        std::array<std::byte, 3> tmp{};
        core::write_u24_be(tmp, value);
        buffer_.insert(buffer_.end(), tmp.begin(), tmp.end());
    }

    void write_u32_be(std::uint32_t value) {
        std::array<std::byte, 4> tmp{};
        core::write_u32_be(tmp, value);
        buffer_.insert(buffer_.end(), tmp.begin(), tmp.end());
    }

    void write_bytes(std::span<const std::byte> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

private:
    std::vector<std::byte> buffer_;
};

} // namespace rtmp_server::core
