#include "rtmp_server/control/management_api.hpp"

#include <gtest/gtest.h>

using rtmp_server::control::HttpRequest;
using rtmp_server::control::ManagementApi;
using rtmp_server::control::ManagementApiOptions;
using rtmp_server::management::StreamManager;

namespace {

StreamManager::Options manager_options() {
    StreamManager::Options options;
    options.public_hostname = "localhost";
    options.rtmp_port = 1935;
    options.token_signing_secret = "test-secret";
    return options;
}

ManagementApiOptions api_options() {
    ManagementApiOptions options;
    options.admin_token = "admin-secret-token";
    options.require_authentication = true;
    return options;
}

HttpRequest authed(std::string method, std::string path, std::string body = "") {
    HttpRequest r;
    r.method = std::move(method);
    r.path = std::move(path);
    r.body = std::move(body);
    r.headers["authorization"] = "Bearer admin-secret-token";
    r.client_ip = "203.0.113.9";
    return r;
}

} // namespace

TEST(ManagementApiTest, HealthLiveNeedsNoAuth) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    HttpRequest r;
    r.method = "GET";
    r.path = "/health/live";
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 200);
}

TEST(ManagementApiTest, ControlPlaneIsOpenByDefault) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, ManagementApiOptions{});
    HttpRequest r;
    r.method = "POST";
    r.path = "/v1/applications";
    r.body = R"({"name":"live"})";
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 201);
}

TEST(ManagementApiTest, WrongTokenIsRejected) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    HttpRequest r = authed("GET", "/v1/applications");
    r.headers["authorization"] = "Bearer wrong-token";
    auto response = api.handle(r);
    EXPECT_EQ(response.status, 401);
}

TEST(ManagementApiTest, CreateAndListApplications) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());

    auto create = api.handle(authed("POST", "/v1/applications", R"({"name":"live"})"));
    EXPECT_EQ(create.status, 201);

    auto list = api.handle(authed("GET", "/v1/applications"));
    EXPECT_EQ(list.status, 200);
    EXPECT_NE(list.body.find("\"live\""), std::string::npos);
}

TEST(ManagementApiTest, CreateStreamReturnsOneUniversalRtmpUrl) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);

    auto create = api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})"));
    ASSERT_EQ(create.status, 201);
    EXPECT_EQ(create.body.find("stream_key"), std::string::npos);
    EXPECT_EQ(create.body.find("publish_name"), std::string::npos);
    EXPECT_EQ(create.body.find("publish_url"), std::string::npos);
    EXPECT_EQ(create.body.find("playback_url"), std::string::npos);
    EXPECT_NE(create.body.find(R"("rtmp_url":"rtmp://localhost:1935/live/alpha")"), std::string::npos);
    EXPECT_NE(create.body.find(R"("hls_path":"/hls/live/alpha/index.m3u8")"), std::string::npos);
    EXPECT_EQ(create.body.find(R"("stream":)"), std::string::npos);

    auto get = api.handle(authed("GET", "/v1/streams/live:alpha"));
    EXPECT_EQ(get.status, 200);
    EXPECT_EQ(get.body.find("stream_key"), std::string::npos); // never leaked from GET
    // The same URL is exposed on every read for publishers and viewers.
    EXPECT_EQ(get.body.find("playback_url"), std::string::npos);
    EXPECT_NE(get.body.find("rtmp_url"), std::string::npos);
    EXPECT_NE(get.body.find("/live/alpha"), std::string::npos);
}

TEST(ManagementApiTest, PercentEncodedStreamIdInPathResolves) {
    // The web panel builds stream-id path segments with
    // encodeURIComponent("<app>:<name>"), so the ':' arrives as "%3A". The
    // API must percent-decode it before splitting; otherwise every
    // per-stream action fails with "application not found".
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    ASSERT_EQ(api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})")).status, 201);

    auto get = api.handle(authed("GET", "/v1/streams/live%3Aalpha"));
    EXPECT_EQ(get.status, 200);
    EXPECT_NE(get.body.find("\"name\":\"alpha\""), std::string::npos);

    auto patch = api.handle(authed("PATCH", "/v1/streams/live%3Aalpha", R"({"enabled":false})"));
    EXPECT_EQ(patch.status, 200);
    EXPECT_NE(patch.body.find("\"enabled\":false"), std::string::npos);
}

TEST(ManagementApiTest, PatchDisablesAStream) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    ASSERT_EQ(api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})")).status, 201);

    auto patch = api.handle(authed("PATCH", "/v1/streams/live:alpha", R"({"enabled":false})"));
    EXPECT_EQ(patch.status, 200);
    EXPECT_NE(patch.body.find("\"enabled\":false"), std::string::npos);
}

TEST(ManagementApiTest, DeleteStreamRemovesItPermanently) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    ASSERT_EQ(api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})")).status, 201);

    auto removed = api.handle(authed("DELETE", "/v1/streams/live%3Aalpha"));
    EXPECT_EQ(removed.status, 200);
    EXPECT_NE(removed.body.find(R"("deleted":true)"), std::string::npos);
    EXPECT_EQ(api.handle(authed("GET", "/v1/streams/live:alpha")).status, 404);
    EXPECT_EQ(api.handle(authed("DELETE", "/v1/streams/live:alpha")).status, 404);
}

TEST(ManagementApiTest, PublishKeyRotationRouteIsNotExposedInOpenMode) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    auto created = api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})"));
    ASSERT_EQ(created.status, 201);

    auto rotate = api.handle(authed("POST", "/v1/streams/live:alpha/rotate-publish-key"));
    EXPECT_EQ(rotate.status, 404);
}

TEST(ManagementApiTest, PlaybackTokenRouteIsNotExposedInOpenMode) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    ASSERT_EQ(api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})")).status, 201);

    auto token_response = api.handle(authed("POST", "/v1/streams/live:alpha/playback-token", R"({"ttl_seconds":60})"));
    EXPECT_EQ(token_response.status, 404);
}

TEST(ManagementApiTest, StatusWithoutRegistryWiredReturnsServiceUnavailable) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    ASSERT_EQ(api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})")).status, 201);

    auto status = api.handle(authed("GET", "/v1/streams/live:alpha/status"));
    EXPECT_EQ(status.status, 503);
}

TEST(ManagementApiTest, StatusUsesInjectedMultiWorkerLiveState) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    ASSERT_EQ(api.handle(authed("POST", "/v1/applications", R"({"name":"live"})")).status, 201);
    ASSERT_EQ(api.handle(authed("POST", "/v1/streams", R"({"application":"live","name":"alpha"})")).status, 201);

    api.set_live_state_provider([] {
        return std::vector<rtmp_server::management::LiveState>{
            {.application = "live", .name = "alpha", .is_live = true, .viewer_count = 42},
        };
    });

    auto status = api.handle(authed("GET", "/v1/streams/live:alpha/status"));
    EXPECT_EQ(status.status, 200);
    EXPECT_NE(status.body.find(R"("is_live":true)"), std::string::npos);
    EXPECT_NE(status.body.find(R"("viewer_count":42)"), std::string::npos);
}

TEST(ManagementApiTest, UnknownRouteReturns404) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    auto response = api.handle(authed("GET", "/v1/nonsense"));
    EXPECT_EQ(response.status, 404);
}

TEST(ManagementApiTest, RepeatedBadTokensLockOutTheIp) {
    StreamManager manager(manager_options());
    auto options = api_options();
    options.max_auth_failures_per_ip = 2;
    ManagementApi api(manager, options);

    HttpRequest bad = authed("GET", "/v1/applications");
    bad.headers["authorization"] = "Bearer wrong";
    EXPECT_EQ(api.handle(bad).status, 401);
    EXPECT_EQ(api.handle(bad).status, 401);

    // Now even the *correct* token is locked out for this IP.
    auto good = authed("GET", "/v1/applications");
    EXPECT_EQ(api.handle(good).status, 401);
}
