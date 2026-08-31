#include "rtmp_server/control/async_http_server.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <poll.h>
#endif

#include <algorithm>
#include <utility>

#include "rtmp_server/control/http_message.hpp"

namespace rtmp_server::control {

namespace {

// How often each loop scans its connections for expired deadlines. Coarse on
// purpose: a timeout is a safety bound, not a scheduling primitive, and a
// per-connection timer would cost far more than one linear scan a second.
constexpr std::chrono::milliseconds kTimeoutSweepInterval{500};

// Suppresses SIGPIPE on a send to a peer that has gone away. Linux offers it
// as a send flag; BSD/Darwin have no such flag and use the SO_NOSIGPIPE socket
// option instead, applied per connection in add_connection().
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Applies the per-socket half of the SIGPIPE guard where the platform needs
// it. No-op on Linux, where kSendFlags already covers every send.
void suppress_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Readiness interests. Kept as our own flags so the epoll and poll backends
// present one interface to the loop.
enum Interest : unsigned { kReadable = 1u << 0, kWritable = 1u << 1 };

struct ReadyEvent {
    int fd = -1;
    unsigned interest = 0;
    bool hangup = false;
};

#if defined(__linux__)

// epoll: O(1) in the number of registered descriptors, which is what makes an
// idle keep-alive connection genuinely free here.
class Poller {
public:
    Poller() = default;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    ~Poller() {
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
    }

    bool open() {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        return epoll_fd_ >= 0;
    }

    bool add(int fd, unsigned interest) { return ctl(EPOLL_CTL_ADD, fd, interest); }
    bool modify(int fd, unsigned interest) { return ctl(EPOLL_CTL_MOD, fd, interest); }
    void remove(int fd) { ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr); }

    // Fills `out` with ready events, empty on timeout. Returns false only on
    // an unrecoverable epoll_wait failure.
    bool wait(std::chrono::milliseconds timeout, std::vector<ReadyEvent>& out) {
        out.clear();
        events_.resize(256);
        const int n = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()),
                                   static_cast<int>(timeout.count()));
        if (n < 0) return errno == EINTR;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto& raw = events_[static_cast<std::size_t>(i)];
            ReadyEvent ready;
            ready.fd = raw.data.fd;
            if (raw.events & EPOLLIN) ready.interest |= kReadable;
            if (raw.events & EPOLLOUT) ready.interest |= kWritable;
            // EPOLLERR/EPOLLHUP can arrive without EPOLLIN/EPOLLOUT, so they
            // must be surfaced explicitly or the connection would stay
            // registered but never progress.
            ready.hangup = (raw.events & (EPOLLERR | EPOLLHUP)) != 0;
            out.push_back(ready);
        }
        return true;
    }

private:
    bool ctl(int op, int fd, unsigned interest) {
        epoll_event event{};
        event.data.fd = fd;
        event.events = 0;
        if (interest & kReadable) event.events |= EPOLLIN;
        if (interest & kWritable) event.events |= EPOLLOUT;
        return ::epoll_ctl(epoll_fd_, op, fd, &event) == 0;
    }

    int epoll_fd_ = -1;
    std::vector<epoll_event> events_;
};

#else

// Portable fallback. O(n) per wait, which is adequate for the connection
// counts non-Linux (developer and test) builds ever see; production is Linux.
class Poller {
public:
    Poller() = default;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    bool open() { return true; }

    bool add(int fd, unsigned interest) {
        index_[fd] = fds_.size();
        fds_.push_back(pollfd{fd, to_events(interest), 0});
        return true;
    }

    bool modify(int fd, unsigned interest) {
        const auto it = index_.find(fd);
        if (it == index_.end()) return false;
        fds_[it->second].events = to_events(interest);
        return true;
    }

    void remove(int fd) {
        const auto it = index_.find(fd);
        if (it == index_.end()) return;
        const std::size_t slot = it->second;
        const std::size_t last = fds_.size() - 1;
        if (slot != last) {
            fds_[slot] = fds_[last];
            index_[fds_[slot].fd] = slot;
        }
        fds_.pop_back();
        index_.erase(it);
    }

    bool wait(std::chrono::milliseconds timeout, std::vector<ReadyEvent>& out) {
        out.clear();
        if (fds_.empty()) return true;
        const int n = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()),
                             static_cast<int>(timeout.count()));
        if (n < 0) return errno == EINTR;
        if (n == 0) return true;
        for (const auto& entry : fds_) {
            if (entry.revents == 0) continue;
            ReadyEvent ready;
            ready.fd = entry.fd;
            if (entry.revents & POLLIN) ready.interest |= kReadable;
            if (entry.revents & POLLOUT) ready.interest |= kWritable;
            ready.hangup = (entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            out.push_back(ready);
        }
        return true;
    }

private:
    static short to_events(unsigned interest) {
        short events = 0;
        if (interest & kReadable) events |= POLLIN;
        if (interest & kWritable) events |= POLLOUT;
        return events;
    }

    std::vector<pollfd> fds_;
    std::unordered_map<int, std::size_t> index_;
};

#endif

// Creates a bound, listening, non-blocking socket. `reuseport` asks the kernel
// to load-balance accepts across every socket bound to the same address, which
// is what lets each loop accept independently with no shared lock. Returns -1
// on failure.
int make_listener(const std::string& address, std::uint16_t port, int backlog, bool reuseport,
                  std::uint16_t& bound_port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#if defined(SO_REUSEPORT)
    if (reuseport && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
        ::close(fd);
        return -1;
    }
#else
    if (reuseport) {
        ::close(fd);
        return -1;
    }
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1 ||
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, backlog) != 0 || !set_nonblocking(fd)) {
        ::close(fd);
        return -1;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port = ntohs(bound.sin_port);
    }
    return fd;
}

} // namespace

// ---------------------------------------------------------------------------
// EventLoop
// ---------------------------------------------------------------------------

class AsyncHttpServer::EventLoop {
public:
    EventLoop(AsyncHttpServer& server, std::size_t id) : server_(server), id_(id) {}

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    ~EventLoop() {
        for (const auto& [fd, connection] : connections_) ::close(fd);
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (wakeup_read_ >= 0) ::close(wakeup_read_);
        if (wakeup_write_ >= 0) ::close(wakeup_write_);
    }

    [[nodiscard]] std::size_t id() const noexcept { return id_; }

    // Sets up the poller and the wakeup pipe. `listen_fd` is this loop's own
    // listening socket (SO_REUSEPORT sharding) or -1 when another loop owns
    // the single shared listener and hands connections over instead.
    bool prepare(int listen_fd);

    void run();
    void request_stop();

    // Takes ownership of a descriptor accepted by another loop.
    void hand_off(int fd, std::string client_ip);

private:
    enum class State { ReadingHead, ReadingBody, Writing };

    struct Connection {
        int fd = -1;
        std::string client_ip;
        State state = State::ReadingHead;

        std::string inbox;        // bytes read but not yet consumed
        HttpRequest request;
        std::size_t expected_body = 0;

        std::string out_head;     // serialised status line + headers
        std::size_t head_sent = 0;
        // Held so the segment stays alive for the whole write. This is the
        // zero-copy path: bytes go straight from the shared segment buffer to
        // the socket, never through a per-connection copy.
        core::SharedBuffer out_body;
        std::size_t body_offset = 0;
        std::size_t body_length = 0;
        std::size_t body_sent = 0;
        std::string out_inline_body; // small bodies the handler built inline

        bool keep_alive = false;
        std::size_t requests_served = 0;
        unsigned interest = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    void add_connection(int fd, std::string client_ip);
    void close_connection(int fd);
    void accept_ready();
    void drain_wakeup();
    void on_readable(Connection& connection);
    void on_writable(Connection& connection);
    // Each returns false when the connection was closed and must not be
    // touched again by the caller.
    bool consume_input(Connection& connection);
    bool dispatch(Connection& connection);
    bool begin_response(Connection& connection, HttpResponse response, bool keep_alive);
    bool fail(Connection& connection, int status);
    bool flush(Connection& connection);
    void update_interest(Connection& connection, unsigned interest);
    void sweep_timeouts();
    void touch(Connection& connection, std::chrono::milliseconds budget);

    AsyncHttpServer& server_;
    std::size_t id_;

    Poller poller_;
    int listen_fd_ = -1;
    int wakeup_read_ = -1;
    int wakeup_write_ = -1;

    std::unordered_map<int, Connection> connections_;
    std::atomic<bool> stopping_{false};

    std::mutex handoff_mutex_;
    std::deque<std::pair<int, std::string>> handoff_;

    std::chrono::steady_clock::time_point next_sweep_{};
    std::vector<ReadyEvent> ready_;
};

bool AsyncHttpServer::EventLoop::prepare(int listen_fd) {
    if (!poller_.open()) return false;

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return false;
    wakeup_read_ = pipe_fds[0];
    wakeup_write_ = pipe_fds[1];
    if (!set_nonblocking(wakeup_read_) || !set_nonblocking(wakeup_write_)) return false;
    if (!poller_.add(wakeup_read_, kReadable)) return false;

    listen_fd_ = listen_fd;
    if (listen_fd_ >= 0 && !poller_.add(listen_fd_, kReadable)) return false;
    return true;
}

void AsyncHttpServer::EventLoop::request_stop() {
    stopping_.store(true, std::memory_order_release);
    // Best-effort: a full pipe already means a wakeup is pending, so a short
    // or failed write needs no handling -- but the result must be consumed,
    // since write() is warn_unused_result.
    const char byte = 0;
    const ssize_t ignored = ::write(wakeup_write_, &byte, 1);
    static_cast<void>(ignored);
}

void AsyncHttpServer::EventLoop::hand_off(int fd, std::string client_ip) {
    {
        std::lock_guard lock(handoff_mutex_);
        handoff_.emplace_back(fd, std::move(client_ip));
    }
    const char byte = 0;
    const ssize_t ignored = ::write(wakeup_write_, &byte, 1);
    static_cast<void>(ignored);
}

void AsyncHttpServer::EventLoop::drain_wakeup() {
    // Drain every queued wakeup byte: the pipe is only a signal, and leaving
    // bytes in it would keep the loop spinning on a readiness that has
    // already been handled.
    char scratch[256];
    while (::read(wakeup_read_, scratch, sizeof(scratch)) > 0) {
    }

    std::deque<std::pair<int, std::string>> pending;
    {
        std::lock_guard lock(handoff_mutex_);
        pending.swap(handoff_);
    }
    for (auto& [fd, ip] : pending) {
        if (stopping_.load(std::memory_order_acquire)) {
            ::close(fd);
            server_.active_connections_.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }
        add_connection(fd, std::move(ip));
    }
}

void AsyncHttpServer::EventLoop::run() {
    next_sweep_ = std::chrono::steady_clock::now() + kTimeoutSweepInterval;

    while (!stopping_.load(std::memory_order_acquire)) {
        if (!poller_.wait(kTimeoutSweepInterval, ready_)) break;

        for (const ReadyEvent& event : ready_) {
            if (event.fd == wakeup_read_) {
                drain_wakeup();
                continue;
            }
            if (event.fd == listen_fd_) {
                accept_ready();
                continue;
            }

            auto it = connections_.find(event.fd);
            if (it == connections_.end()) continue; // closed earlier this batch

            if (event.hangup && (event.interest & kReadable) == 0) {
                close_connection(event.fd);
                continue;
            }
            if (event.interest & kWritable) {
                on_writable(it->second);
                // The write may have closed the connection; re-look it up
                // before handling the readable half of the same event.
                it = connections_.find(event.fd);
                if (it == connections_.end()) continue;
            }
            if (event.interest & kReadable) on_readable(it->second);
        }

        if (std::chrono::steady_clock::now() >= next_sweep_) {
            sweep_timeouts();
            next_sweep_ = std::chrono::steady_clock::now() + kTimeoutSweepInterval;
        }
    }

    // Shutdown: drain anything handed over after the stop flag was observed so
    // no descriptor leaks, then close everything this loop owns.
    drain_wakeup();
    for (const auto& [fd, connection] : connections_) {
        ::close(fd);
        server_.active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }
    connections_.clear();
}

void AsyncHttpServer::EventLoop::accept_ready() {
    // Drain the backlog: the poller reports readiness once for several queued
    // connections, and leaving them would stall them until the next unrelated
    // event on this loop.
    for (;;) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            // ECONNABORTED: the peer reset between readiness and accept, which
            // is ordinary under load. EAGAIN: backlog drained.
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return;
        }

        char ip_buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip_buf, sizeof(ip_buf));

        const std::size_t limit = server_.options_.max_connections;
        const std::size_t open =
            server_.active_connections_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (limit != 0 && open > limit) {
            server_.active_connections_.fetch_sub(1, std::memory_order_relaxed);
            server_.rejected_over_limit_.fetch_add(1, std::memory_order_relaxed);
            // Give players and proxies an explicit retryable answer rather
            // than a reset they would surface as an unrelated media error.
            // Best-effort single write: if it does not fit the socket buffer,
            // the close still conveys refusal.
            HttpResponse response = HttpResponse::json(503, R"({"error":"server_overloaded"})");
            response.headers["Cache-Control"] = "no-store";
            response.headers["Retry-After"] = "1";
            const std::string payload = serialize_response_head(response, false) + response.body;
            suppress_sigpipe(fd);
            const ssize_t sent = ::send(fd, payload.data(), payload.size(), kSendFlags);
            static_cast<void>(sent);
            ::close(fd);
            continue;
        }

        server_.accepted_.fetch_add(1, std::memory_order_relaxed);

        // With SO_REUSEPORT every loop has its own listener and keeps what it
        // accepts. Without it, one loop accepts for all and spreads the work.
        if (server_.shard_accepts_) {
            add_connection(fd, std::string(ip_buf));
        } else {
            const std::size_t target =
                server_.next_loop_.fetch_add(1, std::memory_order_relaxed) % server_.loops_.size();
            if (target == id_) {
                add_connection(fd, std::string(ip_buf));
            } else {
                server_.loops_[target]->hand_off(fd, std::string(ip_buf));
            }
        }
    }
}

void AsyncHttpServer::EventLoop::add_connection(int fd, std::string client_ip) {
    if (!set_nonblocking(fd)) {
        ::close(fd);
        server_.active_connections_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    // Responses here are small and latency-sensitive (a playlist is a few
    // hundred bytes); letting Nagle wait to coalesce them adds up to 40 ms per
    // fetch, which a player sees directly as join delay.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    suppress_sigpipe(fd);

    Connection connection;
    connection.fd = fd;
    connection.client_ip = std::move(client_ip);
    connection.interest = kReadable;
    touch(connection, server_.options_.request_timeout);

    if (!poller_.add(fd, kReadable)) {
        ::close(fd);
        server_.active_connections_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    connections_.emplace(fd, std::move(connection));
}

void AsyncHttpServer::EventLoop::close_connection(int fd) {
    const auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    poller_.remove(fd);
    ::close(fd);
    connections_.erase(it);
    server_.active_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void AsyncHttpServer::EventLoop::touch(Connection& connection, std::chrono::milliseconds budget) {
    connection.deadline = std::chrono::steady_clock::now() + budget;
}

void AsyncHttpServer::EventLoop::update_interest(Connection& connection, unsigned interest) {
    if (connection.interest == interest) return;
    connection.interest = interest;
    poller_.modify(connection.fd, interest);
}

void AsyncHttpServer::EventLoop::on_readable(Connection& connection) {
    // A connection mid-response has nothing to read yet: any pipelined bytes
    // stay in the socket buffer until the current response is out.
    if (connection.state == State::Writing) return;

    const int fd = connection.fd;
    char chunk[16 * 1024];
    for (;;) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            connection.inbox.append(chunk, static_cast<std::size_t>(n));
            if (!consume_input(connection)) return; // closed
            // consume_input may have started a response; let the write drain
            // before reading more.
            if (connection.state == State::Writing) return;
            continue;
        }
        if (n == 0) { // orderly peer shutdown
            close_connection(fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return; // drained
        close_connection(fd);
        return;
    }
}

bool AsyncHttpServer::EventLoop::consume_input(Connection& connection) {
    const auto& options = server_.options_;

    for (;;) {
        if (connection.state == State::ReadingHead) {
            const std::size_t marker = connection.inbox.find("\r\n\r\n");
            if (marker == std::string::npos) {
                // Bound a slowloris peer that never completes a header block.
                if (connection.inbox.size() >= options.max_header_bytes) return fail(connection, 431);
                return true; // need more bytes
            }
            if (marker > options.max_header_bytes) return fail(connection, 431);

            connection.request = HttpRequest{};
            connection.request.client_ip = connection.client_ip;
            if (!parse_request_head(std::string_view(connection.inbox).substr(0, marker),
                                    connection.request)) {
                return fail(connection, 400);
            }

            bool length_valid = true;
            connection.expected_body = content_length_of(connection.request, length_valid);
            if (!length_valid) return fail(connection, 400);
            if (connection.expected_body > options.max_body_bytes) return fail(connection, 413);

            connection.inbox.erase(0, marker + 4);
            connection.state = State::ReadingBody;
        }

        if (connection.state == State::ReadingBody) {
            if (connection.inbox.size() < connection.expected_body) return true; // need more
            connection.request.body = connection.inbox.substr(0, connection.expected_body);
            // Anything past this request's body begins the next pipelined
            // request; keeping it is what makes pipelining work, and dropping
            // it would corrupt the following request.
            connection.inbox.erase(0, connection.expected_body);
            if (!dispatch(connection)) return false;
            // dispatch -> flush may have completed the whole response
            // synchronously and reset us to ReadingHead. If there are more
            // buffered bytes, parse the next request without another syscall.
            if (connection.state == State::ReadingHead && !connection.inbox.empty()) continue;
            return true;
        }

        return true;
    }
}

bool AsyncHttpServer::EventLoop::dispatch(Connection& connection) {
    const auto& options = server_.options_;
    HttpRequest& request = connection.request;

    // Caddy and Varnish reach this listener over loopback and supply the
    // original address in X-Forwarded-For. Trust that header only from
    // loopback; accepting it from a public peer would let an attacker bypass
    // per-IP authentication throttles. Validate the first hop as an IP literal
    // so arbitrary header text never becomes a rate-limit map key.
    if (connection.client_ip == "127.0.0.1") {
        const auto forwarded = request.headers.find("x-forwarded-for");
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
    if (server_.handler_) {
        response = server_.handler_(request);
    } else {
        response.status = 503;
        response.body = R"({"error":"no_handler"})";
    }
    server_.requests_.fetch_add(1, std::memory_order_relaxed);

    // HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close unless the
    // client opts in. Either side can veto with "Connection: close".
    const auto connection_header = request.headers.find("connection");
    const bool client_wants_close =
        connection_header != request.headers.end() && connection_header->second == "close";
    const bool client_wants_keep_alive =
        connection_header != request.headers.end() && connection_header->second == "keep-alive";
    const bool http_1_1 = request.http_version == "HTTP/1.1";
    const bool keep_alive = options.enable_keep_alive && !client_wants_close &&
                            (http_1_1 || client_wants_keep_alive) &&
                            connection.requests_served + 1 < options.max_requests_per_connection;

    ++connection.requests_served;
    return begin_response(connection, std::move(response), keep_alive);
}

bool AsyncHttpServer::EventLoop::begin_response(Connection& connection, HttpResponse response,
                                                bool keep_alive) {
    connection.keep_alive = keep_alive;
    connection.out_head = serialize_response_head(response, keep_alive);
    connection.head_sent = 0;
    connection.body_sent = 0;

    if (!response.shared_body.empty()) {
        const auto view = response.shared_body.view();
        // A handler reporting a slice outside its own buffer is a bug, not a
        // client error: send nothing rather than read past the segment.
        if (response.shared_body_offset > view.size() ||
            response.shared_body_length > view.size() - response.shared_body_offset) {
            close_connection(connection.fd);
            return false;
        }
        connection.out_body = std::move(response.shared_body);
        connection.body_offset = response.shared_body_offset;
        connection.body_length = response.shared_body_length;
        connection.out_inline_body.clear();
    } else {
        connection.out_body = {};
        connection.body_offset = 0;
        connection.body_length = response.body.size();
        connection.out_inline_body = std::move(response.body);
    }

    connection.state = State::Writing;
    touch(connection, server_.options_.request_timeout);
    return flush(connection);
}

bool AsyncHttpServer::EventLoop::fail(Connection& connection, int status) {
    HttpResponse response;
    response.status = status;
    response.body = R"({"error":"bad_request"})";
    // Do not keep a connection open past a malformed or oversized request: its
    // framing is already untrustworthy, so anything after it is too.
    return begin_response(connection, std::move(response), false);
}

bool AsyncHttpServer::EventLoop::flush(Connection& connection) {
    const std::byte* body_bytes = nullptr;
    if (!connection.out_body.empty()) {
        body_bytes = connection.out_body.view().data() + connection.body_offset;
    } else if (!connection.out_inline_body.empty()) {
        body_bytes = reinterpret_cast<const std::byte*>(connection.out_inline_body.data());
    }

    for (;;) {
        iovec vectors[2];
        int count = 0;
        if (connection.head_sent < connection.out_head.size()) {
            vectors[count].iov_base =
                const_cast<char*>(connection.out_head.data() + connection.head_sent);
            vectors[count].iov_len = connection.out_head.size() - connection.head_sent;
            ++count;
        }
        if (body_bytes != nullptr && connection.body_sent < connection.body_length) {
            vectors[count].iov_base = const_cast<std::byte*>(body_bytes + connection.body_sent);
            vectors[count].iov_len = connection.body_length - connection.body_sent;
            ++count;
        }
        if (count == 0) break; // fully written

        // sendmsg rather than writev so MSG_NOSIGNAL is available: a viewer
        // that vanishes mid-segment would otherwise raise SIGPIPE and, with
        // the default disposition, kill the process. apps/rtmp_server/main.cpp
        // ignores SIGPIPE globally, but this server must not depend on its
        // host having remembered to -- the same reasoning as the per-socket
        // guards in apps/rtmp_test_server and src/loadgen.
        msghdr message{};
        message.msg_iov = vectors;
        message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(count);
        const ssize_t written = ::sendmsg(connection.fd, &message, kSendFlags);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full. Wait for writability rather than block
                // this loop on one slow reader -- this is exactly the case
                // that pinned an entire thread in the blocking server.
                update_interest(connection, kWritable);
                return true;
            }
            close_connection(connection.fd);
            return false;
        }

        auto remaining = static_cast<std::size_t>(written);
        const std::size_t head_left = connection.out_head.size() - connection.head_sent;
        const std::size_t head_step = std::min(remaining, head_left);
        connection.head_sent += head_step;
        remaining -= head_step;
        connection.body_sent += remaining;
    }

    if (!connection.keep_alive) {
        close_connection(connection.fd);
        return false;
    }

    // Reset for the next request. Bytes already read past this request stay in
    // `inbox` and the caller parses them without another syscall.
    connection.state = State::ReadingHead;
    connection.out_head.clear();
    connection.out_inline_body.clear();
    connection.out_body = {};
    connection.body_offset = 0;
    connection.body_length = 0;
    connection.head_sent = 0;
    connection.body_sent = 0;
    update_interest(connection, kReadable);
    touch(connection, server_.options_.keep_alive_idle_timeout);
    return true;
}

void AsyncHttpServer::EventLoop::on_writable(Connection& connection) {
    if (connection.state != State::Writing) return;
    const int fd = connection.fd;
    if (!flush(connection)) return; // closed

    // The response finished on this writable event and more pipelined bytes
    // were already buffered: serve them now instead of waiting for the peer to
    // send something new (it may be waiting on us).
    const auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    if (it->second.state == State::ReadingHead && !it->second.inbox.empty()) {
        consume_input(it->second);
    }
}

void AsyncHttpServer::EventLoop::sweep_timeouts() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expired;
    for (const auto& [fd, connection] : connections_) {
        if (connection.deadline <= now) expired.push_back(fd);
    }
    for (const int fd : expired) {
        server_.timeouts_.fetch_add(1, std::memory_order_relaxed);
        close_connection(fd);
    }
}

// ---------------------------------------------------------------------------
// AsyncHttpServer
// ---------------------------------------------------------------------------

AsyncHttpServer::AsyncHttpServer(AsyncHttpServerOptions options) : options_(std::move(options)) {
    if (options_.event_loops == 0) options_.event_loops = 1;
}

AsyncHttpServer::~AsyncHttpServer() { stop(); }

bool AsyncHttpServer::start() {
    if (running_.load(std::memory_order_acquire)) return false;

    // Every loop gets its own listener when SO_REUSEPORT works, so accepts
    // scale with loop count instead of funnelling through one queue. Where it
    // does not, loop 0 owns the only listener and spreads accepted
    // descriptors across its peers.
#if defined(SO_REUSEPORT) && defined(__linux__)
    shard_accepts_ = true;
#else
    shard_accepts_ = false;
#endif

    std::vector<int> listeners;
    listeners.reserve(options_.event_loops);

    // Bind the first socket to learn the concrete port: with port 0 each
    // socket would otherwise land on a different ephemeral port and the group
    // would not share one.
    std::uint16_t resolved = 0;
    int first = make_listener(options_.bind_address, options_.port, options_.listen_backlog,
                              shard_accepts_, resolved);
    if (first < 0 && shard_accepts_) {
        // SO_REUSEPORT can be compiled in but refused at runtime (older
        // kernels, some sandboxes). Fall back rather than failing to start.
        shard_accepts_ = false;
        first = make_listener(options_.bind_address, options_.port, options_.listen_backlog, false,
                              resolved);
    }
    if (first < 0) return false;
    bound_port_ = resolved;
    listeners.push_back(first);

    if (shard_accepts_) {
        for (std::size_t i = 1; i < options_.event_loops; ++i) {
            std::uint16_t ignored = 0;
            const int fd = make_listener(options_.bind_address, bound_port_, options_.listen_backlog,
                                         true, ignored);
            // A later socket failing to join the group is not fatal: run with
            // the listeners already bound. Loops without one still serve every
            // connection handed to them.
            if (fd < 0) break;
            listeners.push_back(fd);
        }
    }

    for (std::size_t i = 0; i < options_.event_loops; ++i) {
        loops_.push_back(std::make_unique<EventLoop>(*this, i));
        const int listen_fd = i < listeners.size() ? listeners[i] : -1;
        if (!loops_.back()->prepare(listen_fd)) {
            // prepare() failed before taking ownership of any listener beyond
            // this one, so close the ones not yet handed to a loop.
            for (std::size_t j = i; j < listeners.size(); ++j) ::close(listeners[j]);
            loops_.clear();
            return false;
        }
    }

    running_.store(true, std::memory_order_release);
    for (auto& loop : loops_) {
        loop_threads_.emplace_back([raw = loop.get()] { raw->run(); });
    }
    return true;
}

void AsyncHttpServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    for (auto& loop : loops_) loop->request_stop();
    for (auto& thread : loop_threads_) {
        if (thread.joinable()) thread.join();
    }
    loop_threads_.clear();
    loops_.clear();
}

AsyncHttpServer::Stats AsyncHttpServer::stats() const {
    Stats snapshot;
    snapshot.accepted = accepted_.load(std::memory_order_relaxed);
    snapshot.rejected_over_limit = rejected_over_limit_.load(std::memory_order_relaxed);
    snapshot.requests = requests_.load(std::memory_order_relaxed);
    snapshot.timeouts = timeouts_.load(std::memory_order_relaxed);
    snapshot.active_connections = active_connections_.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace rtmp_server::control
