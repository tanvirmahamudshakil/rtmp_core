#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/control/async_http_server.hpp"
#include "rtmp_server/core/buffer.hpp"

namespace {

using namespace std::chrono_literals;
using rtmp_server::control::AsyncHttpServer;
using rtmp_server::control::AsyncHttpServerOptions;
using rtmp_server::control::HttpRequest;
using rtmp_server::control::HttpResponse;

// A blocking client socket. The server under test is the async side; the test
// client stays deliberately simple so a failure points at the server.
class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    ~Client() {
        if (fd_ >= 0) ::close(fd_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] bool connected() const { return connected_; }

    bool send_raw(std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Reads until the peer closes, or until `limit` bytes have arrived.
    std::string read_all(std::size_t limit = 1024 * 1024) {
        std::string out;
        char chunk[8192];
        while (out.size() < limit) {
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            out.append(chunk, static_cast<std::size_t>(n));
        }
        return out;
    }

    // Reads exactly one response, framed by its Content-Length. Needed on a
    // keep-alive connection, where the peer never closes to signal the end.
    std::string read_one_response() {
        std::string out;
        char chunk[8192];
        std::size_t header_end = std::string::npos;
        std::size_t expected = 0;
        while (true) {
            if (header_end == std::string::npos) {
                header_end = out.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    const auto pos = out.find("Content-Length: ");
                    expected = pos == std::string::npos
                                   ? 0
                                   : static_cast<std::size_t>(std::stoul(out.substr(pos + 16)));
                }
            }
            if (header_end != std::string::npos && out.size() >= header_end + 4 + expected) break;
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            out.append(chunk, static_cast<std::size_t>(n));
        }
        return out;
    }

private:
    int fd_ = -1;
    bool connected_ = false;
};

AsyncHttpServerOptions test_options() {
    AsyncHttpServerOptions options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.event_loops = 2;
    return options;
}

int status_of(const std::string& response) {
    if (response.size() < 12 || response.compare(0, 9, "HTTP/1.1 ") != 0) return -1;
    return std::stoi(response.substr(9, 3));
}

std::string body_of(const std::string& response) {
    const auto pos = response.find("\r\n\r\n");
    return pos == std::string::npos ? std::string{} : response.substr(pos + 4);
}

TEST(AsyncHttpServer, ServesASingleRequest) {
    AsyncHttpServer server(test_options());
    server.set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = "path=" + request.path + " query=" + request.query;
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /hls/live/demo.m3u8?a=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    const std::string response = client.read_all();

    EXPECT_EQ(status_of(response), 200);
    EXPECT_EQ(body_of(response), "path=/hls/live/demo.m3u8 query=a=1");
    server.stop();
}

TEST(AsyncHttpServer, KeepAliveServesManyRequestsOnOneConnection) {
    AsyncHttpServer server(test_options());
    std::atomic<int> seen{0};
    server.set_handler([&seen](const HttpRequest&) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = std::to_string(seen.fetch_add(1));
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(client.send_raw("GET /x HTTP/1.1\r\nHost: x\r\n\r\n"));
        const std::string response = client.read_one_response();
        EXPECT_EQ(status_of(response), 200) << "request " << i;
        EXPECT_EQ(body_of(response), std::to_string(i));
        EXPECT_NE(response.find("Connection: keep-alive"), std::string::npos);
    }
    EXPECT_EQ(seen.load(), 5);
    server.stop();
}

TEST(AsyncHttpServer, HandlesPipelinedRequests) {
    // Two requests arriving in one segment must both be answered, in order,
    // without the second being dropped with the first request's leftover bytes.
    AsyncHttpServer server(test_options());
    server.set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = request.path;
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /first HTTP/1.1\r\nHost: x\r\n\r\n"
                                "GET /second HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    const std::string response = client.read_all();

    const auto first = response.find("/first");
    const auto second = response.find("/second");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    EXPECT_LT(first, second) << "responses must be in request order";
    server.stop();
}

TEST(AsyncHttpServer, SendsSharedBufferBodyWithoutCopying) {
    // The segment delivery path: the response points into a shared, immutable
    // buffer and the server writes that memory straight to the socket.
    std::vector<std::byte> payload(256 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xff);
    }
    const auto shared = rtmp_server::core::SharedBuffer::adopt(std::move(payload));

    AsyncHttpServer server(test_options());
    server.set_handler([&shared](const HttpRequest&) {
        HttpResponse response;
        response.content_type = "video/mp2t";
        response.shared_body = shared;
        response.shared_body_length = shared.size();
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /seg.ts HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    const std::string response = client.read_all();

    ASSERT_EQ(status_of(response), 200);
    const std::string body = body_of(response);
    ASSERT_EQ(body.size(), shared.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        ASSERT_EQ(static_cast<unsigned char>(body[i]), i & 0xff) << "byte " << i;
    }
    server.stop();
}

TEST(AsyncHttpServer, ServesARangeSliceOfASharedBuffer) {
    std::vector<std::byte> payload(1024);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i & 0xff);
    const auto shared = rtmp_server::core::SharedBuffer::adopt(std::move(payload));

    AsyncHttpServer server(test_options());
    server.set_handler([&shared](const HttpRequest&) {
        HttpResponse response;
        response.status = 206;
        response.content_type = "video/mp2t";
        response.shared_body = shared;
        response.shared_body_offset = 100;
        response.shared_body_length = 50;
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /seg.ts HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    const std::string response = client.read_all();

    EXPECT_EQ(status_of(response), 206);
    const std::string body = body_of(response);
    ASSERT_EQ(body.size(), 50u);
    for (std::size_t i = 0; i < body.size(); ++i) {
        ASSERT_EQ(static_cast<unsigned char>(body[i]), (100 + i) & 0xff) << "byte " << i;
    }
    server.stop();
}

TEST(AsyncHttpServer, ReadsARequestBody) {
    AsyncHttpServer server(test_options());
    server.set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = "got:" + request.body;
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
                                "Connection: close\r\n\r\nhello"));
    const std::string response = client.read_all();
    EXPECT_EQ(body_of(response), "got:hello");
    server.stop();
}

TEST(AsyncHttpServer, RejectsAnOversizedHeaderBlock) {
    auto options = test_options();
    options.max_header_bytes = 1024;
    AsyncHttpServer server(options);
    server.set_handler([](const HttpRequest&) { return HttpResponse{}; });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    // Never send the terminating blank line: the server must cut this off at
    // the bound rather than buffer without limit.
    std::string request = "GET /x HTTP/1.1\r\nHost: x\r\n";
    request += "X-Pad: " + std::string(4096, 'a') + "\r\n";
    client.send_raw(request);
    const std::string response = client.read_all();
    EXPECT_EQ(status_of(response), 431);
    server.stop();
}

TEST(AsyncHttpServer, RejectsAnOversizedBody) {
    auto options = test_options();
    options.max_body_bytes = 16;
    AsyncHttpServer server(options);
    server.set_handler([](const HttpRequest&) { return HttpResponse{}; });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 999\r\n\r\n"));
    const std::string response = client.read_all();
    EXPECT_EQ(status_of(response), 413);
    server.stop();
}

TEST(AsyncHttpServer, RejectsAMalformedContentLength) {
    // A Content-Length the server cannot parse must be refused, never treated
    // as zero: the body bytes would then be parsed as the next request.
    AsyncHttpServer server(test_options());
    server.set_handler([](const HttpRequest&) { return HttpResponse{}; });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 12abc\r\n\r\n"));
    const std::string response = client.read_all();
    EXPECT_EQ(status_of(response), 400);
    server.stop();
}

TEST(AsyncHttpServer, RefusesConnectionsPastTheLimit) {
    auto options = test_options();
    options.event_loops = 1;
    options.max_connections = 2;
    AsyncHttpServer server(options);
    server.set_handler([](const HttpRequest&) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = "ok";
        return response;
    });
    ASSERT_TRUE(server.start());

    // Hold two connections open without completing a request so they occupy
    // the limit, then confirm the third is refused with a retryable 503.
    Client first(server.bound_port());
    Client second(server.bound_port());
    ASSERT_TRUE(first.connected());
    ASSERT_TRUE(second.connected());
    ASSERT_TRUE(first.send_raw("GET /x HTTP/1.1\r\nHost: x\r\n"));
    ASSERT_TRUE(second.send_raw("GET /x HTTP/1.1\r\nHost: x\r\n"));

    // Give the loop a moment to accept both before the third arrives.
    for (int i = 0; i < 100 && server.stats().accepted < 2; ++i) std::this_thread::sleep_for(10ms);

    Client third(server.bound_port());
    ASSERT_TRUE(third.connected());
    const std::string response = third.read_all();
    EXPECT_EQ(status_of(response), 503);
    EXPECT_NE(response.find("server_overloaded"), std::string::npos);
    EXPECT_GE(server.stats().rejected_over_limit, 1u);
    server.stop();
}

TEST(AsyncHttpServer, ClosesAnIdleKeepAliveConnection) {
    auto options = test_options();
    options.keep_alive_idle_timeout = 300ms;
    AsyncHttpServer server(options);
    server.set_handler([](const HttpRequest&) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = "ok";
        return response;
    });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /x HTTP/1.1\r\nHost: x\r\n\r\n"));
    EXPECT_EQ(status_of(client.read_one_response()), 200);

    // Now go idle. read_all returns when the server closes, so this both
    // asserts the timeout fires and that it does not fire before its budget.
    const auto started = std::chrono::steady_clock::now();
    (void)client.read_all();
    const auto waited = std::chrono::steady_clock::now() - started;
    EXPECT_GE(waited, 250ms);
    EXPECT_LT(waited, 5s);
    EXPECT_GE(server.stats().timeouts, 1u);
    server.stop();
}

TEST(AsyncHttpServer, ServesManyConcurrentConnections) {
    // The point of the whole design: far more simultaneous connections than
    // there are threads. Two loops, 200 connections, all served.
    auto options = test_options();
    options.event_loops = 2;
    AsyncHttpServer server(options);
    server.set_handler([](const HttpRequest& request) {
        HttpResponse response;
        response.content_type = "text/plain";
        response.body = request.path;
        return response;
    });
    ASSERT_TRUE(server.start());

    constexpr int kClients = 200;
    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    threads.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&server, &succeeded, i] {
            Client client(server.bound_port());
            if (!client.connected()) return;
            const std::string path = "/c" + std::to_string(i);
            if (!client.send_raw("GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")) {
                return;
            }
            const std::string response = client.read_all();
            if (status_of(response) == 200 && body_of(response) == path) {
                succeeded.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(succeeded.load(), kClients);
    server.stop();
}

TEST(AsyncHttpServer, StopIsIdempotentAndSafeWithLiveConnections) {
    AsyncHttpServer server(test_options());
    server.set_handler([](const HttpRequest&) { return HttpResponse{}; });
    ASSERT_TRUE(server.start());

    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /x HTTP/1.1\r\nHost: x\r\n\r\n"));
    (void)client.read_one_response();

    server.stop();
    server.stop(); // must not double-join or crash
    EXPECT_FALSE(server.running());
}

TEST(AsyncHttpServer, ReportsWithoutAHandler) {
    AsyncHttpServer server(test_options());
    ASSERT_TRUE(server.start());
    Client client(server.bound_port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send_raw("GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    EXPECT_EQ(status_of(client.read_all()), 503);
    server.stop();
}

} // namespace
