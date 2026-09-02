#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <functional>
#include <memory>

#include "rtmp_server/hls/encryption.hpp"
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

    // --- Low-Latency HLS -------------------------------------------------
    // Publish and advertise partial segments. The producing Segmenter must
    // be configured with the same part target, or the store advertises a
    // PART-TARGET no part ever matches.
    bool low_latency = false;
    std::chrono::milliseconds part_target_duration{0};
    // Segments whose parts stay listed in the playlist. Older segments are
    // fetched whole, so repeating their parts only inflates a body a
    // low-latency player refetches several times a second.
    std::size_t part_window_segments = 3;
    // PART-HOLD-BACK. 0 lets the playlist builder apply the RFC 8216bis
    // floor of 3 x PART-TARGET.
    double part_hold_back_seconds = 0.0;
};

// Where the live edge is right now, for a blocking playlist reload
// (_HLS_msn / _HLS_part). A request naming a position at or before this is
// answered immediately; anything beyond it waits.
struct LiveEdge {
    // Media sequence number of the newest segment, complete or open.
    std::uint64_t sequence = 0;
    // Index of the newest published part of that segment, or -1 when the
    // segment has no parts yet.
    std::int64_t part_index = -1;
    // False when nothing has ever been published for this stream, in which
    // case every blocking request must wait rather than be satisfied by a
    // zero-valued edge.
    bool has_media = false;
};

struct SegmentStoreStats {
    std::uint64_t parts_added = 0;
    std::uint64_t parts_evicted = 0;
    std::uint64_t part_hits = 0;
    std::uint64_t part_misses = 0;
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

    // Encrypts every segment and partial segment added from here on, and
    // stamps each with the EXT-X-KEY that decrypts it. Must be set before
    // the producer starts, and shared between renditions of one stream so a
    // player fetches one key for the whole ladder. Null (the default) leaves
    // media in the clear.
    void set_encryptor(std::shared_ptr<SegmentEncryptor> encryptor) {
        encryptor_ = std::move(encryptor);
    }

    // Invoked, with no lock held, after anything that moves the live edge:
    // a new part, a new segment, or the stream ending. This is what releases
    // blocked playlist reloads; it must not block.
    using UpdateNotifier = std::function<void()>;
    void set_update_notifier(UpdateNotifier notifier) { notifier_ = std::move(notifier); }

    // Appends a finished segment and evicts anything past the bounds.
    void add_segment(SegmentPtr segment);

    // Appends one Low-Latency HLS partial segment of the segment currently
    // being produced. Ignored unless the store is configured for low latency.
    void add_part(PartPtr part);

    // Returns the segment with this exact name, or nullptr. The returned
    // shared_ptr keeps the bytes alive for the duration of the response even
    // if eviction removes it concurrently — no copy, no torn read.
    [[nodiscard]] SegmentPtr find_segment(const std::string& name);

    // Same contract as find_segment, for a partial segment. Parts of the
    // open segment and of segments still in the window are both resolvable.
    [[nodiscard]] PartPtr find_part(const std::string& name);

    // Renders the current live-window media playlist.
    [[nodiscard]] std::string playlist(const std::string& segment_uri_prefix = {});

    // Renders the EXT-X-I-FRAMES-ONLY (trick play) playlist over the same
    // window. Empty of segments when no I-frame prefix was ever located.
    [[nodiscard]] std::string iframe_playlist(const std::string& segment_uri_prefix = {});

    // Current live edge, for blocking playlist reload.
    [[nodiscard]] LiveEdge live_edge() const;

    // True once the named (sequence, part) position exists, i.e. a blocking
    // reload for it can be answered. `part_index` < 0 asks only for the
    // segment. Also true once the stream has ended, so a blocked request is
    // never left waiting on media that will never arrive.
    [[nodiscard]] bool has_reached(std::uint64_t sequence, std::int64_t part_index) const;

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
    [[nodiscard]] bool has_reached_locked(std::uint64_t sequence, std::int64_t part_index) const;
    void notify();
    void append_fallback_if_due_locked();
    void evict_locked();

    SegmentStoreConfig config_;
    mutable std::mutex mutex_;
    std::deque<SegmentPtr> segments_;
    std::unordered_map<std::string, SegmentPtr> by_name_;
    // Parts of the segment still being produced. They move into the segment
    // itself once it completes, but stay individually resolvable by name for
    // as long as their segment is retained.
    std::vector<PartPtr> open_parts_;
    std::unordered_map<std::string, PartPtr> parts_by_name_;
    std::uint64_t bytes_held_ = 0;
    // Discontinuities that have already scrolled out of the window, so
    // EXT-X-DISCONTINUITY-SEQUENCE stays correct for late joiners.
    std::uint64_t discontinuity_sequence_ = 0;
    bool ended_ = false;
    bool fallback_active_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_segment_added_at_;
    SegmentStoreStats stats_;
    std::shared_ptr<SegmentEncryptor> encryptor_;
    UpdateNotifier notifier_;
};

} // namespace rtmp_server::hls
