#include "rtmp_server/management/stream_manager.hpp"

#include <gtest/gtest.h>

#include "rtmp_server/observability/audit_log.hpp"
#include "rtmp_server/observability/metrics.hpp"
#include "rtmp_server/persistence/sqlite_store.hpp"

namespace rtmp_server::management {
namespace {

StreamManager::Options test_options() {
    StreamManager::Options options;
    options.public_hostname = "stream.example.com";
    options.rtmp_port = 1935;
    options.token_signing_secret = "test-signing-secret";
    return options;
}

TEST(StreamManagerPersistenceTest, StateSurvivesReloadThroughASharedSqliteStore) {
    auto store = persistence::SqliteStore::open(":memory:").value();

    std::string raw_key;
    {
        StreamManager manager(test_options());
        manager.set_store(store.get());
        ASSERT_TRUE(manager.create_application("live").ok());
        auto created = manager.create_stream("live", "my-show", /*recording_enabled=*/true);
        ASSERT_TRUE(created.ok());
        raw_key = created.value().stream_key;
    } // manager destructed — only the store's rows survive

    StreamManager reloaded(test_options());
    reloaded.set_store(store.get());
    ASSERT_TRUE(reloaded.load_from_store().ok());

    auto stream = reloaded.find_stream("live", "my-show");
    ASSERT_TRUE(stream.has_value());
    EXPECT_TRUE(stream->enabled);
    EXPECT_TRUE(stream->recording_enabled);
    EXPECT_TRUE(reloaded.validate_publish_key("live", raw_key));
}

TEST(StreamManagerPersistenceTest, KeyRotationPersistsTheNewHash) {
    auto store = persistence::SqliteStore::open(":memory:").value();

    StreamManager manager(test_options());
    manager.set_store(store.get());
    ASSERT_TRUE(manager.create_application("live").ok());
    manager.create_stream("live", "my-show");
    auto rotated = manager.rotate_key("live", "my-show");
    ASSERT_TRUE(rotated.ok());

    StreamManager reloaded(test_options());
    reloaded.set_store(store.get());
    ASSERT_TRUE(reloaded.load_from_store().ok());
    EXPECT_TRUE(reloaded.validate_publish_key("live", rotated.value()));
}

TEST(StreamManagerPersistenceTest, MutationsAreRecordedToTheAuditLog) {
    observability::AuditLog audit_log;
    StreamManager manager(test_options());
    manager.set_audit_log(&audit_log);

    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());
    ASSERT_TRUE(manager.set_enabled("live", "my-show", false).ok());

    auto entries = audit_log.snapshot();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].action, "create_application");
    EXPECT_EQ(entries[1].action, "create_stream");
    EXPECT_EQ(entries[2].action, "disable_stream");
    for (const auto& entry : entries) EXPECT_TRUE(entry.success);
}

TEST(StreamManagerPersistenceTest, FailedDisconnectIsRecordedAsAnUnsuccessfulAuditEntry) {
    observability::AuditLog audit_log;
    StreamManager manager(test_options());
    manager.set_audit_log(&audit_log);
    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "my-show").ok());

    protocol::commands::StreamRegistry registry; // empty — nothing published
    auto result = manager.disconnect_publisher("live", "my-show", registry);
    ASSERT_FALSE(result.ok());

    auto entries = audit_log.snapshot();
    ASSERT_EQ(entries.back().action, "disconnect_publisher");
    EXPECT_FALSE(entries.back().success);
}

TEST(StreamManagerPersistenceTest, MutationsIncrementMatchingMetricsCounters) {
    observability::Metrics metrics;
    StreamManager manager(test_options());
    manager.set_metrics(&metrics);

    ASSERT_TRUE(manager.create_application("live").ok());
    ASSERT_TRUE(manager.create_stream("live", "a").ok());
    ASSERT_TRUE(manager.create_stream("live", "b").ok());

    EXPECT_EQ(metrics.counter("management_create_application_total"), 1u);
    EXPECT_EQ(metrics.counter("management_create_stream_total"), 2u);
    EXPECT_EQ(metrics.counter("management_rotate_key_total"), 0u);

    // Phase 7: the old "management.<action>_total" spelling used a '.', which
    // is not a legal Prometheus metric name character. The registry now
    // rejects such names outright rather than exporting an unscrapeable
    // series, so the old spelling must read back as absent.
    EXPECT_EQ(metrics.counter("management.create_stream_total"), 0u);
}

} // namespace
} // namespace rtmp_server::management
