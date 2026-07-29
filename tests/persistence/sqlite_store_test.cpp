#include "rtmp_server/persistence/sqlite_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace rtmp_server::persistence {
namespace {

std::unique_ptr<SqliteStore> open_memory_store() {
    auto result = SqliteStore::open(":memory:");
    EXPECT_TRUE(result.ok());
    return std::move(result.value());
}

TEST(SqliteStoreTest, OpenCreatesSchemaAndStartsEmpty) {
    auto store = open_memory_store();
    ASSERT_NE(store, nullptr);

    auto apps = store->load_applications();
    ASSERT_TRUE(apps.ok());
    EXPECT_TRUE(apps.value().empty());

    auto streams = store->load_streams();
    ASSERT_TRUE(streams.ok());
    EXPECT_TRUE(streams.value().empty());
}

TEST(SqliteStoreTest, UpsertApplicationInsertsThenUpdatesInPlace) {
    auto store = open_memory_store();

    ASSERT_TRUE(store->upsert_application(ApplicationRow{"live", true}).ok());
    auto apps = store->load_applications().value();
    ASSERT_EQ(apps.size(), 1u);
    EXPECT_EQ(apps[0].name, "live");
    EXPECT_TRUE(apps[0].enabled);

    ASSERT_TRUE(store->upsert_application(ApplicationRow{"live", false}).ok());
    apps = store->load_applications().value();
    ASSERT_EQ(apps.size(), 1u); // still one row, not a duplicate
    EXPECT_FALSE(apps[0].enabled);
}

TEST(SqliteStoreTest, DeleteApplicationRemovesIt) {
    auto store = open_memory_store();
    ASSERT_TRUE(store->upsert_application(ApplicationRow{"live", true}).ok());
    ASSERT_TRUE(store->delete_application("live").ok());
    EXPECT_TRUE(store->load_applications().value().empty());
}

TEST(SqliteStoreTest, UpsertStreamRoundTripsAllFields) {
    auto store = open_memory_store();
    StreamRow row;
    row.application = "live";
    row.name = "my-show";
    row.key_hash = "deadbeef";
    row.enabled = true;
    row.recording_enabled = true;
    row.created_at_unix = 1735689600;

    ASSERT_TRUE(store->upsert_stream(row).ok());
    auto streams = store->load_streams().value();
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0].application, "live");
    EXPECT_EQ(streams[0].name, "my-show");
    EXPECT_EQ(streams[0].key_hash, "deadbeef");
    EXPECT_TRUE(streams[0].enabled);
    EXPECT_TRUE(streams[0].recording_enabled);
    EXPECT_EQ(streams[0].created_at_unix, 1735689600);
}

TEST(SqliteStoreTest, UpsertStreamOnSameApplicationAndNameUpdatesInPlace) {
    auto store = open_memory_store();
    StreamRow row;
    row.application = "live";
    row.name = "my-show";
    row.key_hash = "old-hash";
    row.enabled = true;

    ASSERT_TRUE(store->upsert_stream(row).ok());
    row.key_hash = "new-hash";
    row.enabled = false;
    ASSERT_TRUE(store->upsert_stream(row).ok());

    auto streams = store->load_streams().value();
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0].key_hash, "new-hash");
    EXPECT_FALSE(streams[0].enabled);
}

TEST(SqliteStoreTest, DeleteStreamRemovesOnlyThatRow) {
    auto store = open_memory_store();
    ASSERT_TRUE(store->upsert_stream(StreamRow{"live", "a", "hash-a", true, false, 0}).ok());
    ASSERT_TRUE(store->upsert_stream(StreamRow{"live", "b", "hash-b", true, false, 0}).ok());

    ASSERT_TRUE(store->delete_stream("live", "a").ok());

    auto streams = store->load_streams().value();
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0].name, "b");
}

TEST(SqliteStoreTest, TranscodingAssignmentRoundTripsUpdatesAndDeletes) {
    auto store = open_memory_store();
    TranscodingAssignmentRow row{
        "football",
        "live2",
        "Sports ladder",
        "football/live2|720p|live2_720p|default|h264|2500000|high|60|1280|720|"
        "letterbox|aac|128000|first",
    };
    ASSERT_TRUE(store->upsert_transcoding_assignment(row).ok());
    auto assignments = store->load_transcoding_assignments().value();
    ASSERT_EQ(assignments.size(), 1U);
    EXPECT_EQ(assignments.front().template_name, "Sports ladder");
    EXPECT_EQ(assignments.front().rules, row.rules);

    row.template_name = "Updated ladder";
    ASSERT_TRUE(store->upsert_transcoding_assignment(row).ok());
    assignments = store->load_transcoding_assignments().value();
    ASSERT_EQ(assignments.size(), 1U);
    EXPECT_EQ(assignments.front().template_name, "Updated ladder");

    ASSERT_TRUE(store->delete_transcoding_assignment("football", "live2").ok());
    EXPECT_TRUE(store->load_transcoding_assignments().value().empty());
}

TEST(SqliteStoreTest, DataSurvivesAcrossACloseAndReopenOfTheSameFile) {
    // Uses a real temp file (not :memory:) to prove data is actually
    // persisted to disk, not just held in the sqlite3 connection's cache.
    std::string path = std::filesystem::temp_directory_path() / "rtmp_server_sqlite_store_test.db";
    std::filesystem::remove(path);

    {
        auto store = SqliteStore::open(path).value();
        ASSERT_TRUE(store->upsert_application(ApplicationRow{"live", true}).ok());
        ASSERT_TRUE(store->upsert_stream(StreamRow{"live", "my-show", "hash", true, false, 42}).ok());
    } // store destructs, connection closes

    {
        auto store = SqliteStore::open(path).value();
        auto apps = store->load_applications().value();
        ASSERT_EQ(apps.size(), 1u);
        EXPECT_EQ(apps[0].name, "live");

        auto streams = store->load_streams().value();
        ASSERT_EQ(streams.size(), 1u);
        EXPECT_EQ(streams[0].name, "my-show");
        EXPECT_EQ(streams[0].created_at_unix, 42);
    }

    std::filesystem::remove(path);
}

} // namespace
} // namespace rtmp_server::persistence
