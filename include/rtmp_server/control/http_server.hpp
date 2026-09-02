#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/buffer.hpp"

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
    std::string http_version;       // "HTTP/1.1" or "HTTP/1.0", as sent
    std::unordered_map<std::string, std::string> headers; // lower-cased keys
    std::string body;
    std::string client_ip;
};

class DeferredResponse;
using DeferredResponsePtr = std::shared_ptr<DeferredResponse>;

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    // Immutable media payload shared by every response for the same segment.
    // Offset/length allow byte ranges without copying into a new string.
    core::SharedBuffer shared_body;
    std::size_t shared_body_offset = 0;
    std::size_t shared_body_length = 0;
    std::unordered_map<std::string, std::string> headers;

    // Set when the handler cannot answer yet and the server must hold the
    // request open (Low-Latency HLS blocking playlist reload). Every other
    // field is ignored while this is set. Only AsyncHttpServer honours it;
    // the blocking HttpServer answers such a request with 503 rather than
    // pinning one of its finite worker threads on a wait.
    DeferredResponsePtr deferred;

    [[nodiscard]] std::size_t payload_size() const noexcept {
        return shared_body.empty() ? body.size() : shared_body_length;
    }

    static HttpResponse json(int status, std::string body_json) {
        HttpResponse r;
        r.status = status;
        r.body = std::move(body_json);
        return r;
    }
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// A response the handler has deliberately postponed.
//
// Low-Latency HLS requires the origin to hold a playlist request open until
// the live edge reaches the position the player asked for (RFC 8216bis
// 6.2.5.2). Blocking inside the handler is not an option: AsyncHttpServer
// runs handlers on its event-loop threads, so one wait would stall every
// other connection that loop is carrying. Instead the handler returns a
// deferred response, the loop parks the connection, and whoever produces the
// media resolves it from another thread.
//
// Both halves are racy by nature -- the media may arrive before the server
// has finished parking the connection -- so this holds whichever came first
// and delivers exactly once, from whichever thread completes the pair.
class DeferredResponse {
public:
    using Completion = std::function<void(HttpResponse)>;

    // Answer the server sends if this deferral is still unresolved when its
    // budget runs out. Set by the handler before it returns; read by the
    // server only after cancel() has guaranteed no completion will fire, so
    // it needs no synchronisation of its own.
    HttpResponse timeout_response;
    // How long the server may hold the request. 0 uses the server's own
    // request timeout. Low-Latency HLS wants a bound near three target
    // durations, well under a general-purpose request timeout.
    std::chrono::milliseconds timeout{0};

    // Installs the sink that delivers the response. Called by the server.
    // If resolve() already ran, the completion is invoked inline, on the
    // caller's thread.
    void attach(Completion completion) {
        HttpResponse ready;
        {
            std::lock_guard lock(mutex_);
            if (delivered_) return;
            if (!resolved_) {
                completion_ = std::move(completion);
                return;
            }
            delivered_ = true;
            ready = std::move(response_);
        }
        completion(std::move(ready));
    }

    // Supplies the response. The first call wins; later ones are ignored, so
    // a timeout path and a media-arrival path can both fire without either
    // having to know about the other.
    void resolve(HttpResponse response) {
        Completion completion;
        {
            std::lock_guard lock(mutex_);
            if (resolved_ || delivered_) return;
            resolved_ = true;
            if (!completion_) {
                response_ = std::move(response);
                return;
            }
            delivered_ = true;
            completion = std::move(completion_);
        }
        completion(std::move(response));
    }

    // Abandons the deferral: no completion will fire and any later resolve()
    // is a no-op. Returns false when a response was already delivered, i.e.
    // the caller must not send one of its own. This is how the server takes
    // the request back when its budget expires, without racing the producer
    // that may be resolving it at the same moment.
    bool cancel() {
        std::lock_guard lock(mutex_);
        if (delivered_) return false;
        delivered_ = true;
        completion_ = {};
        return true;
    }

    // True once a response has been supplied, whether or not it has been
    // written yet. Lets a waiter registry drop entries it no longer owns.
    [[nodiscard]] bool resolved() const {
        std::lock_guard lock(mutex_);
        return resolved_;
    }

private:
    mutable std::mutex mutex_;
    Completion completion_;
    HttpResponse response_;
    bool resolved_ = false;
    bool delivered_ = false;
};

// Bounds every value here is a resource controlled by a remote client
// (docs/v2_promot.md section 3.5): connection backlog, header/body size,
// and the number of requests allowed to be queued waiting for a worker
// thread. A client that would exceed these gets a fast, bounded rejection
// (a 4xx/503 response) instead of unbounded memory growth or
// an unbounded thread count.
struct HttpServerOptions {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0; // 0 lets the OS choose an ephemeral port (tests)
    // Unbounded by request: the kernel still clamps this to net.core.somaxconn
    // regardless of what we pass, so INT_MAX just asks for the OS ceiling.
    int listen_backlog = std::numeric_limits<int>::max();
    // Threads can't actually be unlimited -- spawning an unbounded number of
    // OS threads at startup hangs/crashes the process before it serves a
    // single request. hardware_concurrency() is the practical ceiling (use
    // every core); falls back to 4 if the platform can't report it.
    std::size_t worker_threads = std::thread::hardware_concurrency() > 0
                                      ? std::thread::hardware_concurrency()
                                      : 4;
    // The following are intentionally unbounded per explicit request. This
    // removes the DoS protection described above: a slow or malicious client
    // can now grow the pending queue, header buffer, or body buffer without
    // limit, exhausting server memory. Re-bound these if this is exposed to
    // untrusted clients.
    std::size_t max_pending_requests = std::numeric_limits<std::size_t>::max();
    std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t max_body_bytes = std::numeric_limits<std::size_t>::max();

    // Off by default: every existing caller (management API, tests) expects
    // one request per connection and detects response-end via EOF. Set true
    // for a route serving frequent, repeated small fetches from the same
    // peer (e.g. HLS playlists/segments) to skip the accept+handshake cost
    // per request. A worker thread is held by the connection while it waits
    // for the next request, so this trades some idle-thread cost for far
    // fewer connection setups under high request rates.
    bool enable_keep_alive = false;
    std::chrono::milliseconds keep_alive_idle_timeout{15'000};
    std::size_t max_requests_per_connection = 1000; // bound a single client's thread hold time
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
// accept and workers is bounded: once full, new connections receive 503
// immediately rather than being queued without limit (section 3.5).
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

    // Atomic because stop() (caller's thread) and accept_loop() (accept
    // thread) both touch it. Phase 8: TSan reported this as a genuine data
    // race, and it was worse than a formality — stop() closed the descriptor
    // and set -1 while the accept thread could still be entering accept() on
    // the old value, so a descriptor number reused by any other thread in the
    // process could have been accepted on. stop() now joins the accept thread
    // before closing, which removes the reuse window entirely; the atomic
    // covers the remaining ordinary visibility requirement.
    std::atomic<int> listen_fd_{-1};
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
