#include "rtmp_server/core/random.hpp"

#include <openssl/rand.h>

#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

namespace rtmp_server::core {

void secure_random_bytes(std::span<std::byte> out) {
    if (out.empty()) return;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()),
                    static_cast<int>(out.size())) != 1) {
        // RAND_bytes failure means the platform CSPRNG is unavailable —
        // this is a fatal startup-class condition, not a recoverable one,
        // since every stream key/token depends on it being secure.
        throw std::runtime_error("secure_random_bytes: RAND_bytes failed");
    }
}

std::string generate_secure_token(std::size_t byte_length) {
    std::vector<std::byte> raw(byte_length);
    secure_random_bytes(raw);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(byte_length * 2);
    for (auto b : raw) {
        auto v = static_cast<unsigned char>(b);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

std::string generate_uuid_v4() {
    std::array<std::byte, 16> bytes{};
    secure_random_bytes(bytes);

    // Set version (4) and variant (10xx) bits per RFC 4122.
    bytes[6] = static_cast<std::byte>((static_cast<unsigned char>(bytes[6]) & 0x0F) | 0x40);
    bytes[8] = static_cast<std::byte>((static_cast<unsigned char>(bytes[8]) & 0x3F) | 0x80);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        auto v = static_cast<unsigned char>(bytes[i]);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 0x0F]);
    }
    return out;
}

std::uint64_t generate_id64() {
    std::array<std::byte, 8> bytes{};
    secure_random_bytes(bytes);
    std::uint64_t value = 0;
    for (auto b : bytes) {
        value = (value << 8) | static_cast<unsigned char>(b);
    }
    return value == 0 ? 1 : value;
}

} // namespace rtmp_server::core
