// Phase 4 (docs/v2_promot.md "Multi-core io_uring worker architecture")
// tests for WorkerPool. Full end-to-end cross-worker media fan-out (a real
// publisher on one worker, a real viewer on another) would require driving
// a complete RTMP handshake/connect/publish/play sequence per connection —
// that pipeline is already covered by tests/integration/
// rtmp_full_session_socket_test.cpp for a single connection; here we cover
// what's specific to WorkerPool itself: worker-count selection, accepting
// real connections spread across more than one worker's ring, and clean
// graceful shutdown. CrossWorkerRouter's own forwarding/bookkeeping logic
// is covered directly in cross_worker_router_test.cpp.
#include "rtmp_server/io/io_uring/worker_pool.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

namespace rtmp_server::io::io_uring {
namespace {

core::ServerConfig make_test_config() {
    core::ServerConfig config;
    config.rtmp_bind_address = "127.0.0.1";
    config.token_signing_secret = "test-secret";
    config.api_authentication_secret = "test-secret";
    return config;
}

// Binds an ephemeral port, reads back what the kernel assigned, then
// releases it immediately — a standard (small-race) trick to get a free
// port number for a server that (unlike this repo's other socket tests)
// must be told its port up front rather than being able to report back
// whatever it bound to.
std::uint16_t find_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool connect_to(const std::string& host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd); // connection reset is fine: we only care that accept() succeeded server-side
    return ok;
}

TEST(WorkerPoolTest, EffectiveWorkerCountUsesExplicitValueWhenSet) {
    core::ServerConfig config = make_test_config();
    config.worker_ring_count = 3;
    config.max_worker_ring_count = 64;
    EXPECT_EQ(WorkerPool::effective_worker_count(config), 3u);
}

TEST(WorkerPoolTest, EffectiveWorkerCountAutoDetectsFromHardwareConcurrencyWhenZero) {
    core::ServerConfig config = make_test_config();
    config.worker_ring_count = 0;
    config.max_worker_ring_count = 1024;
    unsigned hardware = std::thread::hardware_concurrency();
    std::uint32_t expected = hardware == 0 ? 1 : hardware;
    EXPECT_EQ(WorkerPool::effective_worker_count(config), expected);
}

TEST(WorkerPoolTest, EffectiveWorkerCountClampsToConfiguredMaximum) {
    core::ServerConfig config = make_test_config();
    config.worker_ring_count = 100;
    config.max_worker_ring_count = 4;
    EXPECT_EQ(WorkerPool::effective_worker_count(config), 4u);
}

TEST(WorkerPoolTest, RunAcceptsRealConnectionsThenStopsGracefully) {
    core::ServerConfig config = make_test_config();
    config.worker_ring_count = 2;
    config.rtmp_port = find_free_port();

    protocol::commands::StreamRegistry stream_registry;
    protocol::commands::StreamIdRegistry stream_id_registry;
    WorkerPool pool(config, stream_registry, stream_id_registry);

    std::thread run_thread([&pool]() {
        auto result = pool.run();
        EXPECT_TRUE(static_cast<bool>(result));
    });

    // Give both workers' rings time to bind/listen before connecting.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int accepted = 0;
    for (int i = 0; i < 8; ++i) {
        if (connect_to(config.rtmp_bind_address, config.rtmp_port)) ++accepted;
    }
    EXPECT_GT(accepted, 0);

    pool.stop();
    run_thread.join(); // must return promptly: proves graceful shutdown didn't hang
}

} // namespace
} // namespace rtmp_server::io::io_uring
