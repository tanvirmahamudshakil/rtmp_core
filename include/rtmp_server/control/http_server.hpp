#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rtmp_server::control {

// A parsed HTTP/1.1 request. Deliberately minimal: no chunked
// transfer-encoding, no keep-alive, no multipart — every management-API
// request in this project's endpoint list is a small JSON GET/POST/PATCH,
// and every one of those needs is met by this shape (see
// docs/management-api.md "Known limitations" for what's out of scope).
struct HttpRequest {
    std::string method;
    std::string path;              // decoded path only, no query string
    std::string query;              // raw query string, no leading '?'
    std::unordered_map<std::string, std::string> headers; // lower-cased keys
    std::string body;
    std::string client_ip;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::unordered_map<std::string, std::string> headers;

    static HttpResponse json(int status, std::string body_json) {
        HttpResponse r;
        r.status = status;
        r.body = std::move(body_json);
        return r;
    }
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// Bounds every value here is a resource controlled by a remote client
// (docs/v2_promot.md section 3.5): connection backlog, header/body size,
// and the number of requests allowed to be queued waiting for a worker
// thread. A client that would exceed these gets a fast, bounded rejection
// (a closed connection or a 4xx/503) instead of unbounded memory growth or
// an unbounded thread count.
struct HttpServerOptions {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0; // 0 lets the OS choose an ephemeral port (tests)
    int listen_backlog = 64;
    std::size_t worker_threads = 4;
    std::size_t max_pending_requests = 256; // bounded queue between accept and workers
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_body_bytes = 64 * 1024;
};

// Minimal, bounded, single-threaded-accept + fixed-worker-pool HTTP/1.1
// server over plain POSIX sockets (docs/management-api.md "Why hand-rolled
// instead of a vendored library" — no HTTP library was already vendored in
// this repository and none is fetched via CMake FetchContent here, to keep
// the management API's dependency footprint identical to the rest of this
// project: system libraries only, same posture as SQLite3/OpenSSL).
//
// Runs entirely on its own accept thread + a small, fixed pool of worker
// threads — never on an RTMP/io_uring event-loop thread — so a slow
// handler (e.g. one that ends up doing a database lookup) can never block
// media processing (docs/v2_promot.md section 3.6). The queue between
// accept and workers is bounded: once full, new connections are closed
// immediately rather than queued without limit (section 3.5).
class HttpServer {
public:
    explicit HttpServer(HttpServerOptions options);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void set_handler(HttpHandler handler) { handler_ = std::move(handler); }

    // Binds and starts the accept + worker threads. Returns false (and
    // leaves the server not listening) if bind()/listen() fails.
    [[nodiscard]] bool start();
    void stop();

    // The actual bound port — meaningful after start() when
    // HttpServerOptions::port == 0 (ephemeral port, used by tests).
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void accept_loop();
    void worker_loop();
    void handle_connection(int client_fd, std::string client_ip);

    HttpServerOptions options_;
    HttpHandler handler_;

    int listen_fd_ = -1;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> running_{false};

    std::thread accept_thread_;
    std::vector<std::thread> worker_threads_;

    struct PendingConnection {
        int fd;
        std::string client_ip;
    };
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<PendingConnection> pending_;
    bool shutting_down_ = false;
};

} // namespace rtmp_server::control
