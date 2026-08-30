#include "rtmp_server/hls/segment_store.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace rtmp_server::hls {

namespace {

std::string name_with_sequence(const Segment& source, std::uint64_t sequence) {
    const auto old_sequence = std::to_string(source.sequence);
    const auto position = source.name.rfind(old_sequence);
    if (position != std::string::npos) {
        auto name = source.name;
        name.replace(position, old_sequence.size(), std::to_string(sequence));
        return name;
    }
    return "segment-" + std::to_string(sequence) + ".ts";
}

SegmentPtr copy_with_sequence(const Segment& source, std::uint64_t sequence,
                              bool force_discontinuity) {
    auto copy = std::make_shared<Segment>();
    copy->sequence = sequence;
    copy->name = name_with_sequence(source, sequence);
    copy->data = source.data;
    copy->duration = source.duration;
    copy->discontinuity = source.discontinuity || force_discontinuity;
    return copy;
}

} // namespace

void SegmentStore::add_segment(SegmentPtr segment) {
    if (!segment) return;
    std::lock_guard lock(mutex_);

    // A source may have assigned sequence numbers before a fallback segment
    // was inserted. Re-number it at the store boundary so playlist URLs stay
    // strictly monotonic and can never collide in a browser or CDN cache.
    if (config_.repeat_last_segment_on_stall && !segments_.empty()) {
        const auto next = segments_.back()->sequence + 1;
        if (segment->sequence != next || fallback_active_) {
            // Re-number for URL monotonicity regardless; only mark the
            // recovery segment discontinuous when the producer's timeline is
            // NOT seamless across the outage (see seamless_fallback_recovery).
            const bool mark_discontinuous =
                fallback_active_ && !config_.seamless_fallback_recovery;
            segment = copy_with_sequence(*segment, next, mark_discontinuous);
        }
    }

    append_locked(std::move(segment), /*fallback=*/false);
}

void SegmentStore::append_locked(SegmentPtr segment, bool fallback) {
    bytes_held_ += segment->size_bytes();
    by_name_.emplace(segment->name, segment);
    segments_.push_back(std::move(segment));
    stats_.segments_added += 1;
    if (fallback) {
        stats_.fallback_segments_added += 1;
    } else {
        stats_.real_segments_added += 1;
    }
    fallback_active_ = fallback;
    last_segment_added_at_ = std::chrono::steady_clock::now();
    evict_locked();
    stats_.bytes_held = bytes_held_;
}

void SegmentStore::append_fallback_if_due_locked() {
    if (!config_.repeat_last_segment_on_stall || ended_ || segments_.empty() ||
        !last_segment_added_at_) {
        return;
    }

    auto interval = segments_.back()->duration;
    if (interval <= std::chrono::milliseconds::zero()) {
        interval =
            std::chrono::seconds(std::max<std::uint32_t>(1, config_.target_duration_seconds));
    }
    // Allow a healthy publisher a small scheduling/network margin before
    // declaring the first stall. Once fallback is active, continue exactly
    // at the segment cadence so the player's buffer does not drain.
    const auto initial_grace =
        fallback_active_ ? std::chrono::milliseconds::zero()
                         : std::min(interval / 4, std::chrono::milliseconds(500));
    const auto elapsed = std::chrono::steady_clock::now() - *last_segment_added_at_;
    if (elapsed < interval + initial_grace) {
        return;
    }

    // A playlist poll can arrive long after the outage started (slow/rare
    // pollers), so one stall may span many segment intervals. append_locked()
    // stamps last_segment_added_at_ with the real wall clock on every append,
    // so re-checking "elapsed since last append" after each synthetic segment
    // would immediately read ~0 and stop after a single one -- understating
    // the outage. Compute how many intervals have actually passed up front
    // and backfill that many, each still carrying the real segment cadence in
    // its EXTINF/duration metadata. Bounded by the store's own retention so a
    // stall much longer than the window cannot grow memory or fallback count
    // unboundedly -- eviction drops the excess immediately after each append.
    const std::size_t max_backfill =
        std::max<std::size_t>(1, config_.live_window_segments + config_.retention_grace_segments);
    // steady_clock's native tick is commonly nanoseconds while Segment::duration
    // is milliseconds. Dividing the raw counts treated a 35ms outage as tens
    // of thousands of missed 20ms segments and immediately filled/evicted an
    // entire live window with fallback copies. Convert to one unit first so
    // the playlist advances by the number of media intervals that actually
    // elapsed.
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    const auto initial_cadence = interval + initial_grace;
    const auto missed_intervals = static_cast<std::size_t>(
        elapsed_ms.count() / std::max<std::int64_t>(1, initial_cadence.count()));
    const std::size_t backfill_count =
        std::min(std::max<std::size_t>(1, missed_intervals), max_backfill);

    for (std::size_t appended = 0; appended < backfill_count; ++appended) {
        const auto next = segments_.back()->sequence + 1;
        // A backfill copy is byte-identical to a segment the player already
        // decoded, so it needs no EXT-X-DISCONTINUITY -- the player just
        // replays it (a brief freeze). The genuine break is the *first real
        // segment after the stall*, whose content skips the outage; that one
        // is marked discontinuous in append() via fallback_active_. Marking
        // every backfill copy too made a flaky source (one that stalls every
        // ~15-20 s) emit a discontinuity every few segments, which forces a
        // decoder rebuild that often -- a permanent stutter.
        auto fallback = copy_with_sequence(*segments_.back(), next, /*force_discontinuity=*/false);
        append_locked(std::move(fallback), /*fallback=*/true);
    }
}

void SegmentStore::evict_locked() {
    const std::size_t max_retained = config_.live_window_segments + config_.retention_grace_segments;

    auto drop_front = [&] {
        const auto& front = segments_.front();
        // A discontinuity leaving the window must be counted, otherwise a
        // player joining later mis-numbers its discontinuity sequence.
        if (front->discontinuity) discontinuity_sequence_ += 1;
        bytes_held_ -= std::min<std::uint64_t>(bytes_held_, front->size_bytes());
        by_name_.erase(front->name);
        segments_.pop_front();
        stats_.segments_evicted += 1;
    };

    while (segments_.size() > max_retained) drop_front();

    // Byte cap. Always keep at least one segment so a very large single
    // segment cannot empty the playlist entirely.
    while (bytes_held_ > config_.max_total_bytes && segments_.size() > 1) drop_front();
}

SegmentPtr SegmentStore::find_segment(const std::string& name) {
    std::lock_guard lock(mutex_);
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) {
        stats_.segment_misses += 1;
        return nullptr;
    }
    stats_.segment_hits += 1;
    // Returning a shared_ptr copy: the bytes stay valid for this responder
    // even if eviction drops the store's reference a microsecond later.
    return it->second;
}

std::string SegmentStore::playlist(const std::string& segment_uri_prefix) {
    std::vector<SegmentPtr> window;
    MediaPlaylistOptions options;

    {
        std::lock_guard lock(mutex_);
        stats_.playlist_requests += 1;
        append_fallback_if_due_locked();

        // Advertise only the live window; the grace segments are still
        // fetchable by name but are no longer announced.
        const std::size_t count = std::min(config_.live_window_segments, segments_.size());
        const std::size_t start = segments_.size() - count;
        window.assign(segments_.begin() + static_cast<std::ptrdiff_t>(start), segments_.end());

        std::uint64_t discontinuities_before = discontinuity_sequence_;
        for (std::size_t i = 0; i < start; ++i) {
            if (segments_[i]->discontinuity) discontinuities_before += 1;
        }

        options.target_duration_seconds = config_.target_duration_seconds;
        options.version = config_.playlist_version;
        options.discontinuity_sequence = discontinuities_before;
        options.ended = ended_;
        options.segment_uri_prefix = segment_uri_prefix;
    }

    // Playlist rendering happens outside the lock: it is pure string work
    // and must not extend the critical section the media thread contends on.
    return build_media_playlist(window, options);
}

void SegmentStore::mark_ended() {
    std::lock_guard lock(mutex_);
    ended_ = true;
}

void SegmentStore::mark_live() {
    std::lock_guard lock(mutex_);
    ended_ = false;
}

void SegmentStore::clear() {
    std::lock_guard lock(mutex_);
    segments_.clear();
    by_name_.clear();
    bytes_held_ = 0;
    discontinuity_sequence_ = 0;
    ended_ = false;
    fallback_active_ = false;
    last_segment_added_at_.reset();
    stats_.bytes_held = 0;
}

SegmentStoreStats SegmentStore::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t SegmentStore::segment_count() const {
    std::lock_guard lock(mutex_);
    return segments_.size();
}

std::uint64_t SegmentStore::next_sequence() const {
    std::lock_guard lock(mutex_);
    if (segments_.empty() || !segments_.back()) return 0;
    return segments_.back()->sequence + 1;
}

} // namespace rtmp_server::hls
