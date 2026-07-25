#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "rtmp_server/core/clock.hpp"

namespace rtmp_server::observability {

// One record of a security-relevant management-API action (docs/
// rtmp_promot.md Phase 9 "audit logs"): who did what, to which stream, and
// whether it succeeded. Deliberately does not carry raw secrets — callers
// must pass already-redacted detail strings (same rule
// observability::Logger already documents for structured log fields).
struct AuditEntry {
    core::WallClock::time_point timestamp;
    std::string actor; // e.g. "management-api" — a future auth layer can
                        // narrow this to a specific API-key/principal id
    std::string action; // e.g. "create_stream", "rotate_key", "disconnect_publisher"
    std::string application;
    std::string stream_name;
    bool success = true;
    std::string detail; // free-form, redacted context (never a raw key/token)
};

// Bounded, mutex-guarded, append-only ring buffer of AuditEntry — kept
// in-process (Phase 9 in-memory; Phase-9-adjacent persistence would add a
// Store-backed sink the same way SqliteStore backs StreamManager, not
// implemented here — see docs/phase9-checklist.md "Known limitations").
// Bounded so a burst of management calls can't grow this unboundedly, same
// "drop instead of grow forever" posture as recording::Recorder's queue and
// LiveFanout's slow-client eviction.
class AuditLog {
public:
    explicit AuditLog(std::size_t max_entries = 10000) : max_entries_(max_entries) {}

    void record(AuditEntry entry);

    // Most-recent-last snapshot, for a future admin endpoint / test
    // assertions. Copies out from under the lock (small, infrequent).
    [[nodiscard]] std::vector<AuditEntry> snapshot() const;

    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::deque<AuditEntry> entries_;
    std::size_t max_entries_;
};

} // namespace rtmp_server::observability
