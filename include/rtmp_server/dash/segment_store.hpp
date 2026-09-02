#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rtmp_server/dash/segment.hpp"

namespace rtmp_server::dash {

struct SegmentStoreConfig {
    // Segments advertised/retained. Same bounded-window/grace shape as
    // hls::SegmentStoreConfig, for the same reason: a player that just read
    // the manifest may still request a segment that has scrolled out of the
    // advertised window a moment later.
    std::size_t live_window_segments = 6;
    std::size_t retention_grace_segments = 4;
    std::uint64_t max_total_bytes = 256u * 1024u * 1024u;
    std::uint32_t target_duration_seconds = 4;
};

struct SegmentStoreStats {
    std::uint64_t segments_added = 0;
    std::uint64_t segments_evicted = 0;
    std::uint64_t bytes_held = 0;
    std::uint64_t segment_hits = 0;
    std::uint64_t segment_misses = 0;
};

// Thread-safe, bounded, in-memory store of one representation's DASH init +
// media segments — the fMP4 counterpart of hls::SegmentStore. Deliberately a
// separate type rather than a template shared with hls::SegmentStore: the two
// segment shapes differ (an fMP4 segment references an init segment epoch; a
// TS segment carries parts/encryption/I-frame metadata that has no fMP4
// analogue here), and forcing one generic store to carry every field either
// type needs would make each harder to read for what it actually does.
class SegmentStore {
public:
    explicit SegmentStore(SegmentStoreConfig config = {}) : config_(config) {}

    // Publishes a new init segment epoch. Every InitSegmentPtr returned by
    // current_init() from this point on is this one, until the next call.
    void set_init_segment(InitSegmentPtr init);
    [[nodiscard]] InitSegmentPtr current_init() const;

    // Appends a finished media segment and evicts anything past the bounds.
    void add_segment(SegmentPtr segment);

    [[nodiscard]] SegmentPtr find_segment(const std::string& name);
    [[nodiscard]] SegmentPtr find_segment_by_number(std::uint64_t number);

    // $Number$ of the oldest segment currently advertised, for the MPD's
    // SegmentTemplate@startNumber. 0 when nothing has been produced yet.
    [[nodiscard]] std::uint64_t start_number() const;
    [[nodiscard]] std::uint64_t next_number() const;

    void mark_ended();
    void clear();

    [[nodiscard]] SegmentStoreStats stats() const;
    [[nodiscard]] std::size_t segment_count() const;
    [[nodiscard]] const SegmentStoreConfig& config() const noexcept { return config_; }
    [[nodiscard]] bool ended() const;

private:
    void evict_locked();

    SegmentStoreConfig config_;
    mutable std::mutex mutex_;
    std::deque<SegmentPtr> segments_;
    std::unordered_map<std::string, SegmentPtr> by_name_;
    std::uint64_t bytes_held_ = 0;
    bool ended_ = false;
    SegmentStoreStats stats_;

    // Guarded separately from the segment deque: a reader resolving the
    // current init segment must never block behind (or race ahead of) the
    // producer appending a media segment, and vice versa.
    mutable std::mutex init_mutex_;
    InitSegmentPtr init_segment_;
};

} // namespace rtmp_server::dash
