#include <gtest/gtest.h>

#include "rtmp_server/control/dash_http_handler.hpp"

using namespace rtmp_server;
using namespace rtmp_server::dash;
using rtmp_server::control::DashHttpHandler;
using rtmp_server::control::DashHttpOptions;
using rtmp_server::control::HttpRequest;
using rtmp_server::control::HttpResponse;

namespace {

HttpRequest get(const std::string& path) {
    HttpRequest request;
    request.method = "GET";
    request.path = path;
    request.http_version = "HTTP/1.1";
    request.client_ip = "127.0.0.1";
    return request;
}

SegmentPtr make_segment(std::uint64_t number, std::size_t bytes = 200) {
    auto segment = std::make_shared<Segment>();
    segment->number = number;
    segment->name = "chunk-" + std::to_string(number) + ".m4s";
    segment->data = core::SharedBuffer::copy_from(std::vector<std::byte>(bytes, std::byte{0x42}));
    segment->duration = std::chrono::milliseconds(4000);
    segment->init_epoch = 1;
    return segment;
}

InitSegmentPtr make_init() {
    auto init = std::make_shared<InitSegment>();
    init->data = core::SharedBuffer::copy_from(std::vector<std::byte>(64, std::byte{0x11}));
    init->epoch = 1;
    return init;
}

Representation representation(const std::string& id) {
    Representation rep;
    rep.id = id;
    rep.bandwidth = 3'000'000;
    rep.codecs = "avc1.42001e,mp4a.40.2";
    rep.mime_type = "video/mp4";
    rep.init_template = "{rep}/init.mp4";
    rep.media_template = "{rep}/chunk-$Number$.m4s";
    return rep;
}

std::shared_ptr<SegmentStore> registered_store(DashHttpHandler& handler, const std::string& app,
                                               const std::string& stream, const std::string& rep) {
    auto store = std::make_shared<SegmentStore>();
    handler.register_representation(app, stream, rep, store);
    handler.set_representations(app, stream, {representation(rep)}, 90000, 360000);
    return store;
}

} // namespace

TEST(DashHttpTest, ManifestListsEveryRegisteredRepresentation) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0));

    const auto response = handler.handle(get("/dash/live/demo/manifest.mpd"));
    EXPECT_EQ(response.status, 200);
    EXPECT_STREQ(response.content_type.c_str(), rtmp_server::control::kContentTypeMpd);
    EXPECT_NE(response.body.find("id=\"main\""), std::string::npos);
    EXPECT_NE(response.body.find("initialization=\"main/init.mp4\""), std::string::npos);
    EXPECT_NE(response.body.find("startNumber=\"0\""), std::string::npos);
}

TEST(DashHttpTest, UnknownStreamIs404NotCacheable) {
    DashHttpHandler handler({});
    const auto response = handler.handle(get("/dash/live/missing/manifest.mpd"));
    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(response.headers.at("Cache-Control"), "no-store");
}

TEST(DashHttpTest, InitSegmentIsServedOnceAvailableAnd404BeforeThat) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");

    const auto before = handler.handle(get("/dash/live/demo/main/init.mp4"));
    EXPECT_EQ(before.status, 404);

    store->set_init_segment(make_init());
    const auto after = handler.handle(get("/dash/live/demo/main/init.mp4"));
    EXPECT_EQ(after.status, 200);
    EXPECT_EQ(after.payload_size(), 64u);
    EXPECT_STREQ(after.content_type.c_str(), rtmp_server::control::kContentTypeMp4);
}

TEST(DashHttpTest, MediaSegmentsAreServedByNameAndSupportRangeRequests) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0, 1000));

    const auto whole = handler.handle(get("/dash/live/demo/main/chunk-0.m4s"));
    EXPECT_EQ(whole.status, 200);
    EXPECT_EQ(whole.payload_size(), 1000u);

    HttpRequest ranged = get("/dash/live/demo/main/chunk-0.m4s");
    ranged.headers["range"] = "bytes=0-99";
    const auto sliced = handler.handle(ranged);
    EXPECT_EQ(sliced.status, 206);
    EXPECT_EQ(sliced.payload_size(), 100u);
    EXPECT_EQ(sliced.headers.at("Content-Range"), "bytes 0-99/1000");
}

TEST(DashHttpTest, UnknownRepresentationOrSegmentIs404) {
    DashHttpHandler handler({});
    registered_store(handler, "live", "demo", "main");

    EXPECT_EQ(handler.handle(get("/dash/live/demo/other-rep/init.mp4")).status, 404);
    EXPECT_EQ(handler.handle(get("/dash/live/demo/main/chunk-99.m4s")).status, 404);
}

TEST(DashHttpTest, DisabledStreamsServeNothing) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0));
    handler.set_stream_enabled_checker([](const std::string&, const std::string&) { return false; });

    EXPECT_EQ(handler.handle(get("/dash/live/demo/manifest.mpd")).status, 404);
}

TEST(DashHttpTest, UnregisteringAStreamStopsServingIt) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0));
    ASSERT_EQ(handler.handle(get("/dash/live/demo/manifest.mpd")).status, 200);

    handler.unregister_stream("live", "demo");
    EXPECT_EQ(handler.handle(get("/dash/live/demo/manifest.mpd")).status, 404);
}

TEST(DashHttpTest, EdgeFetchSecretGatesEveryRoute) {
    DashHttpOptions options;
    options.edge_fetch_secret = "shared-edge-secret-value";
    DashHttpHandler handler(options);
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0));

    const auto unauthorized = handler.handle(get("/dash/live/demo/manifest.mpd"));
    EXPECT_EQ(unauthorized.status, 403);
    EXPECT_EQ(handler.stats().edge_unauthorized, 1u);

    auto request = get("/dash/live/demo/manifest.mpd");
    request.headers["x-edge-token"] = "shared-edge-secret-value";
    EXPECT_EQ(handler.handle(request).status, 200);
}

TEST(DashHttpTest, RequestsOutsideThePrefixChainToTheNextHandler) {
    DashHttpHandler handler({});
    bool reached_next = false;
    handler.set_next([&reached_next](const HttpRequest&) {
        reached_next = true;
        return HttpResponse::json(200, "{}");
    });
    handler.handle(get("/other/path"));
    EXPECT_TRUE(reached_next);
}

TEST(DashHttpTest, HeadCarriesLengthButNoBody) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0, 500));

    auto request = get("/dash/live/demo/main/chunk-0.m4s");
    request.method = "HEAD";
    const auto response = handler.handle(request);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.headers.at("Content-Length"), "500");
    EXPECT_EQ(response.payload_size(), 0u);
}

TEST(DashHttpTest, StatsCountRequestsByKind) {
    DashHttpHandler handler({});
    auto store = registered_store(handler, "live", "demo", "main");
    store->add_segment(make_segment(0));
    store->set_init_segment(make_init());

    handler.handle(get("/dash/live/demo/manifest.mpd"));
    handler.handle(get("/dash/live/demo/main/chunk-0.m4s"));
    handler.handle(get("/dash/live/demo/main/chunk-99.m4s"));

    const auto stats = handler.stats();
    EXPECT_EQ(stats.manifest_requests, 1u);
    // Hit and miss both count as a segment request; a segment miss (unlike an
    // unroutable resource) is not counted in not_found, matching
    // HlsHttpHandler's own stats semantics.
    EXPECT_EQ(stats.segment_requests, 2u);
    EXPECT_EQ(stats.not_found, 0u);
}
