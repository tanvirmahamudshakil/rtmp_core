#include "rtmp_server/observability/audit_log.hpp"

#include <gtest/gtest.h>

namespace rtmp_server::observability {
namespace {

AuditEntry make_entry(std::string action) {
    AuditEntry entry;
    entry.timestamp = core::wall_now();
    entry.actor = "test";
    entry.action = std::move(action);
    entry.application = "live";
    entry.stream_name = "my-show";
    entry.success = true;
    return entry;
}

TEST(AuditLogTest, RecordedEntriesAppearInSnapshotInOrder) {
    AuditLog log;
    log.record(make_entry("create_stream"));
    log.record(make_entry("rotate_key"));

    auto entries = log.snapshot();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].action, "create_stream");
    EXPECT_EQ(entries[1].action, "rotate_key");
}

TEST(AuditLogTest, SizeReflectsEntryCount) {
    AuditLog log;
    EXPECT_EQ(log.size(), 0u);
    log.record(make_entry("create_stream"));
    EXPECT_EQ(log.size(), 1u);
}

TEST(AuditLogTest, BoundedCapacityDropsOldestEntries) {
    AuditLog log(/*max_entries=*/3);
    log.record(make_entry("a"));
    log.record(make_entry("b"));
    log.record(make_entry("c"));
    log.record(make_entry("d")); // should evict "a"

    auto entries = log.snapshot();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].action, "b");
    EXPECT_EQ(entries[1].action, "c");
    EXPECT_EQ(entries[2].action, "d");
}

} // namespace
} // namespace rtmp_server::observability
