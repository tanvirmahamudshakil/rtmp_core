#include "rtmp_server/control/http_server.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

#include "rtmp_server/observability/logger.hpp"

namespace {
// Shutdown-latency bound for the accept loop. Short enough that stop() feels
// immediate to an operator, long enough that an idle server is not spinning.
constexpr int kAcceptPollTimeoutMs = 100;
} // namespace

namespace rtmp_server::control {

namespace {

using rtmp_server::observability::LogLevel;

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads exactly the request line + headers (bounded by max_header_bytes),
// then, if Content-Length is present, exactly that many body bytes
// (bounded by max_body_bytes). Returns false on malformed input, a client
// that exceeds either bound, or a socket error/EOF before the full request
// arrives — all of which the caller turns into closing the connection
// (with a best-effort 4xx first where feasible), never an unbounded read.
bool read_request(int fd, const HttpServerOptions& options, HttpRequest& out, int& status_on_failure) {
    std::string buffer;
    buffer.reserve(4096);
    char chunk[4096];
    std::size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        if (buffer.size() >= options.max_header_bytes) {
            status_on_failure = 431; // Request Header Fields Too Large
            return false;
        }
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            status_on_failure = 400;
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
        header_end = buffer.find("\r\n\r\n");
    }

    std::string head = buffer.substr(0, header_end);
    std::string leftover = buffer.substr(header_end + 4);

    std::size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) {
        status_on_failure = 400;
        return false;
    }
    std::string request_line = head.substr(0, line_end);
    std::size_t sp1 = request_line.find(' ');
    std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        status_on_failure = 400;
        return false;
    }
    out.method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (auto qpos = target.find('?'); qpos != std::string::npos) {
        out.path = target.substr(0, qpos);
        out.query = target.substr(qpos + 1);
    } else {
        out.path = target;
    }

    std::size_t pos = line_end + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) next = head.size();
        std::string line = head.substr(pos, next - pos);
        if (auto colon = line.find(':'); colon != std::string::npos) {
            std::string key = to_lower(line.substr(0, colon));
            std::size_t vstart = colon + 1;
            while (vstart < line.size() && line[vstart] == ' ') ++vstart;
            out.headers[key] = line.substr(vstart);
        }
        pos = next + 2;
    }

    std::size_t content_length = 0;
    if (auto it = out.headers.find("content-length"); it != out.headers.end()) {
        content_length = static_cast<std::size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    }
    if (content_length > options.max_body_bytes) {
        status_on_failure = 413; // Payload Too Large
        return false;
    }

    out.body = std::move(leftover);
    while (out.body.size() < content_length) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            status_on_failure = 400;
            return false;
        }
        out.body.append(chunk, static_cast<std::size_t>(n));
    }
    out.body.resize(content_length);
    return true;
}

// Reason phrases for the statuses this server actually emits. HLS clients
// and CDNs key retry/caching behaviour off the status line, and a 206 or 416
// labelled "Error"/"OK" is confusing to intermediaries even though the code
// is what is normative.
const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
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
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return status < 400 ? "OK" : "Error";
    }
}

void write_response(int fd, const HttpResponse& response) {
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
    out += "Connection: close\r\n";
    for (const auto& [k, v] : response.headers) out += k + ": " + v + "\r\n";
    out += "\r\n";

    auto send_all = [fd](const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            ssize_t n = ::send(fd, data + sent, size - sent, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    };

    if (!send_all(out.data(), out.size())) return;
    if (!response.shared_body.empty()) {
        const auto view = response.shared_body.view();
        if (response.shared_body_offset > view.size() ||
            response.shared_body_length > view.size() - response.shared_body_offset) {
            return;
        }
        const auto* bytes =
            reinterpret_cast<const char*>(view.data() + response.shared_body_offset);
        static_cast<void>(send_all(bytes, response.shared_body_length));
    } else {
        static_cast<void>(send_all(response.body.data(), response.body.size()));
    }
}

} // namespace

HttpServer::HttpServer(HttpServerOptions options) : options_(std::move(options)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    // Built up in a local and published to listen_fd_ only once the socket is
    // fully bound and listening: nothing may observe a half-initialised
    // descriptor, and the accept thread does not exist until after the store.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    if (::listen(fd, options_.listen_backlog) != 0) {
        ::close(fd);
        return false;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }

    listen_fd_.store(fd, std::memory_order_release);

    running_.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < options_.worker_threads; ++i) {
        worker_threads_.emplace_back([this] { worker_loop(); });
    }
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    const int fd = listen_fd_.load(std::memory_order_acquire);

    // Join the accept thread BEFORE closing the listening descriptor. Closing
    // first (as this did before Phase 8) leaves a window in which the accept
    // thread is between its running_ check and its accept() call: the
    // descriptor number can by then have been reused by any other thread in
    // the process, and the accept would apply to an unrelated file. The
    // accept loop polls with a timeout, so clearing running_ is sufficient to
    // make it exit promptly without needing close() as the wakeup mechanism.
    if (accept_thread_.joinable()) accept_thread_.join();

    if (fd >= 0) {
        listen_fd_.store(-1, std::memory_order_release);
        ::close(fd);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutting_down_ = true;
        for (auto& conn : pending_) ::close(conn.fd);
        pending_.clear();
    }
    queue_cv_.notify_all();
    for (auto& t : worker_threads_) {
        if (t.joinable()) t.join();
    }
    worker_threads_.clear();
}

void HttpServer::accept_loop() {
    const int fd = listen_fd_.load(std::memory_order_acquire);
    if (fd < 0) return;

    while (running_.load(std::memory_order_acquire)) {
        // poll() with a bounded timeout rather than a blocking accept(). A
        // blocking accept() can only be woken by closing the descriptor out
        // from under it, which is precisely the unsynchronised close this
        // loop must avoid; polling makes running_ the single, race-free
        // shutdown signal and caps shutdown latency at the timeout.
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, kAcceptPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) continue; // signal, not an error
            break;
        }
        if (ready == 0) continue; // timeout: re-check running_

        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int client_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client_fd < 0) {
            // EAGAIN/ECONNABORTED are ordinary: the pending connection was
            // reset between poll() and accept().
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) continue;
            if (!running_.load(std::memory_order_acquire)) break;
            continue;
        }

        char ip_buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip_buf, sizeof(ip_buf));

        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutting_down_ || pending_.size() >= options_.max_pending_requests) {
            // Bounded queue (docs/v2_promot.md section 3.5 "Pending
            // management requests"): reject rather than grow unbounded.
            ::close(client_fd);
            continue;
        }
        pending_.push_back(PendingConnection{client_fd, std::string(ip_buf)});
        queue_cv_.notify_one();
    }
}

void HttpServer::worker_loop() {
    while (true) {
        PendingConnection conn{-1, {}};
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return shutting_down_ || !pending_.empty(); });
            if (shutting_down_ && pending_.empty()) return;
            if (pending_.empty()) continue;
            conn = pending_.front();
            pending_.pop_front();
        }
        handle_connection(conn.fd, conn.client_ip);
    }
}

void HttpServer::handle_connection(int client_fd, std::string client_ip) {
    HttpRequest request;
    request.client_ip = client_ip;
    int failure_status = 400;

    if (!read_request(client_fd, options_, request, failure_status)) {
        HttpResponse response;
        response.status = failure_status;
        response.body = R"({"error":"bad_request"})";
        write_response(client_fd, response);
        ::close(client_fd);
        return;
    }

    // Caddy reaches this listener over loopback and supplies the original
    // address in X-Forwarded-For. Trust that header only from loopback;
    // accepting it from a public peer would let an attacker bypass per-IP
    // authentication throttles. Validate the first hop as an IP literal so
    // arbitrary header text never becomes a rate-limit map key.
    if (client_ip == "127.0.0.1") {
        auto forwarded = request.headers.find("x-forwarded-for");
        if (forwarded != request.headers.end()) {
            std::string candidate = forwarded->second.substr(0, forwarded->second.find(','));
            while (!candidate.empty() && candidate.front() == ' ') candidate.erase(candidate.begin());
            while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
            in_addr ipv4{};
            in6_addr ipv6{};
            if (::inet_pton(AF_INET, candidate.c_str(), &ipv4) == 1 ||
                ::inet_pton(AF_INET6, candidate.c_str(), &ipv6) == 1) {
                request.client_ip = std::move(candidate);
            }
        }
    }

    HttpResponse response;
    if (handler_) {
        response = handler_(request);
    } else {
        response.status = 503;
        response.body = R"({"error":"no_handler"})";
    }
    write_response(client_fd, response);
    ::close(client_fd);
}

} // namespace rtmp_server::control
