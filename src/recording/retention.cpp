#include "rtmp_server/recording/retention.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <sys/stat.h>

namespace rtmp_server::recording {

namespace fs = std::filesystem;

RetentionPlan plan_retention(std::vector<RecordingFile> files, const RetentionPolicy& policy,
                             std::chrono::system_clock::time_point now) {
    RetentionPlan plan;

    // Oldest first: every limit below evicts from the front.
    std::sort(files.begin(), files.end(),
              [](const RecordingFile& a, const RecordingFile& b) { return a.modified < b.modified; });

    std::vector<bool> deleted(files.size(), false);

    auto drop = [&](std::size_t i) {
        if (deleted[i]) return;
        deleted[i] = true;
        plan.to_delete.push_back(files[i].path);
        plan.bytes_reclaimed += files[i].size_bytes;
    };

    // 1. Age limit.
    if (policy.max_age.count() > 0) {
        const auto cutoff = now - policy.max_age;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].modified < cutoff) drop(i);
        }
    }

    auto surviving = [&] {
        std::size_t n = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (!deleted[i]) ++n;
        }
        return n;
    };

    // 2. Count limit — evict oldest survivors until within budget.
    if (policy.max_files > 0) {
        for (std::size_t i = 0; i < files.size() && surviving() > policy.max_files; ++i) {
            drop(i);
        }
    }

    // 3. Total-size limit — same, by bytes.
    if (policy.max_total_bytes > 0) {
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (!deleted[i]) total += files[i].size_bytes;
        }
        for (std::size_t i = 0; i < files.size() && total > policy.max_total_bytes; ++i) {
            if (deleted[i]) continue;
            total -= files[i].size_bytes;
            drop(i);
        }
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        if (deleted[i]) continue;
        plan.files_kept += 1;
        plan.bytes_kept += files[i].size_bytes;
    }
    return plan;
}

core::Result<RetentionPlan> apply_retention(const std::string& directory, const RetentionPolicy& policy,
                                            std::string_view suffix) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        return core::Error(core::ErrorCode::NotFound, core::ErrorCategory::Storage,
                           "recording directory not found: " + directory);
    }

    std::vector<RecordingFile> files;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string path = it->path().string();
        if (suffix.size() > path.size()) continue;
        if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

        // POSIX stat rather than fs::last_write_time + clock_cast: the
        // file_clock -> system_clock conversion is not reliably available
        // across the libstdc++/libc++ versions this project targets, and
        // st_mtime is already exactly the system-clock epoch value we want.
        struct ::stat st {};
        if (::stat(path.c_str(), &st) != 0) continue;

        RecordingFile file;
        file.path = path;
        file.size_bytes = static_cast<std::uint64_t>(st.st_size);
        file.modified = std::chrono::system_clock::from_time_t(st.st_mtime);
        files.push_back(std::move(file));
    }
    if (ec) {
        return core::Error(core::ErrorCode::StorageUnavailable, core::ErrorCategory::Storage,
                           "failed to scan " + directory + ": " + ec.message());
    }

    auto plan = plan_retention(std::move(files), policy, std::chrono::system_clock::now());
    // A file that vanished between the scan and the unlink is not an error.
    for (const auto& path : plan.to_delete) {
        std::error_code remove_ec;
        fs::remove(path, remove_ec);
    }
    return plan;
}

} // namespace rtmp_server::recording
