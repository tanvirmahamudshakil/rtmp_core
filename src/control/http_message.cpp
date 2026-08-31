#include "rtmp_server/control/http_message.hpp"

#include <charconv>
#include <cctype>

namespace rtmp_server::control {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool parse_request_head(std::string_view head, HttpRequest& out) {
    const std::size_t line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);

    const std::size_t sp1 = request_line.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos
                                                          : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return false;

    out.method = std::string(request_line.substr(0, sp1));
    out.http_version = std::string(request_line.substr(sp2 + 1));

    const std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (const auto qpos = target.find('?'); qpos != std::string_view::npos) {
        out.path = std::string(target.substr(0, qpos));
        out.query = std::string(target.substr(qpos + 1));
    } else {
        out.path = std::string(target);
        out.query.clear();
    }

    if (line_end == std::string_view::npos) return true; // request line only

    std::size_t pos = line_end + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view line = head.substr(pos, next - pos);
        if (const auto colon = line.find(':'); colon != std::string_view::npos) {
            std::size_t vstart = colon + 1;
            while (vstart < line.size() && line[vstart] == ' ') ++vstart;
            // Later duplicates overwrite earlier ones, matching the previous
            // behaviour of both servers.
            out.headers[to_lower(line.substr(0, colon))] = std::string(line.substr(vstart));
        }
        pos = next + 2;
    }
    return true;
}

std::size_t content_length_of(const HttpRequest& request, bool& valid) {
    valid = true;
    const auto it = request.headers.find("content-length");
    if (it == request.headers.end()) return 0;

    const std::string& value = it->second;
    std::size_t length = 0;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto result = std::from_chars(begin, end, length);
    if (result.ec != std::errc{} || result.ptr != end || value.empty()) {
        valid = false;
        return 0;
    }
    return length;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return status < 400 ? "OK" : "Error";
    }
}

std::string serialize_response_head(const HttpResponse& response, bool keep_alive) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " ";
    out += reason_phrase(response.status);
    out += "\r\n";
    out += "Content-Type: " + response.content_type + "\r\n";
    // A handler may set Content-Length itself (a HEAD response describes the
    // body it would have sent while carrying none). Emitting our own too
    // would produce a duplicate header, which is a request-smuggling hazard,
    // so the handler's value wins.
    if (!response.headers.contains("Content-Length")) {
        out += "Content-Length: " + std::to_string(response.payload_size()) + "\r\n";
    }
    out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (const auto& [key, value] : response.headers) out += key + ": " + value + "\r\n";
    out += "\r\n";
    return out;
}

} // namespace rtmp_server::control
