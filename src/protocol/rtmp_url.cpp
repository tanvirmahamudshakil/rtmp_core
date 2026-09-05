#include "rtmp_server/protocol/rtmp_url.hpp"

#include <charconv>
#include <limits>

namespace rtmp_server::protocol {
namespace {

core::Error url_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration,
                       core::ErrorCategory::Configuration, std::move(message));
}

} // namespace

core::Result<RtmpUrl> parse_rtmp_url(std::string_view url) {
    constexpr std::string_view scheme = "rtmp://";
    if (url.find('#') != std::string_view::npos) {
        return url_error("RTMP URL fragments are not sent to the origin");
    }
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20u) {
            return url_error("RTMP URL contains whitespace or a control character");
        }
    }
    if (!url.starts_with(scheme)) {
        return url_error("URL must use plain rtmp:// (RTMPS is not enabled in the native client)");
    }
    const auto remainder = url.substr(scheme.size());
    const auto slash = remainder.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= remainder.size()) {
        return url_error("RTMP URL must be rtmp://host[:port]/application/stream");
    }

    const auto authority = remainder.substr(0, slash);
    if (authority.find('@') != std::string_view::npos) {
        return url_error("userinfo in an RTMP URL is unsupported; pass authentication in the stream query");
    }

    std::string_view host;
    std::string_view port_text;
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) return url_error("invalid bracketed IPv6 host");
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return url_error("invalid RTMP authority");
            port_text = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) return url_error("IPv6 RTMP hosts must be enclosed in brackets");
            host = authority.substr(0, colon);
            port_text = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty()) return url_error("RTMP URL has an empty host");

    std::uint16_t port = 1935;
    if (!port_text.empty()) {
        unsigned parsed_port = 0;
        const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
        if (ec != std::errc{} || end != port_text.data() + port_text.size() || parsed_port == 0 ||
            parsed_port > std::numeric_limits<std::uint16_t>::max()) {
            return url_error("RTMP URL has an invalid port");
        }
        port = static_cast<std::uint16_t>(parsed_port);
    } else if (authority.ends_with(':')) {
        return url_error("RTMP URL has an empty port");
    }

    const auto path_and_query = remainder.substr(slash + 1);
    const auto query = path_and_query.find('?');
    const auto path = path_and_query.substr(0, query);
    const auto app_slash = path.find('/');
    if (app_slash == std::string_view::npos || app_slash == 0 || app_slash + 1 >= path.size()) {
        return url_error("RTMP URL must contain both application and stream names");
    }

    RtmpUrl parsed;
    parsed.host = std::string(host);
    parsed.port = port;
    parsed.application = std::string(path.substr(0, app_slash));
    parsed.stream = std::string(path.substr(app_slash + 1));
    if (query != std::string_view::npos) parsed.stream += std::string(path_and_query.substr(query));
    parsed.tc_url = "rtmp://" + std::string(authority) + "/" + parsed.application;
    return parsed;
}

} // namespace rtmp_server::protocol
