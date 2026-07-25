#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::management {

// Signed playback/publish tokens (docs/rtmp_promot.md "RTMP Link
// Generation" — "rtmp://.../<stream-name>?token=<signed-token>&expires=
// <unix-time>"). The token is HMAC-SHA256(secret, application + ":" + name +
// ":" + expires_at_unix), hex-encoded — deterministic and stateless, so
// verification never needs a database lookup or shared token store (only
// the expiry timestamp, which is carried in the URL and covered by the
// signature, needs checking against the current time).
//
// `secret` is always the deployment's `ServerConfig::token_signing_secret`
// (see core/config.hpp) — never a per-stream secret, so rotating a stream's
// publish key does not invalidate outstanding playback tokens for it.
[[nodiscard]] std::string sign_token(std::string_view secret, std::string_view application, std::string_view name,
                                      std::int64_t expires_at_unix);

// Recomputes the expected token and compares it to `token` in constant
// time, then checks `now_unix <= expires_at_unix`. Returns
// core::ErrorCode::Unauthorized for a bad/forged signature and
// core::ErrorCode::ExpiredToken for a correctly-signed but expired token —
// callers that care about the distinction (e.g. logging "expired" vs
// "forged" differently) can inspect the returned Error's code.
[[nodiscard]] core::Result<void> verify_token(std::string_view secret, std::string_view application,
                                               std::string_view name, std::string_view token,
                                               std::int64_t expires_at_unix, std::int64_t now_unix);

} // namespace rtmp_server::management
