// Byte-correct RTMP handshake exercised over a real loopback TCP socket,
// substituting for an actual OBS/RTMP client in this environment (see
// docs/rtmp_promot.md Phase 2 acceptance criteria and the Phase 2 report).
//
// This deliberately does NOT go through io_uring/TcpConnection — the
// protocol layer (HandshakeSession) is transport-agnostic by design
// (docs/architecture.md "Architectural Separation"), and this test's own
// harness (below) is test-only glue: a plain blocking-socket TCP loop that
// feeds real, possibly-fragmented network reads into a HandshakeSession
// exactly like the io_uring transport does in production
// (src/io/io_uring/event_loop.cpp's start_handshake()). It runs on any
// POSIX platform, so it exercises the full state machine even where
// io_uring itself is unavailable (e.g. this build host).

#include "rtmp_server/protocol/handshake/handshake_session.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace rtmp_server::protocol::handshake {
namespace {

// Minimal blocking-socket test server: accepts exactly one connection,
// drives a HandshakeSession off whatever real `recv()` fragmentation the
// kernel/socket happens to produce, and writes the session's responses
// back with `send()`. This is intentionally not production code — it
// exists only to prove HandshakeSession behaves correctly end-to-end over
// an actual TCP connection.
class LoopbackTestServer {
public:
    LoopbackTestServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // let the OS pick a free port
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 1);

        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    ~LoopbackTestServer() {
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }

    void run_one_connection() {
        thread_ = std::thread([this]() {
            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) return;

            HandshakeSession session;
            session.set_send_handler([client_fd](core::SharedBuffer buf) {
                auto view = buf.view();
                std::size_t sent = 0;
                while (sent < view.size()) {
                    // Exercise partial-write handling the same way
                    // production async_write does — send in small pieces.
                    std::size_t chunk = std::min<std::size_t>(97, view.size() - sent);
                    ssize_t n = ::send(client_fd, view.data() + sent, chunk, 0);
                    if (n <= 0) return;
                    sent += static_cast<std::size_t>(n);
                }
            });
            session.set_complete_handler([this]() { completed_.store(true); });
            session.set_fail_handler([this](core::Error) { failed_.store(true); });

            std::array<std::byte, 256> read_buf{};
            while (!session.is_terminal()) {
                ssize_t n = ::recv(client_fd, read_buf.data(), read_buf.size(), 0);
                if (n <= 0) break;
                session.on_bytes_received(
                    std::span<const std::byte>(read_buf.data(), static_cast<std::size_t>(n)));
            }
            ::close(client_fd);
        });
    }

    [[nodiscard]] bool completed() const { return completed_.load(); }
    [[nodiscard]] bool failed() const { return failed_.load(); }

private:
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> completed_{false};
    std::atomic<bool> failed_{false};
};

int connect_to(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::vector<std::byte> make_c1() {
    std::vector<std::byte> c1(kHandshakeChunkSize, std::byte{0});
    for (std::size_t i = 8; i < c1.size(); ++i) c1[i] = static_cast<std::byte>(i % 256);
    return c1;
}

void send_fragmented(int fd, std::span<const std::byte> data, std::size_t piece) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        std::size_t n = std::min(piece, data.size() - sent);
        ssize_t written = ::send(fd, data.data() + sent, n, 0);
        ASSERT_GT(written, 0);
        sent += static_cast<std::size_t>(written);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<std::byte> recv_exact(int fd, std::size_t n) {
    std::vector<std::byte> out(n);
    std::size_t received = 0;
    while (received < n) {
        ssize_t r = ::recv(fd, out.data() + received, n - received, 0);
        if (r <= 0) break;
        received += static_cast<std::size_t>(r);
    }
    out.resize(received);
    return out;
}

TEST(HandshakeSocketIntegration, RealLoopbackSocketCompletesHandshake) {
    LoopbackTestServer server;
    server.run_one_connection();

    int client_fd = connect_to(server.port());
    ASSERT_GE(client_fd, 0);

    // C0 and C1, deliberately sent in small fragments to prove the real
    // kernel socket's fragmentation is handled correctly end-to-end.
    std::vector<std::byte> c0c1;
    c0c1.push_back(std::byte{kRtmpVersion});
    auto c1 = make_c1();
    c0c1.insert(c0c1.end(), c1.begin(), c1.end());
    send_fragmented(client_fd, c0c1, 173);

    // S0 + S1 + S2.
    auto response = recv_exact(client_fd, kC0Size + 2 * kHandshakeChunkSize);
    ASSERT_EQ(response.size(), kC0Size + 2 * kHandshakeChunkSize);
    EXPECT_EQ(static_cast<std::uint8_t>(response[0]), kRtmpVersion);

    // C2, also fragmented.
    std::vector<std::byte> c2(kHandshakeChunkSize, std::byte{0x42});
    send_fragmented(client_fd, c2, 211);

    // Give the server thread a moment to process the final fragment and
    // invoke the complete handler before we assert on it.
    for (int i = 0; i < 200 && !server.completed() && !server.failed(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(server.completed());
    EXPECT_FALSE(server.failed());

    ::close(client_fd);
}

TEST(HandshakeSocketIntegration, RealLoopbackSocketRejectsInvalidVersion) {
    LoopbackTestServer server;
    server.run_one_connection();

    int client_fd = connect_to(server.port());
    ASSERT_GE(client_fd, 0);

    std::vector<std::byte> bad_c0 = {std::byte{0x99}};
    ASSERT_GT(::send(client_fd, bad_c0.data(), bad_c0.size(), 0), 0);

    for (int i = 0; i < 200 && !server.completed() && !server.failed(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_FALSE(server.completed());
    EXPECT_TRUE(server.failed());

    ::close(client_fd);
}

} // namespace
} // namespace rtmp_server::protocol::handshake
