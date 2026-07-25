#include "rtmp_server/management/url_builder.hpp"

namespace rtmp_server::management {

namespace {
std::string build_base(std::string_view host, std::uint16_t port, std::string_view application,
                        std::string_view path_segment) {
    std::string out;
    out.reserve(7 + host.size() + 6 + application.size() + 1 + path_segment.size());
    out += "rtmp://";
    out += host;
    out += ':';
    out += std::to_string(port);
    out += '/';
    out += application;
    out += '/';
    out += path_segment;
    return out;
}
} // namespace

std::string build_publish_url(std::string_view host, std::uint16_t port, std::string_view application,
                               std::string_view stream_key) {
    return build_base(host, port, application, stream_key);
}

std::string build_playback_url(std::string_view host, std::uint16_t port, std::string_view application,
                                std::string_view stream_name) {
    return build_base(host, port, application, stream_name);
}

std::string append_signed_token(std::string_view base_url, std::string_view token, std::int64_t expires_at_unix) {
    std::string out(base_url);
    out += "?token=";
    out += token;
    out += "&expires=";
    out += std::to_string(expires_at_unix);
    return out;
}

} // namespace rtmp_server::management
