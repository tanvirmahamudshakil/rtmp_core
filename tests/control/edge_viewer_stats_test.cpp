#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "rtmp_server/control/edge_viewer_stats.hpp"

namespace {

using namespace std::chrono_literals;
using rtmp_server::control::EdgeViewerStats;
using rtmp_server::control::parse_edge_viewer_stats;

// The exact shape deploy/viewer-estimator/viewer_estimator.py writes:
// json.dump with separators=(",", ":"), so no whitespace anywhere.
std::string document(double generated_at) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f", generated_at);
    return std::string(R"({"generated_at":)") + buffer +
           R"(,"window_seconds":20,"bitrate_window_seconds":10,"identity":"playback_session",)"
           R"("viewers":{"kk/KK_480p":1200,"kk/KK_720p":340,"live/main":7},)"
           R"("bytes_total":{"kk/KK_480p":900,"kk/KK_720p":100},)"
           R"("bitrate_bps":{"kk/KK_480p":2500000},)"
           R"("totals":{"viewers":1500,"bytes_total":1000,"bitrate_bps":2500000}})";
}

std::chrono::system_clock::time_point at(double unix_seconds) {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(unix_seconds)));
}

TEST(EdgeViewerStatsTest, ParsesPerLinkViewerCounts) {
    const auto stats = parse_edge_viewer_stats(document(1000), at(1001));
    ASSERT_TRUE(stats.fresh);
    EXPECT_EQ(stats.viewers.at("kk/KK_480p"), 1200U);
    EXPECT_EQ(stats.viewers.at("kk/KK_720p"), 340U);
    EXPECT_EQ(stats.viewers.at("live/main"), 7U);
    EXPECT_EQ(stats.bytes_total.at("kk/KK_480p"), 900U);
    EXPECT_EQ(stats.bitrate_bps.at("kk/KK_480p"), 2500000U);
}

// The estimator's own totals are a union over sessions, so they must be used
// verbatim rather than re-derived by summing the per-link map: a player
// mid-ABR-switch appears under two rendition keys at once.
TEST(EdgeViewerStatsTest, UsesTheEstimatorsDeduplicatedTotals) {
    const auto stats = parse_edge_viewer_stats(document(1000), at(1000));
    ASSERT_TRUE(stats.fresh);
    EXPECT_EQ(stats.total_viewers, 1500U); // not 1200 + 340 + 7
    EXPECT_EQ(stats.total_bitrate_bps, 2500000U);
}

TEST(EdgeViewerStatsTest, RejectsAReadingOlderThanItsOwnWindow) {
    // window_seconds 20 + 10s grace, floored at 30s.
    EXPECT_TRUE(parse_edge_viewer_stats(document(1000), at(1029)).fresh);
    EXPECT_FALSE(parse_edge_viewer_stats(document(1000), at(1100)).fresh);
}

// A clock skew in either direction is equally untrustworthy: a file stamped
// in the future says nothing about who is watching now.
TEST(EdgeViewerStatsTest, RejectsAReadingStampedFarInTheFuture) {
    EXPECT_FALSE(parse_edge_viewer_stats(document(2000), at(1000)).fresh);
}

TEST(EdgeViewerStatsTest, TreatsMalformedDocumentsAsUnavailableNotAsZeroViewers) {
    for (const auto* text : {"", "{", "not json", R"({"viewers":{"a/b":})",
                             R"({"window_seconds":20})" /* no generated_at */}) {
        const auto stats = parse_edge_viewer_stats(text, at(1000));
        EXPECT_FALSE(stats.fresh) << text;
        EXPECT_TRUE(stats.viewers.empty()) << text;
        EXPECT_EQ(stats.total_viewers, 0U) << text;
    }
}

// A negative or non-finite count is dropped rather than clamped: a corrupted
// field must not be able to invent or erase viewers.
TEST(EdgeViewerStatsTest, DropsImplausibleCounts) {
    const auto stats = parse_edge_viewer_stats(
        R"({"generated_at":1000,"window_seconds":20,"viewers":{"a/b":-5,"a/c":9}})", at(1000));
    ASSERT_TRUE(stats.fresh);
    EXPECT_FALSE(stats.viewers.contains("a/b"));
    EXPECT_EQ(stats.viewers.at("a/c"), 9U);
}

// Python's json.dump escapes non-ASCII by default, and stream names are only
// constrained to exclude '/' and control characters.
TEST(EdgeViewerStatsTest, DecodesEscapedStreamNames) {
    const auto stats = parse_edge_viewer_stats(
        R"({"generated_at":1000,"viewers":{"kk/বাংলা":3}})", at(1000));
    ASSERT_TRUE(stats.fresh);
    EXPECT_EQ(stats.viewers.at("kk/বাংলা"), 3U);
}

TEST(EdgeViewerStatsTest, MissingFileReportsUnavailableRatherThanZero) {
    EdgeViewerStats::Options options;
    options.path = "/nonexistent/streamforge/viewer_estimate.json";
    EdgeViewerStats stats(options);
    EXPECT_FALSE(stats.snapshot().fresh);
    EXPECT_EQ(stats.viewers_for("kk", "KK_480p"), 0U);
}

TEST(EdgeViewerStatsTest, SumsAcrossARenditionLadder) {
    // A fixed name in the test's working directory: std::tmpnam is
    // deprecated and this file is written and removed by this test alone.
    const std::string path = "edge-viewer-stats-test-estimate.json";
    {
        std::ofstream out(path, std::ios::binary);
        const auto now = std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        out << document(now);
    }
    EdgeViewerStats::Options options;
    options.path = path;
    EdgeViewerStats stats(options);

    ASSERT_TRUE(stats.snapshot().fresh);
    EXPECT_EQ(stats.viewers_for("kk", "KK_480p"), 1200U);
    const std::vector<std::string> ladder = {"KK", "KK_480p", "KK_720p"};
    EXPECT_EQ(stats.viewers_for_any("kk", ladder), 1540U);
    EXPECT_EQ(stats.bytes_for_any("kk", ladder), 1000U);
    // An application that shares a rendition name with another must not
    // absorb its numbers.
    EXPECT_EQ(stats.viewers_for("other", "KK_480p"), 0U);
    std::remove(path.c_str());
}

} // namespace
