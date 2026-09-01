#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/hls/segment.hpp"

namespace rtmp_server::hls {

struct SegmentStoreConfig {
    // Number of segments advertised in the live playlist. RFC 8216 requires
    // at least 3 target durations of media for a stable live window.
    std::size_t live_window_segments = 6;

    // Segments kept beyond the live window before deletion. A player (or
    // CDN) that fetched a playlist a moment ago may still request a segment
    // that has just scrolled out; without this grace it would get a 404.
    // This is the "bounded cleanup" bound — total retained segments is
    // live_window_segments + retention_grace_segments, never more.
    std::size_t retention_grace_segments = 4;

    // Absolute cap on bytes held for this stream regardless of counts, so a
    // high-bitrate publisher cannot balloon memory (docs/v2_promot.md 3.5).
    std::uint64_t max_total_bytes = 256u * 1024u * 1024u;

    std::uint32_t target_duration_seconds = 4;
    std::uint32_t playlist_version = 3;

    // Advertised EXT-X-SERVER-CONTROL HOLD-BACK in seconds. 0 lets the player
    // use its 3 x TARGETDURATION default; a smaller value is clamped up. See
    // MediaPlaylistOptions::hold_back_seconds. CAN-BLOCK-RELOAD=NO is emitted
    // unconditionally so players poll at the segment cadence.
    double playlist_hold_back_seconds = 0.0;

    // Keep an established live URL moving through a temporary source
    // outage by repeating the last complete transport-stream segment. The
    // synthetic copies carry discontinuities because their media timestamps
    // restart. Disabled for ordinary publishers; source-transcode jobs opt in.
    bool repeat_last_segment_on_stall = false;

    // Set when the producer re-encodes onto one continuous, re-anchored
    // output timeline (a source-transcode job): the first real segment after
    // an outage then has identical codec parameters and monotonic PTS, so it
    // needs no EXT-X-DISCONTINUITY. Without this, a source that stalls every
    // minute stamps a discontinuity that often and players freeze crossing
    // each one. A genuine EXT-X-DISCONTINUITY advertised by the upstream
    // playlist still propagates (Segment::discontinuity). Off for passthrough
    // ingest, where a post-outage timestamp jump is real.
    bool seamless_fallback_recovery = false;
};

struct SegmentStoreStats {
    std::uint64_t segments_added = 0;
    // Producer-originated segments only. Unlike segments_added, this does
    // not advance when the store synthesizes outage fallback media, so
    // health monitoring can still detect a wedged transcoder.
    std::uint64_t real_segments_added = 0;
    std::uint64_t segments_evicted = 0;
    std::uint64_t bytes_held = 0;
    std::uint64_t playlist_requests = 0;
    std::uint64_t segment_hits = 0;
    std::uint64_t segment_misses = 0;
    std::uint64_t fallback_segments_added = 0;
};

// Thread-safe, bounded, in-memory store of one stream's HLS segments.
//
// In-memory rather than on-disk by design: segments are short-lived and
// already reference-counted (core::SharedBuffer), so serving them from RAM
// avoids putting disk I/O anywhere near the request path, and avoids the
// write-then-read amplification a disk-backed packager pays. A CDN in front
// absorbs scale; see docs/hls.md "Storage model".
//
// The producer (Segmenter, on the media thread) calls add_segment(); HTTP
// worker threads call find_segment()/playlist(). Both take the same short
// mutex and neither performs I/O or invokes a callback while holding it
// (docs/v2_promot.md 3.7).
class SegmentStore {
public:
    explicit SegmentStore(SegmentStoreConfig config = {}) : config_(config) {}

    // Appends a finished segment and evicts anything past the bounds.
    void add_segment(SegmentPtr segment);

    // Returns the segment with this exact name, or nullptr. The returned
    // shared_ptr keeps the bytes alive for the duration of the response even
    // if eviction removes it concurrently — no copy, no torn read.
    [[nodiscard]] SegmentPtr find_segment(const std::string& name);

    // Renders the current live-window media playlist.
    [[nodiscard]] std::string playlist(const std::string& segment_uri_prefix = {});

    // Marks the stream finished; subsequent playlists carry EXT-X-ENDLIST.
    void mark_ended();
    // Reopens a retained live window when its producer is being rebuilt.
    // Existing segments and media sequence remain intact.
    void mark_live();
    void clear();

    [[nodiscard]] SegmentStoreStats stats() const;
    [[nodiscard]] std::size_t segment_count() const;
    // Sequence the next producer should use when resuming into this store.
    // Keeping it monotonic prevents CDN/browser cache collisions after a
    // source-transcode pipeline is automatically rebuilt.
    [[nodiscard]] std::uint64_t next_sequence() const;
    [[nodiscard]] const SegmentStoreConfig& config() const noexcept { return config_; }

private:
    // Caller must hold mutex_.
    void append_locked(SegmentPtr segment, bool fallback);
    void append_fallback_if_due_locked();
    void evict_locked();

    SegmentStoreConfig config_;
    mutable std::mutex mutex_;
    std::deque<SegmentPtr> segments_;
    std::unordered_map<std::string, SegmentPtr> by_name_;
    std::uint64_t bytes_held_ = 0;
    // Discontinuities that have already scrolled out of the window, so
    // EXT-X-DISCONTINUITY-SEQUENCE stays correct for late joiners.
    std::uint64_t discontinuity_sequence_ = 0;
    bool ended_ = false;
    bool fallback_active_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_segment_added_at_;
    SegmentStoreStats stats_;
};

} // namespace rtmp_server::hls
