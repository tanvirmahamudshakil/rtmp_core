#include "rtmp_server/dispatch/agent_http_client.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace rtmp_server::dispatch {
namespace {

core::Error network_error(core::ErrorCode code, std::string message) {
    return core::Error(code, core::ErrorCategory::Network, std::move(message));
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

core::Result<int> connect_to(const std::string& host, std::uint16_t port,
                             std::chrono::seconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (gai != 0) {
        return network_error(core::ErrorCode::NotFound,
                             "agent DNS lookup failed: " + std::string(gai_strerror(gai)));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last_error = "no address could be connected";
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }
        if (!set_nonblocking(fd)) {
            last_error = std::strerror(errno);
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc == 0) {
            ::freeaddrinfo(addresses);
            return fd;
        }
        if (errno != EINPROGRESS && errno != EINTR) {
            last_error = std::strerror(errno);
            ::close(fd);
            continue;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd item{fd, POLLOUT, 0};
            const int polled = ::poll(&item, 1, 100);
            if (polled < 0 && errno == EINTR) continue;
            if (polled <= 0) continue;
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 &&
                socket_error == 0) {
                ::freeaddrinfo(addresses);
                return fd;
            }
            last_error = std::strerror(socket_error == 0 ? errno : socket_error);
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    return network_error(core::ErrorCode::ConnectionTimedOut,
                         "agent connection failed: " + last_error);
}

} // namespace

AgentHttpClient::AgentHttpClient(Options options) : options_(options) {}

core::Result<AgentHttpResponse> AgentHttpClient::request(const std::string& method,
                                                        const std::string& host,
                                                        std::uint16_t port, const std::string& path,
                                                        const std::string& body) const {
    auto connected = connect_to(host, port, options_.connect_timeout);
    if (!connected) return connected.error();
    const int fd = connected.value();

    std::string out = method + " " + path + " HTTP/1.1\r\n";
    out += "Host: " + host + "\r\n";
    out += "Connection: close\r\n";
    if (!body.empty()) {
        out += "Content-Type: application/json\r\n";
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    out += "\r\n";
    out += body;

    const auto deadline = std::chrono::steady_clock::now() + options_.request_timeout;
    std::size_t sent_offset = 0;
    while (sent_offset < out.size()) {
        pollfd item{fd, POLLOUT, 0};
        if (::poll(&item, 1, 100) <= 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ::close(fd);
                return network_error(core::ErrorCode::ConnectionTimedOut, "agent request timed out sending");
            }
            continue;
        }
        const auto sent = ::send(fd, out.data() + sent_offset, out.size() - sent_offset, 0);
        if (sent > 0) {
            sent_offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        ::close(fd);
        return network_error(core::ErrorCode::ConnectionReset,
                             "agent send failed: " + std::string(std::strerror(errno)));
    }

    std::string in;
    char buffer[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{fd, POLLIN, 0};
        const int polled = ::poll(&item, 1, 100);
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) continue;
        const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            in.append(buffer, static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0) break; // peer closed: response complete (Connection: close)
        if (errno == EINTR) continue;
        ::close(fd);
        return network_error(core::ErrorCode::ConnectionReset,
                             "agent receive failed: " + std::string(std::strerror(errno)));
    }
    ::close(fd);

    const auto header_end = in.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return network_error(core::ErrorCode::ConnectionReset, "agent response had no header terminator");
    }
    const auto status_line_end = in.find("\r\n");
    if (status_line_end == std::string::npos || status_line_end < 12) {
        return network_error(core::ErrorCode::ConnectionReset, "agent response had a malformed status line");
    }
    AgentHttpResponse response;
    try {
        response.status = std::stoi(in.substr(9, 3));
    } catch (const std::exception&) {
        return network_error(core::ErrorCode::ConnectionReset, "agent response status code was not numeric");
    }
    response.body = in.substr(header_end + 4);
    return response;
}

core::Result<AgentHttpResponse> AgentHttpClient::post(const std::string& host, std::uint16_t port,
                                                      const std::string& path,
                                                      const std::string& json_body) const {
    return request("POST", host, port, path, json_body);
}

core::Result<AgentHttpResponse> AgentHttpClient::del(const std::string& host, std::uint16_t port,
                                                     const std::string& path) const {
    return request("DELETE", host, port, path, "");
}

core::Result<AgentHttpResponse> AgentHttpClient::get(const std::string& host, std::uint16_t port,
                                                     const std::string& path) const {
    return request("GET", host, port, path, "");
}

} // namespace rtmp_server::dispatch
