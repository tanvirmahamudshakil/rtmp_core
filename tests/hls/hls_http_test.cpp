#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/control/hls_http_handler.hpp"
#include "rtmp_server/management/token.hpp"

using namespace rtmp_server;
using namespace rtmp_server::control;
using namespace std::chrono_literals;

namespace {

constexpr const char* kSecret = "phase6-test-signing-secret";
constexpr const char* kSessionA = "00112233445566778899aabbccddeeff";
constexpr const char* kSessionB = "ffeeddccbbaa99887766554433221100";

std::string session_query(std::string_view session, std::string_view master = "demo") {
    return "viewer_session=" + std::string(session) + "&viewer_stream=" + std::string(master);
}

hls::SegmentPtr make_segment(std::uint64_t sequence, std::size_t bytes = 1024) {
    auto segment = std::make_shared<hls::Segment>();
    segment->sequence = sequence;
    segment->name = "segment-" + std::to_string(sequence) + ".ts";
    segment->duration = 4000ms;
    segment->data = core::SharedBuffer::adopt(std::vector<std::byte>(bytes, std::byte{0x47}));
    return segment;
}

std::shared_ptr<hls::SegmentStore> populated_store(std::size_t count = 5) {
    auto store = std::make_shared<hls::SegmentStore>();
    for (std::uint64_t i = 0; i < count; ++i) store->add_segment(make_segment(i));
    return store;
}

HttpRequest get(const std::string& path, const std::string& query = {}) {
    HttpRequest request;
    request.method = "GET";
    request.path = path;
    request.query = query;
    request.client_ip = "127.0.0.1";
    return request;
}

std::string header_of(const HttpResponse& response, const std::string& name) {
    const auto it = response.headers.find(name);
    return it == response.headers.end() ? std::string{} : it->second;
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// --- Routing and content types --------------------------------------------

TEST(HlsHttpTest, ServesTheMediaPlaylistWithTheHlsContentType) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto response = handler.handle(get("/hls/live/demo/index.m3u8"));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, kContentTypeM3u8);
    EXPECT_EQ(response.content_type, "application/vnd.apple.mpegurl");
    EXPECT_TRUE(response.body.rfind("#EXTM3U", 0) == 0);
}

TEST(HlsHttpTest, ServesSegmentsWithTheMpegTsContentType) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto response = handler.handle(get("/hls/live/demo/segment-2.ts"));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "video/mp2t");
    EXPECT_EQ(response.payload_size(), 1024u);
    ASSERT_FALSE(response.shared_body.empty());
    EXPECT_EQ(static_cast<unsigned char>(response.shared_body.view()[0]), 0x47u);
}

TEST(HlsHttpTest, ServesTheMasterPlaylistWhenRenditionsAreDeclared) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    // Without renditions there is nothing to advertise.
    EXPECT_EQ(handler.handle(get("/hls/live/demo/master.m3u8")).status, 404);

    hls::Rendition rendition;
    rendition.uri = "index.m3u8";
    rendition.bandwidth = 2'500'000;
    rendition.codecs = "avc1.42C01E,mp4a.40.2";
    rendition.width = 1280;
    rendition.height = 720;
    handler.set_renditions("live", "demo", {rendition});

    auto response = handler.handle(get("/hls/live/demo/master.m3u8"));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, kContentTypeM3u8);
    EXPECT_NE(response.body.find("BANDWIDTH=2500000"), std::string::npos);
    EXPECT_NE(response.body.find("RESOLUTION=1280x720"), std::string::npos);
}

TEST(HlsHttpTest, FreshPlaylistOpenGetsUniquePlaybackSessionRedirect) {
    HlsHttpOptions options;
    options.enable_playback_sessions = true;
    options.playback_session_id_factory = [] { return std::string(kSessionA); };
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store());
    hls::Rendition rendition;
    rendition.uri = "../demo_480/index.m3u8";
    handler.set_renditions("live", "demo", {rendition});

    const auto response = handler.handle(get("/hls/live/demo/master.m3u8", "token=keep-me"));
    EXPECT_EQ(response.status, 302);
    EXPECT_EQ(header_of(response, "Cache-Control"), "private, no-store");
    EXPECT_EQ(header_of(response, "Location"),
              "/hls/live/demo/master.m3u8?token=keep-me&" + session_query(kSessionA));
}

TEST(HlsHttpTest, PlaybackSessionPropagatesFromMasterToVariantAndSegments) {
    HlsHttpOptions options;
    options.enable_playback_sessions = true;
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store());
    handler.register_stream("live", "demo_480", populated_store(3));
    hls::Rendition rendition;
    rendition.uri = "../demo_480/index.m3u8";
    handler.set_renditions("live", "demo", {rendition});
    const auto query = session_query(kSessionA);

    const auto master = handler.handle(get("/hls/live/demo/master.m3u8", query));
    ASSERT_EQ(master.status, 200);
    EXPECT_EQ(header_of(master, "Cache-Control"), "private, no-store");
    EXPECT_NE(master.body.find("../demo_480/index.m3u8?" + query), std::string::npos) << master.body;

    const auto media = handler.handle(get("/hls/live/demo_480/index.m3u8", query));
    ASSERT_EQ(media.status, 200);
    EXPECT_NE(media.body.find("segment-0.ts?" + query), std::string::npos) << media.body;
}

TEST(HlsHttpTest, InvalidCallerSessionIsReplacedInsteadOfTrusted) {
    HlsHttpOptions options;
    options.enable_playback_sessions = true;
    options.playback_session_id_factory = [] { return std::string(kSessionB); };
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store());

    const auto response = handler.handle(
        get("/hls/live/demo/index.m3u8", "viewer_session=forged&viewer_stream=fake&token=x"));
    ASSERT_EQ(response.status, 302);
    EXPECT_EQ(header_of(response, "Location"),
              "/hls/live/demo/index.m3u8?token=x&" + session_query(kSessionB));
}

TEST(HlsHttpTest, UnknownStreamAndUnknownSegmentReturn404) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    EXPECT_EQ(handler.handle(get("/hls/live/missing/index.m3u8")).status, 404);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/segment-999.ts")).status, 404);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/whatever.txt")).status, 404);
    EXPECT_EQ(handler.handle(get("/hls/too/few")).status, 404);
}

TEST(HlsHttpTest, DisabledStreamServesNoPlaylistsOrSegments) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    bool enabled = true;
    handler.set_stream_enabled_checker(
        [&](const std::string& application, const std::string& stream) {
            return enabled && application == "live" && stream == "demo";
        });

    // Enabled: normal service.
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 200);

    // Disabled: playlists and segments 404, as if the stream did not exist.
    enabled = false;
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 404);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/segment-2.ts")).status, 404);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/master.m3u8")).status, 404);

    // Re-enabled: service resumes.
    enabled = true;
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 200);
}

TEST(HlsHttpTest, EvictedSegment404IsNotCacheable) {
    // A CDN caching this 404 would keep serving it after the stream
    // recovers, so it must be explicitly no-store.
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto response = handler.handle(get("/hls/live/demo/segment-999.ts"));
    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(header_of(response, "Cache-Control"), "no-store");
}

TEST(HlsHttpTest, NonGetMethodsAreRejectedWithAllowHeader) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto request = get("/hls/live/demo/index.m3u8");
    request.method = "POST";
    auto response = handler.handle(request);
    EXPECT_EQ(response.status, 405);
    EXPECT_EQ(header_of(response, "Allow"), "GET, HEAD");
}

TEST(HlsHttpTest, PathTraversalAttemptsAreRejected) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    EXPECT_EQ(handler.handle(get("/hls/live/demo/../../../etc/passwd")).status, 404);
    EXPECT_EQ(handler.handle(get("/hls/../secrets/x.ts")).status, 404);
}

TEST(HlsHttpTest, RequestsOutsideThePrefixAreForwardedToTheNextHandler) {
    // Proves one HttpServer serves both HLS and the management API: no
    // second HTTP stack is introduced.
    HlsHttpHandler handler{HlsHttpOptions{}};
    bool forwarded = false;
    handler.set_next([&](const HttpRequest& request) {
        forwarded = true;
        EXPECT_EQ(request.path, "/v1/streams");
        return HttpResponse::json(200, "{\"ok\":true}");
    });

    auto response = handler.handle(get("/v1/streams"));
    EXPECT_TRUE(forwarded);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.content_type, "application/json");
}

TEST(HlsHttpTest, PrefixMatchIsBoundaryAwareAndDoesNotSwallowSimilarPaths) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    bool forwarded = false;
    handler.set_next([&](const HttpRequest&) {
        forwarded = true;
        return HttpResponse::json(200, "{}");
    });

    // "/hlsx/..." must NOT be treated as being under "/hls".
    // The response is intentionally unused: this asserts on `forwarded`,
    // i.e. that the request reached the fallback handler at all.
    (void)handler.handle(get("/hlsx/live/demo/index.m3u8"));
    EXPECT_TRUE(forwarded);
}

// --- Cache-Control (CDN behaviour) ----------------------------------------

TEST(HlsHttpTest, LivePlaylistIsNotCacheableButSegmentsAreImmutable) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    // The live playlist changes every segment; caching it stalls viewers.
    auto playlist = handler.handle(get("/hls/live/demo/index.m3u8"));
    EXPECT_EQ(header_of(playlist, "Cache-Control"), "no-cache, max-age=0");

    // Segments are uniquely named and never rewritten: safe to cache hard.
    auto segment = handler.handle(get("/hls/live/demo/segment-1.ts"));
    EXPECT_EQ(header_of(segment, "Cache-Control"), "public, max-age=31536000, immutable");
}

TEST(HlsHttpTest, CorsAndAcceptRangesHeadersArePresentForBrowserPlayers) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto response = handler.handle(get("/hls/live/demo/segment-0.ts"));
    EXPECT_EQ(header_of(response, "Access-Control-Allow-Origin"), "*");
    EXPECT_EQ(header_of(response, "Accept-Ranges"), "bytes");
}

TEST(HlsHttpTest, HeadReturnsHeadersAndAccurateContentLengthWithNoBody) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto request = get("/hls/live/demo/segment-0.ts");
    request.method = "HEAD";
    auto response = handler.handle(request);

    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.body.empty());
    EXPECT_EQ(header_of(response, "Content-Length"), "1024");
    EXPECT_EQ(response.content_type, "video/mp2t");
}

// --- Range requests -------------------------------------------------------

TEST(HlsHttpTest, ByteRangeRequestReturns206WithContentRange) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto request = get("/hls/live/demo/segment-0.ts");
    request.headers["range"] = "bytes=100-199";
    auto response = handler.handle(request);

    EXPECT_EQ(response.status, 206);
    EXPECT_EQ(response.payload_size(), 100u);
    EXPECT_EQ(header_of(response, "Content-Range"), "bytes 100-199/1024");
}

TEST(HlsHttpTest, OpenEndedAndSuffixRangesAreSupported) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto open_ended = get("/hls/live/demo/segment-0.ts");
    open_ended.headers["range"] = "bytes=1000-";
    auto r1 = handler.handle(open_ended);
    EXPECT_EQ(r1.status, 206);
    EXPECT_EQ(r1.payload_size(), 24u);
    EXPECT_EQ(header_of(r1, "Content-Range"), "bytes 1000-1023/1024");

    auto suffix = get("/hls/live/demo/segment-0.ts");
    suffix.headers["range"] = "bytes=-50";
    auto r2 = handler.handle(suffix);
    EXPECT_EQ(r2.status, 206);
    EXPECT_EQ(r2.payload_size(), 50u);
    EXPECT_EQ(header_of(r2, "Content-Range"), "bytes 974-1023/1024");
}

TEST(HlsHttpTest, UnsatisfiableRangeReturns416) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto request = get("/hls/live/demo/segment-0.ts");
    request.headers["range"] = "bytes=99999-100000";
    auto response = handler.handle(request);

    EXPECT_EQ(response.status, 416);
    EXPECT_EQ(header_of(response, "Content-Range"), "bytes */1024");
}

TEST(HlsHttpTest, MalformedRangeFallsBackTo416RatherThanServingGarbage) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    for (const char* bad : {"bytes=abc-def", "items=0-10", "bytes=", "bytes=5-3"}) {
        auto request = get("/hls/live/demo/segment-0.ts");
        request.headers["range"] = bad;
        auto response = handler.handle(request);
        EXPECT_EQ(response.status, 416) << "range: " << bad;
    }
}

// --- Playback token integration (Phase 5 parity) --------------------------

TEST(HlsHttpTest, TokenIsRequiredWhenConfiguredAndAValidOneIsAccepted) {
    HlsHttpOptions options;
    options.require_playback_token = true;
    options.token_signing_secret = kSecret;
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store());

    // No token at all.
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 403);

    const auto expires = now_unix() + 3600;
    const auto token = management::sign_token(kSecret, "live", "demo", expires);
    const auto query = "token=" + token + "&expires=" + std::to_string(expires);

    auto response = handler.handle(get("/hls/live/demo/index.m3u8", query));
    EXPECT_EQ(response.status, 200);
    // Segments are gated too, not just the playlist.
    EXPECT_EQ(handler.handle(get("/hls/live/demo/segment-0.ts", query)).status, 200);
}

TEST(HlsHttpTest, ExpiredAndForgedTokensAreRejected) {
    HlsHttpOptions options;
    options.require_playback_token = true;
    options.token_signing_secret = kSecret;
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store());

    const auto past = now_unix() - 60;
    const auto expired = management::sign_token(kSecret, "live", "demo", past);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8",
                                 "token=" + expired + "&expires=" + std::to_string(past)))
                  .status,
              403);

    const auto future = now_unix() + 3600;
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8",
                                 "token=deadbeef&expires=" + std::to_string(future)))
                  .status,
              403);

    // A token signed for a different stream must not work here (the stream
    // name is part of the signed claims).
    const auto other = management::sign_token(kSecret, "live", "other-stream", future);
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8",
                                 "token=" + other + "&expires=" + std::to_string(future)))
                  .status,
              403);
}

TEST(HlsHttpTest, AuthorizationFailureIsNotCacheable) {
    HlsHttpOptions options;
    options.require_playback_token = true;
    options.token_signing_secret = kSecret;
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store());

    auto response = handler.handle(get("/hls/live/demo/index.m3u8"));
    EXPECT_EQ(response.status, 403);
    EXPECT_EQ(header_of(response, "Cache-Control"), "no-store");
}

TEST(HlsHttpTest, PlaylistPropagatesTheTokenQueryOntoSegmentUris) {
    // Without this a token-gated stream would produce a playlist whose
    // segment URIs are all 403.
    HlsHttpOptions options;
    options.require_playback_token = true;
    options.token_signing_secret = kSecret;
    HlsHttpHandler handler{options};
    handler.register_stream("live", "demo", populated_store(3));

    const auto expires = now_unix() + 3600;
    const auto token = management::sign_token(kSecret, "live", "demo", expires);
    const auto query = "token=" + token + "&expires=" + std::to_string(expires);

    auto response = handler.handle(get("/hls/live/demo/index.m3u8", query));
    ASSERT_EQ(response.status, 200);
    EXPECT_NE(response.body.find("segment-0.ts?" + query), std::string::npos) << response.body;
    // Tag lines must not be decorated.
    EXPECT_EQ(response.body.find("#EXTM3U?"), std::string::npos);
    EXPECT_NE(response.body.find("#EXTM3U\n"), std::string::npos);
}

TEST(HlsHttpTest, TokenIsNotRequiredWhenTheFeatureIsDisabled) {
    HlsHttpHandler handler{HlsHttpOptions{}}; // require_playback_token defaults to false
    handler.register_stream("live", "demo", populated_store());
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 200);
}

// --- Lifecycle and concurrency --------------------------------------------

TEST(HlsHttpTest, UnregisteringAStreamStopsServingIt) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());
    ASSERT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 200);

    handler.unregister_stream("live", "demo");
    EXPECT_EQ(handler.handle(get("/hls/live/demo/index.m3u8")).status, 404);
    EXPECT_EQ(handler.find_stream("live", "demo"), nullptr);
}

TEST(HlsHttpTest, ManyConcurrentViewersFetchPlaylistsAndSegmentsSafely) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    auto store = std::make_shared<hls::SegmentStore>();
    handler.register_stream("live", "demo", store);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ok_responses{0};
    std::atomic<std::uint64_t> bad_responses{0};

    // The media thread keeps publishing while viewers read.
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < 300; ++i) {
            store->add_segment(make_segment(i));
            std::this_thread::yield();
        }
        stop = true;
    });

    std::vector<std::thread> viewers;
    for (int v = 0; v < 8; ++v) {
        viewers.emplace_back([&] {
            while (!stop.load()) {
                auto playlist = handler.handle(get("/hls/live/demo/index.m3u8"));
                if (playlist.status == 200 && playlist.body.rfind("#EXTM3U", 0) == 0) {
                    ok_responses++;
                } else {
                    bad_responses++;
                }
                auto segment = handler.handle(get("/hls/live/demo/segment-5.ts"));
                // 200 (still retained) or 404 (evicted) are both correct;
                // anything else, or a wrong-sized body, is not.
                if (segment.status == 200) {
                    if (segment.payload_size() != 1024) bad_responses++;
                } else if (segment.status != 404) {
                    bad_responses++;
                }
            }
        });
    }

    producer.join();
    for (auto& t : viewers) t.join();

    EXPECT_EQ(bad_responses.load(), 0u);
    EXPECT_GT(ok_responses.load(), 0u);
}

// --- Live delivery stats (per-stream bandwidth/viewer tracking) -----------

TEST(HlsHttpTest, LinkStatsAccumulateBytesAndDedupeByPlaybackSessionNotIp) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto request_a = get("/hls/live/demo/index.m3u8", session_query(kSessionA));
    request_a.client_ip = "10.0.0.1";
    auto first = handler.handle(request_a);
    ASSERT_EQ(first.status, 200);
    const auto first_bytes = first.payload_size();

    auto request_a_again = request_a; // same client, a second poll
    auto second = handler.handle(request_a_again);
    ASSERT_EQ(second.status, 200);

    // A second VLC on the exact same NAT/IP is still a second viewer.
    auto request_b = get("/hls/live/demo/index.m3u8", session_query(kSessionB));
    request_b.client_ip = "10.0.0.1";
    auto third = handler.handle(request_b);
    ASSERT_EQ(third.status, 200);

    const auto stats = handler.link_stats("live", "demo");
    EXPECT_EQ(stats.bytes_total, first_bytes + second.payload_size() + third.payload_size());
    EXPECT_EQ(stats.viewer_count, 2u);
}

TEST(HlsHttpTest, SamePlaybackSessionOnDifferentIpsStillCountsOnce) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto first = get("/hls/live/demo/index.m3u8", session_query(kSessionA));
    first.client_ip = "10.0.0.1";
    auto second = first;
    second.client_ip = "203.0.113.9";
    ASSERT_EQ(handler.handle(first).status, 200);
    ASSERT_EQ(handler.handle(second).status, 200);
    EXPECT_EQ(handler.link_stats("live", "demo").viewer_count, 1u);
}

TEST(HlsHttpTest, SegmentDeliveryAlsoCountsTowardsLinkStats) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());

    auto segment = handler.handle(get("/hls/live/demo/segment-0.ts", session_query(kSessionA)));
    ASSERT_EQ(segment.status, 200);

    const auto stats = handler.link_stats("live", "demo");
    EXPECT_EQ(stats.bytes_total, segment.payload_size());
    EXPECT_EQ(stats.viewer_count, 1u);
}

TEST(HlsHttpTest, MasterPlaylistRequestsDoNotCountTowardsLinkStats) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());
    hls::Rendition rendition;
    rendition.uri = "index.m3u8";
    handler.set_renditions("live", "demo", {rendition});

    ASSERT_EQ(handler.handle(get("/hls/live/demo/master.m3u8")).status, 200);

    // A master is fetched once per session, not on a recurring cadence like a
    // media playlist/segment, so counting it would misrepresent liveness.
    const auto stats = handler.link_stats("live", "demo");
    EXPECT_EQ(stats.bytes_total, 0u);
    EXPECT_EQ(stats.viewer_count, 0u);
}

TEST(HlsHttpTest, AggregateLinkStatsSumsAcrossEveryAdvertisedRendition) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "job-720p", populated_store());
    handler.register_stream("live", "job-480p", populated_store());

    // Matches the fixed "../" + output_stream + "/index.m3u8" shape every
    // renditions-ready callback builds (see source_job_manager.cpp / main.cpp).
    hls::Rendition high;
    high.uri = "../job-720p/index.m3u8";
    hls::Rendition low;
    low.uri = "../job-480p/index.m3u8";
    handler.set_renditions("live", "job", {high, low});

    auto request_720p = get("/hls/live/job-720p/index.m3u8", session_query(kSessionA, "job"));
    request_720p.client_ip = "10.0.0.1";
    auto response_720p = handler.handle(request_720p);
    ASSERT_EQ(response_720p.status, 200);

    auto request_480p = get("/hls/live/job-480p/index.m3u8", session_query(kSessionB, "job"));
    request_480p.client_ip = "10.0.0.2";
    auto response_480p = handler.handle(request_480p);
    ASSERT_EQ(response_480p.status, 200);

    const auto aggregate = handler.aggregate_link_stats("live", "job");
    EXPECT_EQ(aggregate.bytes_total, response_720p.payload_size() + response_480p.payload_size());
    EXPECT_EQ(aggregate.viewer_count, 2u);

    // The job's own pseudo-stream key never registered a store/segments, so
    // querying it directly (not through the aggregate) sees nothing.
    EXPECT_EQ(handler.link_stats("live", "job").bytes_total, 0u);
}

TEST(HlsHttpTest, AggregateLinkStatsDedupesOneSessionAcrossAbrRenditions) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "job-720p", populated_store());
    handler.register_stream("live", "job-480p", populated_store());
    hls::Rendition high;
    high.uri = "../job-720p/index.m3u8";
    hls::Rendition low;
    low.uri = "../job-480p/index.m3u8";
    handler.set_renditions("live", "job", {high, low});

    const auto query = session_query(kSessionA, "job");
    ASSERT_EQ(handler.handle(get("/hls/live/job-720p/index.m3u8", query)).status, 200);
    ASSERT_EQ(handler.handle(get("/hls/live/job-480p/index.m3u8", query)).status, 200);
    EXPECT_EQ(handler.aggregate_link_stats("live", "job").viewer_count, 1u);
}

TEST(HlsHttpTest, AggregateLinkStatsFallsBackToOwnStatsWithoutParseableRenditions) {
    HlsHttpHandler handler{HlsHttpOptions{}};
    handler.register_stream("live", "demo", populated_store());
    // A plain passthrough stream's own rendition points at itself, not at
    // "../other-stream/index.m3u8" — no separate rendition stream to sum.
    hls::Rendition rendition;
    rendition.uri = "index.m3u8";
    handler.set_renditions("live", "demo", {rendition});

    ASSERT_EQ(handler.handle(get("/hls/live/demo/index.m3u8", session_query(kSessionA))).status, 200);

    const auto aggregate = handler.aggregate_link_stats("live", "demo");
    EXPECT_GT(aggregate.bytes_total, 0u);
    EXPECT_EQ(aggregate.viewer_count, 1u);
}
