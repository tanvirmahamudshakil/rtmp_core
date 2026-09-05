#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::protocol {

// A parsed plain-RTMP URL: `rtmp://host[:port]/application/stream[?query]`.
// Query parameters belong to the stream name (the convention token-
// authenticated RTMP origins use), while tc_url identifies the application
// connection endpoint sent in `connect`.
struct RtmpUrl {
    std::string host;
    std::uint16_t port = 1935;
    std::string application;
    std::string stream;
    std::string tc_url;
};

// Shared by every RTMP client this server opens — the source-transcode puller
// that plays from an origin, and the relay/stream-target publisher that
// publishes to one. Rejects anything that could smuggle a second request line
// or a credential into the wire format: whitespace/control characters,
// fragments and userinfo.
[[nodiscard]] core::Result<RtmpUrl> parse_rtmp_url(std::string_view url);

} // namespace rtmp_server::protocol
