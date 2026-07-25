#pragma once

#include <string>
#include <string_view>

namespace rtmp_server::core {

// Hex-encoded HMAC-SHA256 of `message` keyed by `secret`, backed by
// OpenSSL's EVP MAC API (same OpenSSL::Crypto link already used by
// random.hpp). Used to sign playback/publish tokens (see
// docs/rtmp_promot.md "RTMP Link Generation") — never roll a custom MAC.
[[nodiscard]] std::string hmac_sha256_hex(std::string_view secret, std::string_view message);

// Hex-encoded SHA-256 digest of `data`. Used to store stream keys hashed
// rather than in plaintext (docs/rtmp_promot.md "hashed key persistence
// where practical") — the raw key is only ever returned to the caller at
// creation/rotation time, never persisted or logged.
[[nodiscard]] std::string sha256_hex(std::string_view data);

// Constant-time comparison: always inspects every byte of the shorter
// argument's length-matched prefix rather than short-circuiting on the
// first mismatch, so comparison time does not leak how many leading bytes
// of a secret/token/signature guess were correct (timing side-channel
// hardening required for tokens/keys, per docs/rtmp_promot.md
// "Authentication" — "use constant-time comparison for secrets"). Returns
// false immediately (non-constant-time) only when the lengths differ, which
// leaks length, not content — the accepted tradeoff every constant-time
// compare in practice makes (lengths of hex-encoded fixed-size digests are
// public anyway).
[[nodiscard]] bool constant_time_equals(std::string_view a, std::string_view b) noexcept;

} // namespace rtmp_server::core
