#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "rtmp_server/recording/retention.hpp"

namespace fs = std::filesystem;
using namespace rtmp_server::recording;
using namespace std::chrono_literals;

namespace {

const auto kNow = std::chrono::system_clock::now();

RecordingFile file(std::string name, std::uint64_t size, std::chrono::seconds age) {
    RecordingFile f;
    f.path = std::move(name);
    f.size_bytes = size;
    f.modified = kNow - age;
    return f;
}

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

} // namespace

TEST(RetentionPolicyTest, EmptyPolicyKeepsEverything) {
    std::vector<RecordingFile> files{file("a", 100, 10s), file("b", 100, 5000s)};
    auto plan = plan_retention(files, RetentionPolicy{}, kNow);
    EXPECT_TRUE(plan.to_delete.empty());
    EXPECT_EQ(plan.files_kept, 2u);
    EXPECT_EQ(plan.bytes_kept, 200u);
}

TEST(RetentionPolicyTest, AgeLimitDeletesOnlyOlderRecordings) {
    std::vector<RecordingFile> files{file("fresh", 10, 60s), file("stale", 10, 7200s)};
    RetentionPolicy policy;
    policy.max_age = 3600s;

    auto plan = plan_retention(files, policy, kNow);
    ASSERT_EQ(plan.to_delete.size(), 1u);
    EXPECT_EQ(plan.to_delete[0], "stale");
    EXPECT_EQ(plan.files_kept, 1u);
    EXPECT_EQ(plan.bytes_reclaimed, 10u);
}

TEST(RetentionPolicyTest, CountLimitEvictsOldestFirst) {
    std::vector<RecordingFile> files{file("oldest", 1, 300s), file("middle", 1, 200s),
                                     file("newest", 1, 100s)};
    RetentionPolicy policy;
    policy.max_files = 2;

    auto plan = plan_retention(files, policy, kNow);
    ASSERT_EQ(plan.to_delete.size(), 1u);
    EXPECT_EQ(plan.to_delete[0], "oldest");
    EXPECT_EQ(plan.files_kept, 2u);
}

TEST(RetentionPolicyTest, TotalSizeLimitEvictsOldestUntilWithinBudget) {
    std::vector<RecordingFile> files{file("a", 500, 400s), file("b", 500, 300s), file("c", 500, 200s)};
    RetentionPolicy policy;
    policy.max_total_bytes = 1000;

    auto plan = plan_retention(files, policy, kNow);
    ASSERT_EQ(plan.to_delete.size(), 1u);
    EXPECT_EQ(plan.to_delete[0], "a");
    EXPECT_EQ(plan.bytes_kept, 1000u);
}

TEST(RetentionPolicyTest, CombinedLimitsApplyTogetherWithoutDoubleCounting) {
    std::vector<RecordingFile> files{file("ancient", 100, 99999s), file("big1", 900, 300s),
                                     file("big2", 900, 200s), file("recent", 100, 10s)};
    RetentionPolicy policy;
    policy.max_age = 3600s;        // removes "ancient"
    policy.max_total_bytes = 1000; // then trims the remaining 1900 bytes
    policy.max_files = 3;

    auto plan = plan_retention(files, policy, kNow);
    EXPECT_TRUE(contains(plan.to_delete, "ancient"));
    EXPECT_TRUE(contains(plan.to_delete, "big1"));
    EXPECT_LE(plan.bytes_kept, 1000u);
    // Each deleted file is listed exactly once.
    auto sorted = plan.to_delete;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end());
    EXPECT_TRUE(contains(plan.to_delete, "big1"));
    EXPECT_FALSE(contains(plan.to_delete, "recent"));
}

TEST(RetentionApplyTest, DeletesFromDiskAndIgnoresNonRecordingFiles) {
    const auto dir = fs::temp_directory_path() / ("rtmp_retention_" + std::to_string(::getpid()));
    fs::create_directories(dir);

    auto write = [&](const std::string& name, std::size_t size) {
        std::ofstream out(dir / name, std::ios::binary);
        out << std::string(size, 'x');
    };
    write("one.flv", 100);
    write("two.flv", 100);
    write("three.flv", 100);
    write("notes.txt", 100);       // wrong suffix: must be untouched
    write("live.flv.part", 100);   // in-progress: must be untouched

    RetentionPolicy policy;
    policy.max_files = 1;

    auto plan = apply_retention(dir.string(), policy, ".flv");
    ASSERT_TRUE(plan.ok()) << plan.error().message();
    EXPECT_EQ(plan.value().files_kept, 1u);
    EXPECT_EQ(plan.value().to_delete.size(), 2u);

    EXPECT_TRUE(fs::exists(dir / "notes.txt"));
    EXPECT_TRUE(fs::exists(dir / "live.flv.part"));

    std::size_t remaining_flv = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".flv") ++remaining_flv;
    }
    EXPECT_EQ(remaining_flv, 1u);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(RetentionApplyTest, MissingDirectoryIsAnExplicitError) {
    auto plan = apply_retention("/definitely/not/a/directory/phase6", RetentionPolicy{}, ".flv");
    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code(), rtmp_server::core::ErrorCode::NotFound);
}
