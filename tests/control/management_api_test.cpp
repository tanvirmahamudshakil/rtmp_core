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

TEST(ManagementApiTest, SourceJobsCanBeCreatedListedAndRemoved) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());

    std::string created_for;
    api.set_source_job_handlers(
        [&](std::string_view application) -> rtmp_server::core::Result<std::string> {
            return std::string(R"({"items":[{"application":")") + std::string(application) + R"("}]})";
        },
        [&](std::string_view application, std::string_view name, std::string_view source_url,
            std::string_view /*tmpl*/, std::string_view /*rules*/, bool /*auto_restart*/,
            std::uint32_t /*restart_delay_seconds*/)
            -> rtmp_server::core::Result<std::string> {
            created_for = std::string(application) + "/" + std::string(name) + " <- " + std::string(source_url);
            return std::string(R"({"id":")") + std::string(application) + ":" + std::string(name) +
                   R"(","status":"running"})";
        },
        [&](std::string_view, std::string_view) -> rtmp_server::core::Result<void> { return {}; });

    // Create via POST with the header-carried inputs.
    HttpRequest post = authed("POST", "/v1/transcoding/source-jobs", "live/restream|720p|restream_720p|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first|HD");
    post.headers["x-application"] = "live";
    post.headers["x-output-name"] = "restream";
    post.headers["x-source-url"] = "https://cdn.example.com/live/index.m3u8";
    post.headers["x-template-name"] = "ladder";
    auto create = api.handle(post);
    EXPECT_EQ(create.status, 200);
    EXPECT_NE(create.body.find("\"status\":\"running\""), std::string::npos);
    EXPECT_EQ(created_for, "live/restream <- https://cdn.example.com/live/index.m3u8");

    // List.
    HttpRequest list_req = authed("GET", "/v1/transcoding/source-jobs");
    list_req.query = "application=live";
    auto list = api.handle(list_req);
    EXPECT_EQ(list.status, 200);
    EXPECT_NE(list.body.find("\"application\":\"live\""), std::string::npos);

    // Delete.
    auto del = api.handle(authed("DELETE", "/v1/transcoding/source-jobs/live:restream"));
    EXPECT_EQ(del.status, 200);
    EXPECT_NE(del.body.find("\"deleted\":true"), std::string::npos);
}

TEST(ManagementApiTest, SourceJobCreateRejectsMissingHeaders) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    api.set_source_job_handlers(
        [](std::string_view) -> rtmp_server::core::Result<std::string> { return std::string("{}"); },
        [](std::string_view, std::string_view, std::string_view, std::string_view,
           std::string_view, bool, std::uint32_t) -> rtmp_server::core::Result<std::string> {
            return std::string("{}");
        },
        [](std::string_view, std::string_view) -> rtmp_server::core::Result<void> { return {}; });

    // No X-* headers, empty body: rejected as a bad request.
    auto response = api.handle(authed("POST", "/v1/transcoding/source-jobs"));
    EXPECT_EQ(response.status, 400);
}

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

TEST(ManagementApiTest, TranscodingStatusIsSafeWhenSupervisorIsDisabled) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    auto response = api.handle(authed("GET", "/v1/transcoding/status"));
    EXPECT_EQ(response.status, 200);
    EXPECT_NE(response.body.find(R"("enabled":false)"), std::string::npos);
    EXPECT_NE(response.body.find(R"("jobs":[])"), std::string::npos);
}

TEST(ManagementApiTest, TranscodingAssignmentsCanBeListedAppliedAndRemoved) {
    StreamManager manager(manager_options());
    ManagementApi api(manager, api_options());
    std::string applied_rules;
    bool removed = false;
    api.set_transcoding_assignment_handlers(
        [](std::string_view application) -> rtmp_server::core::Result<std::string> {
            EXPECT_EQ(application, "football");
            return std::string(R"({"items":[]})");
        },
        [&applied_rules](std::string_view application, std::string_view source,
                         std::string_view template_name,
                         std::string_view rules) -> rtmp_server::core::Result<std::string> {
            EXPECT_EQ(application, "football");
            EXPECT_EQ(source, "live2");
            EXPECT_EQ(template_name, "Sports HD");
            applied_rules = rules;
            return std::string(R"({"application":"football","source_stream":"live2"})");
        },
        [&removed](std::string_view application,
                    std::string_view source) -> rtmp_server::core::Result<void> {
            EXPECT_EQ(application, "football");
            EXPECT_EQ(source, "live2");
            removed = true;
            return {};
        });

    auto list_request = authed("GET", "/v1/transcoding/assignments");
    list_request.query = "application=football";
    auto list = api.handle(list_request);
    EXPECT_EQ(list.status, 200);
    EXPECT_EQ(list.body, R"({"items":[]})");

    auto put = authed("PUT", "/v1/transcoding/assignments/football%3Alive2",
                      "football/live2|720p|live2_720p|default|h264|2500000|high|60|1280|720|"
                      "letterbox|aac|128000|first");
    put.headers["x-template-name"] = "Sports HD";
    auto applied = api.handle(put);
    EXPECT_EQ(applied.status, 200);
    EXPECT_FALSE(applied_rules.empty());

    auto deleted = api.handle(authed("DELETE", "/v1/transcoding/assignments/football%3Alive2"));
    EXPECT_EQ(deleted.status, 200);
    EXPECT_TRUE(removed);
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
