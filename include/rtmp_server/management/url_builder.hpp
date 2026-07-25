#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rtmp_server::management {

// Pure string-building helpers for the two RTMP URL shapes documented in
// docs/rtmp_promot.md "RTMP Link Generation". Deliberately free functions
// with no state/validation (that's StreamManager's job) so they're trivial
// to unit test and reuse from both stream creation and key rotation.

// rtmp://<host>:<port>/<application>/<stream-key> — the secret path segment
// a publisher's RTMP client uses in its publish() call.
[[nodiscard]] std::string build_publish_url(std::string_view host, std::uint16_t port,
                                             std::string_view application, std::string_view stream_key);

// rtmp://<host>:<port>/<application>/<stream-name> — the public path segment
// viewers use in their play() call. Never contains the secret stream key.
[[nodiscard]] std::string build_playback_url(std::string_view host, std::uint16_t port,
                                              std::string_view application, std::string_view stream_name);

// Appends a signed-token query string to an already-built playback URL:
// "?token=<token>&expires=<expires_at_unix>". Used when playback
// authorization is required (docs/rtmp_promot.md "Signed playback link").
[[nodiscard]] std::string append_signed_token(std::string_view base_url, std::string_view token,
                                               std::int64_t expires_at_unix);

} // namespace rtmp_server::management
