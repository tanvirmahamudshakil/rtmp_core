#include <gtest/gtest.h>

#ifdef RTMP_NATIVE_TRANSCODE

#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/transcoding/native/ingest_transcoder.hpp"

namespace {

using rtmp_server::core::ErrorCode;
using rtmp_server::transcoding::native::IngestTranscodeManager;
using rtmp_server::transcoding::native::IngestTranscodeOptions;

// Records what a manager persists and hands back, so a reload can be exercised
// without SQLite.
class FakeStore final : public rtmp_server::persistence::Store {
public:
    rtmp_server::core::Result<void> upsert_application(
        const rtmp_server::persistence::ApplicationRow&) override {
        return {};
    }
    rtmp_server::core::Result<void> delete_application(std::string_view) override { return {}; }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::ApplicationRow>>
    load_applications() override {
        return std::vector<rtmp_server::persistence::ApplicationRow>{};
    }
    rtmp_server::core::Result<void> upsert_stream(
        const rtmp_server::persistence::StreamRow&) override {
        return {};
    }
    rtmp_server::core::Result<void> delete_stream(std::string_view, std::string_view) override {
        return {};
    }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::StreamRow>> load_streams()
        override {
        return std::vector<rtmp_server::persistence::StreamRow>{};
    }

    rtmp_server::core::Result<void> upsert_transcoding_assignment(
        const rtmp_server::persistence::TranscodingAssignmentRow& row) override {
        assignments[row.application + "/" + row.source_stream] = row;
        return {};
    }
    rtmp_server::core::Result<void> delete_transcoding_assignment(
        std::string_view application, std::string_view source_stream) override {
        assignments.erase(std::string(application) + "/" + std::string(source_stream));
        return {};
    }
    rtmp_server::core::Result<std::vector<rtmp_server::persistence::TranscodingAssignmentRow>>
    load_transcoding_assignments() override {
        std::vector<rtmp_server::persistence::TranscodingAssignmentRow> rows;
        for (const auto& [key, row] : assignments) rows.push_back(row);
        return rows;
    }

    std::unordered_map<std::string, rtmp_server::persistence::TranscodingAssignmentRow> assignments;
};

// Two rungs off one published stream, in the pipe-delimited preset shape the
// admin panel and the source-job API already post.
constexpr const char* kLadder =
    "live/main|hd|main_720p|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first|HD\n"
    "live/main|sd|main_480p|default|h264|900000|main|60|854|480|letterbox|aac|96000|first|SD\n";

TEST(IngestTranscodeManagerTest, StoresAnAssignmentAndDescribesItsLadder) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});

    auto created = manager.upsert("live", "main", "ladder", kLadder);
    ASSERT_TRUE(created) << created.error().message();
    EXPECT_NE(created.value().find(R"("master_hls_path":"/hls/live/main/master.m3u8")"),
              std::string::npos);
    EXPECT_NE(created.value().find(R"("stream":"main_720p")"), std::string::npos);
    EXPECT_NE(created.value().find(R"("stream":"main_480p")"), std::string::npos);

    const auto assignment = manager.find("live", "main");
    ASSERT_TRUE(assignment.has_value());
    ASSERT_EQ(assignment->renditions.size(), 2u);
    EXPECT_EQ(assignment->template_name, "ladder");
    EXPECT_FALSE(assignment->active); // configuration only until a publisher arrives
    EXPECT_EQ(store.assignments.size(), 1u);
}

// The publisher's own name already serves the untranscoded playlist; a rung
// claiming it would replace that registration and make the source rendition
// unreachable.
TEST(IngestTranscodeManagerTest, RejectsARungThatClaimsTheSourceStreamName) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});

    auto result = manager.upsert(
        "live", "main", "bad",
        "live/other|hd|main|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first\n");
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("source stream name"), std::string::npos);
    EXPECT_TRUE(store.assignments.empty());
}

TEST(IngestTranscodeManagerTest, RejectsTwoRungsSharingOneOutgoingName) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});

    auto result = manager.upsert(
        "live", "main", "bad",
        "live/main|hd|main_720p|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first\n"
        "live/main|sd|main_720p|default|h264|900000|main|60|854|480|letterbox|aac|96000|first\n");
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("share one outgoing stream name"), std::string::npos);
}

TEST(IngestTranscodeManagerTest, RejectsRulesItCannotParse) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});
    EXPECT_FALSE(manager.upsert("live", "main", "bad", "not a preset line"));
    EXPECT_FALSE(manager.upsert("live", "main", "bad", ""));
    EXPECT_FALSE(manager.upsert("", "main", "bad", kLadder));
}

TEST(IngestTranscodeManagerTest, RemovingAnAssignmentClearsItFromTheStore) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});
    ASSERT_TRUE(manager.upsert("live", "main", "ladder", kLadder));

    ASSERT_TRUE(manager.remove("live", "main"));
    EXPECT_FALSE(manager.find("live", "main").has_value());
    EXPECT_TRUE(store.assignments.empty());

    auto missing = manager.remove("live", "main");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(IngestTranscodeManagerTest, RebuildsPersistedAssignmentsOnRestart) {
    FakeStore store;
    {
        IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});
        ASSERT_TRUE(manager.upsert("live", "main", "ladder", kLadder));
    }

    IngestTranscodeManager restarted({}, &store, IngestTranscodeOptions{});
    restarted.load_from_store();
    const auto assignment = restarted.find("live", "main");
    ASSERT_TRUE(assignment.has_value());
    EXPECT_EQ(assignment->renditions.size(), 2u);
    EXPECT_EQ(assignment->master_hls_path, "/hls/live/main/master.m3u8");
}

// One bad stored blob must not take the whole server's assignments down with
// it, so the row is skipped and every other assignment still loads.
TEST(IngestTranscodeManagerTest, SkipsAStoredRowThatNoLongerParses) {
    FakeStore store;
    rtmp_server::persistence::TranscodingAssignmentRow broken;
    broken.application = "live";
    broken.source_stream = "broken";
    broken.rules = "this is not a preset line";
    store.assignments["live/broken"] = broken;

    rtmp_server::persistence::TranscodingAssignmentRow good;
    good.application = "live";
    good.source_stream = "main";
    good.template_name = "ladder";
    good.rules = kLadder;
    store.assignments["live/main"] = good;

    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});
    manager.load_from_store();
    EXPECT_FALSE(manager.find("live", "broken").has_value());
    EXPECT_TRUE(manager.find("live", "main").has_value());
}

// An unassigned publish must keep exactly the passthrough behaviour it had.
TEST(IngestTranscodeManagerTest, CreatesNoSinkForAnUnassignedStream) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});
    ASSERT_TRUE(manager.upsert("live", "main", "ladder", kLadder));

    EXPECT_EQ(manager.create_sink("live", "other"), nullptr);
    EXPECT_EQ(manager.create_sink("other", "main"), nullptr);
    EXPECT_EQ(manager.active_ladder_count(), 0u);
}

TEST(IngestTranscodeManagerTest, ListsOnlyTheRequestedApplication) {
    FakeStore store;
    IngestTranscodeManager manager({}, &store, IngestTranscodeOptions{});
    ASSERT_TRUE(manager.upsert("live", "main", "ladder", kLadder));
    ASSERT_TRUE(manager.upsert(
        "sports", "match", "ladder",
        "sports/match|hd|match_720p|default|h264|2500000|high|60|1280|720|letterbox|aac|128000|first\n"));

    const auto live = manager.list("live");
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live.front().source_stream, "main");
    EXPECT_EQ(manager.list("").size(), 2u);
}

} // namespace

#endif // RTMP_NATIVE_TRANSCODE
