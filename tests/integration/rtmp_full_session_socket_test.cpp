// Real-loopback-socket version of the Phase 1 pipeline (docs/v2_promot.md
// "PHASE 1" Required tests: "Abrupt disconnect during handshake/publish/
// play", "Connection cleanup"). Wires HandshakeSession -> (trailing bytes) ->
// RtmpConnectionSession exactly the way
// IoUringEventLoop::start_handshake/start_rtmp_session does in
// src/io/io_uring/event_loop.cpp, but drives it with a plain blocking POSIX
// socket read/write loop instead of io_uring — the same substitution
// tests/protocol/handshake/handshake_socket_integration_test.cpp already
// makes, extended one layer further up the pipeline. This is what proves
// "socket input reaches the chunk decoder" and "abrupt disconnect" cleanup
// over an actual TCP connection on a host where the io_uring transport
// itself cannot be built (macOS/Darwin — CMAKE_SYSTEM_NAME STREQUAL "Linux"
// guard). It does not exercise io_uring-specific behavior (partial sends,
// completion cancellation, ...) — that is Phase 2's scope.
//
// This test file is also what caught a real Phase 1 bug during
// development: RtmpConnectionSession must send a Set Chunk Size control
// message before encoding any message with a non-default chunk size, or a
// real peer's decoder (which still assumes chunk::kDefaultChunkSize until
// told otherwise) stalls forever trying to reassemble it — see
// RtmpConnectionSession::start() in rtmp_connection_session.cpp/.hpp, added
// specifically because HandshakePlusConnectInSameWriteReachesTheSession
// below failed without it.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_encoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_decoder.hpp"
#include "rtmp_server/protocol/chunk/chunk_encoder.hpp"
#include "rtmp_server/protocol/commands/live_fanout.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"
#include "rtmp_server/protocol/handshake/handshake_session.hpp"
#include "rtmp_server/protocol/session/rtmp_connection_session.hpp"

namespace rtmp_server::protocol::session {
namespace {

using amf0::Amf0Value;
using chunk::MessageTypeId;
using chunk::RtmpMessage;
using commands::LiveFanout;
using commands::StreamRegistry;
using handshake::HandshakeSession;

// Blocking-socket server harness: accepts one connection, runs
// HandshakeSession, then (mirroring IoUringEventLoop::start_rtmp_session)
// hands any trailing bytes plus every subsequent read into a
// RtmpConnectionSession. Test-only glue, not production code — the
// production wiring lives in src/io/io_uring/event_loop.cpp and is only
// buildable on Linux.
class LoopbackServer {
public:
    LoopbackServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listen_fd_, 1);
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    ~LoopbackServer() {
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] bool disconnected() const { return disconnected_.load(); }
    [[nodiscard]] bool published() const { return published_.load(); }
    [[nodiscard]] StreamRegistry& registry() { return registry_; }

    void run_one_connection() {
        thread_ = std::thread([this]() {
            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) return;

            HandshakeSession handshake;
            handshake.set_send_handler([client_fd](core::SharedBuffer buf) {
                auto view = buf.view();
                std::size_t sent = 0;
                while (sent < view.size()) {
                    ssize_t n = ::send(client_fd, view.data() + sent, view.size() - sent, 0);
                    if (n <= 0) return;
                    sent += static_cast<std::size_t>(n);
                }
            });

            std::unique_ptr<RtmpConnectionSession> rtmp_session;
            auto install_rtmp_session = [&](std::vector<std::byte> trailing) {
                RtmpConnectionSession::Dependencies deps;
                deps.registry = &registry_;
                deps.live_fanout = &fanout_;
                rtmp_session = std::make_unique<RtmpConnectionSession>(1, deps, 1024 * 1024, 4096);
                rtmp_session->set_outgoing_handler([client_fd](std::vector<std::byte> bytes) {
                    std::size_t sent = 0;
                    while (sent < bytes.size()) {
                        ssize_t n = ::send(client_fd, bytes.data() + sent, bytes.size() - sent, 0);
                        if (n <= 0) return;
                        sent += static_cast<std::size_t>(n);
                    }
                });
                rtmp_session->set_close_handler([]() { /* would close the socket in production */ });
                // Handlers must be wired before start() (Set Chunk Size may
                // be emitted synchronously) and before any trailing bytes
                // are fed in, mirroring IoUringEventLoop::start_rtmp_session.
                rtmp_session->start();
                if (!trailing.empty()) rtmp_session->on_bytes_received(trailing);
            };

            handshake.set_complete_handler([&]() { install_rtmp_session(handshake.take_trailing_bytes()); });
            handshake.set_fail_handler([](core::Error) {});

            std::array<std::byte, 512> read_buf{};
            for (;;) {
                ssize_t n = ::recv(client_fd, read_buf.data(), read_buf.size(), 0);
                if (n <= 0) break; // peer closed / abrupt disconnect
                std::span<const std::byte> data(read_buf.data(), static_cast<std::size_t>(n));
                if (!handshake.is_terminal()) {
                    handshake.on_bytes_received(data);
                } else if (rtmp_session) {
                    rtmp_session->on_bytes_received(data);
                    if (registry_.is_published("mykey")) published_.store(true);
                }
            }

            if (rtmp_session) rtmp_session->on_connection_closed(); // deterministic teardown on disconnect
            disconnected_.store(true);
            ::close(client_fd);
        });
    }

private:
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> disconnected_{false};
    std::atomic<bool> published_{false};
    StreamRegistry registry_;
    LiveFanout fanout_;
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
    std::vector<std::byte> c1(handshake::kHandshakeChunkSize, std::byte{0});
    for (std::size_t i = 8; i < c1.size(); ++i) c1[i] = static_cast<std::byte>(i % 256);
    return c1;
}

void wait_until(const std::function<bool()>& predicate, int max_iterations = 200) {
    for (int i = 0; i < max_iterations && !predicate(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

RtmpMessage make_command(std::uint32_t message_stream_id, std::vector<Amf0Value> values) {
    RtmpMessage msg;
    msg.chunk_stream_id = 3;
    msg.message_stream_id = message_stream_id;
    msg.message_type_id = static_cast<std::uint8_t>(MessageTypeId::Amf0Command);
    for (const auto& v : values) amf0::encode(v, msg.payload);
    return msg;
}

std::vector<std::byte> encode(const RtmpMessage& message) {
    chunk::ChunkEncoder encoder;
    std::vector<std::byte> out;
    encoder.encode_message(message, out);
    return out;
}

void send_all(int fd, std::span<const std::byte> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        ASSERT_GT(n, 0);
        sent += static_cast<std::size_t>(n);
    }
}

TEST(RtmpFullSessionSocketIntegration, HandshakePlusConnectInSameWriteReachesTheSession) {
    LoopbackServer server;
    server.run_one_connection();
    int fd = connect_to(server.port());
    ASSERT_GE(fd, 0);

    // C0+C1, then read S0/S1/S2 back so C2 can echo a plausible size.
    std::vector<std::byte> c0c1 = {std::byte{handshake::kRtmpVersion}};
    auto c1 = make_c1();
    c0c1.insert(c0c1.end(), c1.begin(), c1.end());
    send_all(fd, c0c1);

    std::vector<std::byte> s0s1s2(handshake::kC0Size + 2 * handshake::kHandshakeChunkSize);
    std::size_t received = 0;
    while (received < s0s1s2.size()) {
        ssize_t n = ::recv(fd, s0s1s2.data() + received, s0s1s2.size() - received, 0);
        ASSERT_GT(n, 0);
        received += static_cast<std::size_t>(n);
    }

    // C2 pipelined together with `connect` in a single write — the case
    // production-gap-analysis item #3 documents as historically broken.
    std::vector<std::byte> c2(handshake::kHandshakeChunkSize, std::byte{0x42});
    auto connect_bytes = encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    std::vector<std::byte> combined = c2;
    combined.insert(combined.end(), connect_bytes.begin(), connect_bytes.end());
    send_all(fd, combined);

    // Expect a `_result` (NetConnection.Connect.Success) reply, proving the
    // C2-adjacent `connect` bytes were not discarded, and that the server's
    // larger encoder chunk size (4096) round-trips correctly against a
    // decoder that starts out assuming the RTMP default (128) until told
    // otherwise via Set Chunk Size.
    chunk::ChunkDecoder decoder(1024);
    std::atomic<bool> got_result{false};
    decoder.set_message_handler([&](const RtmpMessage& m) {
        if (m.message_type_id != static_cast<std::uint8_t>(MessageTypeId::Amf0Command)) return;
        auto decoded = amf0::decode_all(m.payload);
        if (decoded.ok() && !decoded.value().empty() && decoded.value()[0].as_string() == "_result") {
            got_result.store(true);
        }
    });

    std::array<std::byte, 256> buf{};
    for (int i = 0; i < 200 && !got_result.load(); ++i) {
        // Short recv timeout so the test fails fast instead of hanging if
        // something regresses, rather than being a real poll mechanism.
        timeval tv{0, 20000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) decoder.on_bytes_received(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)));
    }

    EXPECT_TRUE(got_result.load());
    ::close(fd);
}

TEST(RtmpFullSessionSocketIntegration, AbruptDisconnectDuringPublishCleansUpRegistry) {
    LoopbackServer server;
    server.run_one_connection();
    int fd = connect_to(server.port());
    ASSERT_GE(fd, 0);

    std::vector<std::byte> c0c1 = {std::byte{handshake::kRtmpVersion}};
    auto c1 = make_c1();
    c0c1.insert(c0c1.end(), c1.begin(), c1.end());
    send_all(fd, c0c1);
    std::vector<std::byte> s0s1s2(handshake::kC0Size + 2 * handshake::kHandshakeChunkSize);
    std::size_t received = 0;
    while (received < s0s1s2.size()) {
        ssize_t n = ::recv(fd, s0s1s2.data() + received, s0s1s2.size() - received, 0);
        ASSERT_GT(n, 0);
        received += static_cast<std::size_t>(n);
    }
    std::vector<std::byte> c2(handshake::kHandshakeChunkSize, std::byte{0x42});
    send_all(fd, c2);

    auto connect_bytes = encode(make_command(
        0, {Amf0Value::string("connect"), Amf0Value::number(1), Amf0Value::object({{"app", Amf0Value::string("live")}})}));
    send_all(fd, connect_bytes);
    auto create_bytes =
        encode(make_command(0, {Amf0Value::string("createStream"), Amf0Value::number(2), Amf0Value::null()}));
    send_all(fd, create_bytes);
    auto publish_bytes = encode(make_command(
        1, {Amf0Value::string("publish"), Amf0Value::number(0), Amf0Value::null(), Amf0Value::string("mykey"),
            Amf0Value::string("live")}));
    send_all(fd, publish_bytes);

    wait_until([&]() { return server.published(); });
    ASSERT_TRUE(server.published());
    ASSERT_TRUE(server.registry().is_published("mykey"));

    // Abrupt disconnect: close the socket without a clean RTMP teardown
    // (no deleteStream). The server's recv() loop must observe EOF/error,
    // and its finally-block equivalent (rtmp_session->on_connection_closed())
    // must remove the publisher registration deterministically.
    ::close(fd);

    wait_until([&]() { return server.disconnected(); });
    ASSERT_TRUE(server.disconnected());
    EXPECT_FALSE(server.registry().is_published("mykey"));
}

} // namespace
} // namespace rtmp_server::protocol::session
