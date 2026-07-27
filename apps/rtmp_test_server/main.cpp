// rtmp_test_server — a TEST-ONLY, POSIX-socket RTMP server front-end.
//
// WHY THIS EXISTS, AND WHAT IT IS NOT
// ===================================
// The production RTMP transport is io::io_uring::IoUringEventLoop
// (apps/rtmp_server), which requires Linux io_uring and cannot be built on
// this macOS development host — every phase report since Phase 0 records
// this, and CMakeLists.txt guards those targets with
// `NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux"`.
//
// Phase 7 requires real, socket-level load measurements. Without a runnable
// server binary on this host there would be nothing for the Phase 7 load
// generator to connect to, and the only honest options would be to report no
// measurements at all, or to fabricate them.
//
// This binary is the third option: it runs the SAME production protocol and
// fan-out stack the real server runs —
//     HandshakeSession -> RtmpConnectionSession -> ChunkDecoder/Encoder ->
//     CommandSession -> StreamRegistry -> LiveFanout (GOP cache, ViewerQueue)
// — behind a plain non-blocking poll(2) loop instead of io_uring. It is a
// substitute for exactly one layer: the event loop and its socket
// submission/completion machinery.
//
// It is therefore NOT a production server and must never be deployed:
//   * No io_uring, no provided buffer rings, no multishot accept/recv.
//   * One worker thread; no SO_REUSEPORT sharding, no CrossWorkerRouter.
//   * No authentication, no persistence, no recording, no HLS, no TLS.
//   * No management API.
// Numbers measured against it characterise the PROTOCOL, FAN-OUT and
// MEMORY behaviour of this codebase, and the syscall cost of a poll-based
// loop. They do NOT characterise the io_uring transport's throughput,
// syscall efficiency, or multi-core scaling. docs/capacity-report.md states
// this distinction for every row it reports.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/protocol/session/rtmp_connection_session.hpp"

namespace {

using rtmp_server::core::SharedBuffer;
using rtmp_server::observability::MetricId;
using rtmp_server::observability::Metrics;
using rtmp_server::protocol::commands::LiveFanout;
using rtmp_server::protocol::commands::StreamRegistry;
using rtmp_server::protocol::handshake::HandshakeSession;
using rtmp_server::protocol::session::RtmpConnectionSession;

std::atomic<bool> g_stop{false};

// A peer that vanishes mid-write must not kill the process with SIGPIPE.
// Darwin/BSD: SO_NOSIGPIPE socket option. Linux: MSG_NOSIGNAL send flag.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

inline void suppress_sigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}


void on_signal(int) { g_stop.store(true); }

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Connection {
    int fd = -1;
    std::unique_ptr<HandshakeSession> handshake;
    std::unique_ptr<RtmpConnectionSession> session;
    std::vector<std::byte> out;
    std::size_t out_offset = 0;
    bool closing = false;
};

// Honest partial-write handling: a short ::send is normal, and the unsent
// remainder must be retried, never dropped.
void drain(Connection& conn, Metrics& metrics) {
    while (conn.out_offset < conn.out.size()) {
        const std::size_t remaining = conn.out.size() - conn.out_offset;
        const ssize_t n = ::send(conn.fd, conn.out.data() + conn.out_offset, remaining, kSendFlags);
        if (n > 0) {
            conn.out_offset += static_cast<std::size_t>(n);
            if (static_cast<std::size_t>(n) < remaining) metrics.increment(MetricId::PartialSendCount);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        conn.closing = true;
        return;
    }
    conn.out.clear();
    conn.out_offset = 0;
}

} // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 1935;
    int metrics_interval_s = 5;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--metrics-interval") == 0 && i + 1 < argc) metrics_interval_s = std::atoi(argv[++i]);
    }

    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN); // a peer that vanishes mid-write must not kill us

    Metrics metrics;
    StreamRegistry registry;
    LiveFanout fanout;
    registry.set_metrics(&metrics);
    fanout.set_metrics(&metrics);

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        return EXIT_FAILURE;
    }
    int reuse = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        return EXIT_FAILURE;
    }
    ::listen(listen_fd, 512);
    set_nonblocking(listen_fd);

    std::printf("rtmp_test_server (TEST-ONLY, poll-based, no io_uring) listening on 127.0.0.1:%u\n", port);
    std::fflush(stdout);

    std::vector<std::unique_ptr<Connection>> connections;
    std::uint64_t next_connection_id = 1;
    std::array<std::byte, 65536> buf{};
    auto last_metrics = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        std::vector<pollfd> pfds;
        pfds.reserve(connections.size() + 1);
        pollfd listener{};
        listener.fd = listen_fd;
        listener.events = POLLIN;
        pfds.push_back(listener);
        for (auto& conn : connections) {
            pollfd p{};
            p.fd = conn->fd;
            p.events = POLLIN;
            if (conn->out_offset < conn->out.size()) p.events |= POLLOUT;
            pfds.push_back(p);
        }

        const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 10);
        if (ready > 0) {
            if ((pfds[0].revents & POLLIN) != 0) {
                for (;;) {
                    const int fd = ::accept(listen_fd, nullptr, nullptr);
                    if (fd < 0) break;
                    set_nonblocking(fd);
                    suppress_sigpipe(fd);
                    int one = 1;
                    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    auto conn = std::make_unique<Connection>();
                    conn->fd = fd;
                    Connection* raw = conn.get();
                    conn->handshake = std::make_unique<HandshakeSession>();
                    conn->handshake->set_send_handler([raw](SharedBuffer b) {
                        raw->out.insert(raw->out.end(), b.view().begin(), b.view().end());
                    });
                    conn->handshake->set_fail_handler([raw](rtmp_server::core::Error) { raw->closing = true; });
                    conn->handshake->set_complete_handler([raw, &registry, &fanout, &metrics, &next_connection_id]() {
                        RtmpConnectionSession::Dependencies deps;
                        deps.registry = &registry;
                        deps.live_fanout = &fanout;
                        raw->session = std::make_unique<RtmpConnectionSession>(next_connection_id++, deps,
                                                                              8u * 1024u * 1024u, 4096);
                        raw->session->set_metrics(&metrics);
                        raw->session->set_outgoing_handler([raw](std::vector<std::byte> bytes) {
                            raw->out.insert(raw->out.end(), bytes.begin(), bytes.end());
                        });
                        raw->session->set_close_handler([raw]() { raw->closing = true; });
                        raw->session->start();
                        auto trailing = raw->handshake->take_trailing_bytes();
                        if (!trailing.empty()) raw->session->on_bytes_received(trailing);
                    });

                    metrics.add(MetricId::ActiveConnections, +1);
                    connections.push_back(std::move(conn));
                }
            }

            // The accept loop above may have appended new connections, so
            // `connections` can now be longer than the pollfd array built
            // before poll(). Only walk the polled prefix; indexing pfds by
            // connections.size() would read past the end.
            const std::size_t polled = pfds.size() - 1;
            for (std::size_t i = 0; i < polled && i < connections.size(); ++i) {
                auto& conn = *connections[i];
                const short revents = pfds[i + 1].revents;
                if (revents == 0) continue;
                if ((revents & POLLOUT) != 0) drain(conn, metrics);
                if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    for (;;) {
                        const ssize_t n = ::recv(conn.fd, buf.data(), buf.size(), 0);
                        if (n > 0) {
                            std::span<const std::byte> data(buf.data(), static_cast<std::size_t>(n));
                            if (!conn.handshake->is_terminal()) conn.handshake->on_bytes_received(data);
                            else if (conn.session) conn.session->on_bytes_received(data);
                            continue;
                        }
                        if (n == 0) { conn.closing = true; break; }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        conn.closing = true;
                        break;
                    }
                }
                if (!conn.closing) drain(conn, metrics);
            }
        } else {
            for (auto& conn : connections) {
                if (!conn->closing) drain(*conn, metrics);
            }
        }

        std::erase_if(connections, [&metrics](const std::unique_ptr<Connection>& conn) {
            if (!conn->closing) return false;
            if (conn->session) conn->session->on_connection_closed();
            if (conn->fd >= 0) ::close(conn->fd);
            metrics.add(MetricId::ActiveConnections, -1);
            return true;
        });

        const auto now = std::chrono::steady_clock::now();
        if (metrics_interval_s > 0 && now - last_metrics >= std::chrono::seconds{metrics_interval_s}) {
            last_metrics = now;
            fanout.sample_gauges();
            metrics.refresh_derived(now);
            metrics.refresh_process_metrics();
            metrics.set_connections_for_worker(0, static_cast<std::int64_t>(connections.size()));
            std::printf(
                "[metrics] conns=%lld pubs=%lld viewers=%lld vps_max=%lld ingress=%lld bps egress=%lld bps "
                "gop=%lld B queue=%lld B dropV=%lld dropA=%lld evict=%lld recover=%lld partial=%lld rss=%lld B\n",
                (long long)metrics.value(MetricId::ActiveConnections),
                (long long)metrics.value(MetricId::ActivePublishers),
                (long long)metrics.value(MetricId::ActiveViewers),
                (long long)metrics.value(MetricId::ViewersPerStreamMax),
                (long long)metrics.value(MetricId::IngressBitrate),
                (long long)metrics.value(MetricId::EgressBitrate),
                (long long)metrics.value(MetricId::GopCacheBytes),
                (long long)metrics.value(MetricId::OutboundQueueBytes),
                (long long)metrics.value(MetricId::DroppedVideoFrames),
                (long long)metrics.value(MetricId::DroppedAudioFrames),
                (long long)metrics.value(MetricId::SlowViewerEvictions),
                (long long)metrics.value(MetricId::SlowViewerRecoveries),
                (long long)metrics.value(MetricId::PartialSendCount),
                (long long)metrics.value(MetricId::ProcessMemoryBytes));
            std::fflush(stdout);
        }
    }

    for (auto& conn : connections) {
        if (conn->session) conn->session->on_connection_closed();
        if (conn->fd >= 0) ::close(conn->fd);
    }
    ::close(listen_fd);
    std::printf("%s", metrics.render_prometheus().c_str());
    return EXIT_SUCCESS;
}
