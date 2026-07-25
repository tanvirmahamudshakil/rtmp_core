#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Big-endian read/write helpers, including RTMP's 24-bit integers.
// RTMP wire format is big-endian throughout (handshake sizes, chunk headers,
// AMF0 numbers), independent of host endianness.
namespace rtmp_server::core {

inline std::uint16_t read_u16_be(std::span<const std::byte, 2> bytes) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8) | static_cast<std::uint16_t>(bytes[1]));
}

inline std::uint32_t read_u24_be(std::span<const std::byte, 3> bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 16) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           static_cast<std::uint32_t>(bytes[2]);
}

inline std::uint32_t read_u32_be(std::span<const std::byte, 4> bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

inline void write_u16_be(std::span<std::byte, 2> out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::byte>((value >> 8) & 0xFF);
    out[1] = static_cast<std::byte>(value & 0xFF);
}

inline void write_u24_be(std::span<std::byte, 3> out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>((value >> 16) & 0xFF);
    out[1] = static_cast<std::byte>((value >> 8) & 0xFF);
    out[2] = static_cast<std::byte>(value & 0xFF);
}

inline void write_u32_be(std::span<std::byte, 4> out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>((value >> 24) & 0xFF);
    out[1] = static_cast<std::byte>((value >> 16) & 0xFF);
    out[2] = static_cast<std::byte>((value >> 8) & 0xFF);
    out[3] = static_cast<std::byte>(value & 0xFF);
}

} // namespace rtmp_server::core
