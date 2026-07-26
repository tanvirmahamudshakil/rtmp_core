#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::recording {

// Configurable recording retention (docs/v2_promot.md Phase 6 task 8).
//
// A zero value disables that individual limit; limits combine (a file is
// deleted if ANY enabled limit says it should go). Deletion always removes
// the OLDEST recordings first, so the most recent material survives longest.
struct RetentionPolicy {
    // Delete recordings older than this. 0 = no age limit.
    std::chrono::seconds max_age{0};
    // Keep at most this many recordings. 0 = no count limit.
    std::size_t max_files = 0;
    // Keep at most this many bytes in total. 0 = no size limit.
    std::uint64_t max_total_bytes = 0;
};

// One recording file considered by the policy.
struct RecordingFile {
    std::string path;
    std::uint64_t size_bytes = 0;
    std::chrono::system_clock::time_point modified{};
};

struct RetentionPlan {
    std::vector<std::string> to_delete;
    std::uint64_t bytes_reclaimed = 0;
    std::size_t files_kept = 0;
    std::uint64_t bytes_kept = 0;
};

// Pure decision function: given the current set of recordings, decide which
// to delete. Deliberately separated from any filesystem access so the policy
// is exhaustively unit-testable with no disk (same seam style as the
// Recorder/FileSink split). `files` need not be sorted.
[[nodiscard]] RetentionPlan plan_retention(std::vector<RecordingFile> files, const RetentionPolicy& policy,
                                           std::chrono::system_clock::time_point now);

// Scans `directory` for files ending in `suffix`, applies plan_retention and
// unlinks the selected files.
//
// This performs blocking filesystem I/O (directory scan + unlink) and MUST
// NOT be called from an RTMP event-loop/command thread — run it from a
// maintenance thread or a management-API worker (docs/v2_promot.md 3.6).
// In-progress recordings (".part" files) are never considered.
[[nodiscard]] core::Result<RetentionPlan> apply_retention(const std::string& directory,
                                                          const RetentionPolicy& policy,
                                                          std::string_view suffix = ".flv");

} // namespace rtmp_server::recording
