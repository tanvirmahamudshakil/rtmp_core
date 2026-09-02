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

#include "rtmp_server/control/http_message.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace {
// Shutdown-latency bound for the accept loop. Short enough that stop() feels
// immediate to an operator, long enough that an idle server is not spinning.
constexpr int kAcceptPollTimeoutMs = 100;
} // namespace

namespace rtmp_server::control {

namespace {

using rtmp_server::observability::LogLevel;


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

    // Shared with AsyncHttpServer so both servers frame requests identically:
    // two hand-rolled parsers drifting apart is how a request-smuggling
    // difference gets introduced.
    if (!parse_request_head(std::string_view(buffer).substr(0, header_end), out)) {
        status_on_failure = 400;
        return false;
    }
    std::string leftover = buffer.substr(header_end + 4);

    bool length_valid = true;
    const std::size_t content_length = content_length_of(out, length_valid);
    if (!length_valid) {
        status_on_failure = 400;
        return false;
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

bool write_response(int fd, const HttpResponse& response, bool keep_alive) {
    const std::string out = serialize_response_head(response, keep_alive);

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

    if (!send_all(out.data(), out.size())) return false;
    if (!response.shared_body.empty()) {
        const auto view = response.shared_body.view();
        if (response.shared_body_offset > view.size() ||
            response.shared_body_length > view.size() - response.shared_body_offset) {
            return false;
        }
        const auto* bytes =
            reinterpret_cast<const char*>(view.data() + response.shared_body_offset);
        return send_all(bytes, response.shared_body_length);
    }
    return send_all(response.body.data(), response.body.size());
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

        bool overloaded = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (shutting_down_) {
                ::close(client_fd);
                continue;
            }
            if (pending_.size() >= options_.max_pending_requests) {
                overloaded = true;
            } else {
                pending_.push_back(PendingConnection{client_fd, std::string(ip_buf)});
                queue_cv_.notify_one();
            }
        }
        if (overloaded) {
            // Give reverse proxies and players an explicit retryable response
            // instead of an empty reply/reset that can surface as an unrelated
            // media/decode failure. Keep this best-effort and outside the queue
            // lock so a slow peer cannot block workers from draining requests.
            HttpResponse response = HttpResponse::json(503, R"({"error":"server_overloaded"})");
            response.headers["Cache-Control"] = "no-store";
            response.headers["Retry-After"] = "1";
            write_response(client_fd, response, false);
            ::close(client_fd);
        }
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
    for (std::size_t requests_served = 0;; ++requests_served) {
        HttpRequest request;
        request.client_ip = client_ip;
        int failure_status = 400;

        if (!read_request(client_fd, options_, request, failure_status)) {
            // A keep-alive peer that simply closed/timed out between requests
            // reads as EOF here, which read_request reports as a plain
            // failure; only send a 4xx if bytes of a new request had actually
            // started arriving is not distinguishable at this layer, so this
            // matches the original single-shot behaviour (best-effort 4xx).
            if (requests_served == 0 || failure_status != 400) {
                HttpResponse response;
                response.status = failure_status;
                response.body = R"({"error":"bad_request"})";
                write_response(client_fd, response, false);
            }
            break;
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

        // This server owns one blocking thread per connection, so parking a
        // request would consume a worker for the whole wait -- the exact
        // coupling AsyncHttpServer exists to remove. Take the handler's own
        // fallback answer instead of holding the thread.
        if (response.deferred) {
            auto deferred = std::move(response.deferred);
            response = deferred->cancel() ? std::move(deferred->timeout_response) : HttpResponse{};
            response.deferred.reset();
            if (response.status == 0) response.status = 503;
        }

        // HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close unless
        // the client opts in. Either side can veto with "Connection: close".
        auto conn_it = request.headers.find("connection");
        const bool client_wants_close = conn_it != request.headers.end() && conn_it->second == "close";
        const bool client_wants_keep_alive =
            conn_it != request.headers.end() && conn_it->second == "keep-alive";
        const bool http_1_1 = request.http_version == "HTTP/1.1";
        bool keep_alive = options_.enable_keep_alive && !client_wants_close &&
                           (http_1_1 || client_wants_keep_alive) &&
                           requests_served + 1 < options_.max_requests_per_connection;

        if (!write_response(client_fd, response, keep_alive) || !keep_alive) break;

        // Bound how long this worker thread waits idle for the next request
        // on this connection: an idle keep-alive peer must not hold a worker
        // forever out of the fixed pool that serves every other connection.
        pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        const int timeout_ms = static_cast<int>(options_.keep_alive_idle_timeout.count());
        const int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready <= 0) break; // idle timeout or poll error: close
    }
    ::close(client_fd);
}

} // namespace rtmp_server::control
