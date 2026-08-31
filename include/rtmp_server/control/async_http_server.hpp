#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/control/http_server.hpp"

namespace rtmp_server::control {

// Bounds and sizing for AsyncHttpServer. Deliberately a separate type from
// HttpServerOptions: `worker_threads` there means "how many requests can be
// in flight at once", which is exactly the coupling this server removes, so
// reusing the name would invite sizing it the same way.
struct AsyncHttpServerOptions {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0; // 0 lets the OS choose an ephemeral port (tests)
    int listen_backlog = std::numeric_limits<int>::max();

    // Event loops, each one thread. Connections are spread across these and
    // are NOT bound to one each -- a loop multiplexes as many as the fd limit
    // allows. Sizing this above the core count buys nothing: a loop is
    // CPU-bound on its own thread, never blocked on a socket.
    std::size_t event_loops = std::thread::hardware_concurrency() > 0
                                  ? std::thread::hardware_concurrency()
                                  : 4;

    // Hard ceiling on simultaneously open connections across all loops.
    // 0 means "bounded only by the process fd limit". Reaching it produces an
    // immediate 503 rather than an accept() that later fails with EMFILE,
    // which would degrade every connection instead of refusing one.
    std::size_t max_connections = 0;

    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 128 * 1024;

    // Unlike the blocking server, keep-alive here costs no thread: an idle
    // connection is an entry in an interest set, so it is on by default.
    bool enable_keep_alive = true;
    // How long an idle keep-alive connection may sit between requests.
    std::chrono::milliseconds keep_alive_idle_timeout{15'000};
    // How long a single request may take from first byte to fully written
    // response. Bounds a slowloris peer that dribbles headers, and a peer
    // that stops reading mid-response.
    std::chrono::milliseconds request_timeout{30'000};
    std::size_t max_requests_per_connection = 1000;
};

// Event-driven HTTP/1.1 server: connection count is decoupled from thread
// count.
//
// Why this exists alongside HttpServer: HttpServer assigns one blocking OS
// thread per connection for that connection's whole life, so the number of
// viewers it can serve at once is the size of its thread pool, no matter how
// idle the machine is. Raising that pool trades the ceiling for memory (a
// default 8 MiB stack each) and scheduler pressure, and never removes it.
// Here each loop thread multiplexes an unbounded number of connections
// through a readiness poll, so capacity is set by file descriptors and
// bandwidth -- the resources actually being spent.
//
// Threading contract for handlers: the handler runs ON a loop thread, so a
// handler that blocks stalls every other connection that loop is carrying.
// Handlers must be non-blocking and safe to call concurrently from every
// loop (HlsHttpHandler is; see its atomic counters and sharded session
// tracker). This is stricter than HttpServer's contract, where a slow
// handler only consumed its own worker.
//
// The handler contract itself (HttpRequest in, HttpResponse out) is
// unchanged, so this is a drop-in replacement for HttpServer at the call
// site.
class AsyncHttpServer {
public:
    explicit AsyncHttpServer(AsyncHttpServerOptions options);
    ~AsyncHttpServer();
    AsyncHttpServer(const AsyncHttpServer&) = delete;
    AsyncHttpServer& operator=(const AsyncHttpServer&) = delete;

    // Must be called before start(); the handler is read from every loop
    // thread thereafter and must outlive the server.
    void set_handler(HttpHandler handler) { handler_ = std::move(handler); }

    [[nodiscard]] bool start();
    void stop();

    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    struct Stats {
        std::uint64_t accepted = 0;
        std::uint64_t rejected_over_limit = 0;
        std::uint64_t requests = 0;
        std::uint64_t timeouts = 0;
        std::uint64_t active_connections = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    class EventLoop;

    AsyncHttpServerOptions options_;
    HttpHandler handler_;

    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<std::thread> loop_threads_;

    std::atomic<bool> running_{false};
    std::uint16_t bound_port_ = 0;

    // True when every loop got its own SO_REUSEPORT listener, so each keeps
    // what it accepts. False on the fallback path, where one loop accepts for
    // all and round-robins descriptors to its peers via next_loop_.
    bool shard_accepts_ = false;
    std::atomic<std::size_t> next_loop_{0};

    // Shared across loops so max_connections is a whole-server bound, not a
    // per-loop one.
    std::atomic<std::size_t> active_connections_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_over_limit_{0};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> timeouts_{0};
};

} // namespace rtmp_server::control
