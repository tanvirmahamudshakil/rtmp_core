#include <gtest/gtest.h>

#include <mutex>

#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/dispatch/agent_http_client.hpp"

using namespace rtmp_server;
using rtmp_server::control::HttpRequest;
using rtmp_server::control::HttpResponse;
using rtmp_server::control::HttpServer;
using rtmp_server::control::HttpServerOptions;
using rtmp_server::dispatch::AgentHttpClient;

namespace {

class FakeAgent {
public:
    FakeAgent() {
        HttpServerOptions options;
        options.bind_address = "127.0.0.1";
        options.port = 0;
        server_ = std::make_unique<HttpServer>(options);
        server_->set_handler([this](const HttpRequest& request) { return handle(request); });
        started_ = server_->start();
    }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] std::uint16_t port() const { return server_->bound_port(); }

    void set_response(int status, std::string body) {
        response_status_ = status;
        response_body_ = std::move(body);
    }

    // handle() runs on an HttpServer worker thread; these are read from the
    // test's main thread after the client call returns, so they need a lock
    // -- an earlier, unsynchronized version of this same pattern in
    // transcoder_dispatch_manager_test.cpp's FakeAgent produced visibly
    // corrupted string content under this exact race.
    [[nodiscard]] std::vector<std::string> methods() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return methods_;
    }
    [[nodiscard]] std::vector<std::string> paths() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return paths_;
    }
    [[nodiscard]] std::vector<std::string> bodies() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bodies_;
    }

private:
    HttpResponse handle(const HttpRequest& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        methods_.push_back(request.method);
        paths_.push_back(request.path);
        bodies_.push_back(request.body);
        return HttpResponse::json(response_status_, response_body_);
    }

    std::unique_ptr<HttpServer> server_;
    bool started_ = false;
    mutable std::mutex mutex_;
    int response_status_ = 200;
    std::string response_body_ = "{}";
    std::vector<std::string> methods_;
    std::vector<std::string> paths_;
    std::vector<std::string> bodies_;
};

TEST(AgentHttpClientTest, PostsAJsonBodyAndReturnsTheResponse) {
    FakeAgent agent;
    ASSERT_TRUE(agent.started());
    agent.set_response(201, R"({"accepted":true})");

    AgentHttpClient client;
    auto response = client.post("127.0.0.1", agent.port(), "/jobs", R"({"id":"live/main"})");
    ASSERT_TRUE(response) << response.error().message();
    EXPECT_EQ(response.value().status, 201);
    EXPECT_EQ(response.value().body, R"({"accepted":true})");

    ASSERT_EQ(agent.methods().size(), 1u);
    EXPECT_EQ(agent.methods().front(), "POST");
    EXPECT_EQ(agent.paths().front(), "/jobs");
    EXPECT_EQ(agent.bodies().front(), R"({"id":"live/main"})");
}

TEST(AgentHttpClientTest, SendsDeleteWithNoBody) {
    FakeAgent agent;
    ASSERT_TRUE(agent.started());
    agent.set_response(200, R"({"deleted":true})");

    AgentHttpClient client;
    auto response = client.del("127.0.0.1", agent.port(), "/jobs/live%2Fmain");
    ASSERT_TRUE(response) << response.error().message();
    EXPECT_EQ(response.value().status, 200);
    EXPECT_EQ(agent.methods().front(), "DELETE");
    EXPECT_TRUE(agent.bodies().front().empty());
}

TEST(AgentHttpClientTest, ReportsTheAgentsOwnErrorStatus) {
    FakeAgent agent;
    ASSERT_TRUE(agent.started());
    agent.set_response(409, R"({"error":"already running"})");

    AgentHttpClient client;
    auto response = client.post("127.0.0.1", agent.port(), "/jobs", "{}");
    ASSERT_TRUE(response); // the transport succeeded; the caller inspects .status
    EXPECT_EQ(response.value().status, 409);
}

TEST(AgentHttpClientTest, FailsWithoutHangingWhenNothingIsListening) {
    AgentHttpClient::Options options;
    options.connect_timeout = std::chrono::seconds(1);
    AgentHttpClient client(options);
    // Port 1 is privileged/unassigned on loopback and should refuse
    // immediately rather than accept.
    auto response = client.post("127.0.0.1", 1, "/jobs", "{}");
    ASSERT_FALSE(response);
}

} // namespace
