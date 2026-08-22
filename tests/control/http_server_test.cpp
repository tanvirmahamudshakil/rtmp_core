#include "rtmp_server/control/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

using rtmp_server::control::HttpRequest;
using rtmp_server::control::HttpResponse;
using rtmp_server::control::HttpServer;
using rtmp_server::control::HttpServerOptions;

namespace {

// Minimal blocking HTTP client sufficient for exercising HttpServer in
// tests: sends a raw request, reads until the peer closes (server always
// sends Connection: close), returns the full raw response text.
std::string do_request(std::uint16_t port, const std::string& raw_request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    ::send(fd, raw_request.data(), raw_request.size(), 0);
    std::string response;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) response.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return response;
}

} // namespace

TEST(HttpServerTest, RespondsToASimpleGetRequest) {
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options);
    server.set_handler([](const HttpRequest& req) {
        EXPECT_EQ(req.method, "GET");
        EXPECT_EQ(req.path, "/hello");
        return HttpResponse::json(200, R"({"ok":true})");
    });
    ASSERT_TRUE(server.start());

    auto response = do_request(server.bound_port(), "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find(R"({"ok":true})"), std::string::npos);

    server.stop();
}

TEST(HttpServerTest, RejectsBodyLargerThanConfiguredLimit) {
    HttpServerOptions options;
    options.port = 0;
    options.max_body_bytes = 8;
    HttpServer server(options);
    server.set_handler([](const HttpRequest&) { return HttpResponse::json(200, "{}"); });
    ASSERT_TRUE(server.start());

    std::string body(64, 'x');
    std::string request = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\n\r\n" + body;
    auto response = do_request(server.bound_port(), request);
    EXPECT_NE(response.find("413"), std::string::npos);

    server.stop();
}

TEST(HttpServerTest, ConnectionsBeyondPendingQueueReceiveRetryable503) {
    HttpServerOptions options;
    options.port = 0;
    options.worker_threads = 1;
    options.max_pending_requests = 1;
    HttpServer server(options);
    std::atomic<int> in_flight{0};
    server.set_handler([&](const HttpRequest&) {
        ++in_flight;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return HttpResponse::json(200, "{}");
    });
    ASSERT_TRUE(server.start());

    // Fire several connections concurrently; with only one worker and a
    // pending-queue depth of one, at least one must receive an explicit 503
    // rather than an empty reply or being left queued without bound.
    std::vector<std::thread> clients;
    std::atomic<int> overloaded_count{0};
    for (int i = 0; i < 6; ++i) {
        clients.emplace_back([&] {
            auto resp = do_request(server.bound_port(), "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
            if (resp.find("503 Service Unavailable") != std::string::npos &&
                resp.find("Retry-After: 1") != std::string::npos) {
                ++overloaded_count;
            }
        });
    }
    for (auto& t : clients) t.join();

    EXPECT_GT(overloaded_count.load(), 0);
    server.stop();
}
