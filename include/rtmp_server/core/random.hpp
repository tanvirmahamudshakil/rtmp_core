#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rtmp_server::core {

// Cryptographically secure random bytes, backed by OpenSSL's RAND_bytes.
// Used for stream keys, tokens, and connection/operation IDs where
// unpredictability matters (see docs/security.md, "no key enumeration").
void secure_random_bytes(std::span<std::byte> out);

// Hex-encoded secure random string of `byte_length` random bytes
// (output length is 2 * byte_length characters).
[[nodiscard]] std::string generate_secure_token(std::size_t byte_length = 32);

// UUID v4 string, e.g. "d03d5ba9-35d9-4d88-b92f-762f6942a269".
[[nodiscard]] std::string generate_uuid_v4();

// 64-bit random identifier, non-zero. Suitable for connection/operation IDs
// combined with a generation counter (see docs/architecture.md section 6).
[[nodiscard]] std::uint64_t generate_id64();

} // namespace rtmp_server::core
