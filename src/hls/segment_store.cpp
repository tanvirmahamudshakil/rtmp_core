#include "rtmp_server/hls/segment_store.hpp"

#include <algorithm>
#include <utility>

namespace rtmp_server::hls {

void SegmentStore::add_segment(SegmentPtr segment) {
    if (!segment) return;
    std::lock_guard lock(mutex_);
    bytes_held_ += segment->size_bytes();
    by_name_.emplace(segment->name, segment);
    segments_.push_back(std::move(segment));
    stats_.segments_added += 1;
    evict_locked();
    stats_.bytes_held = bytes_held_;
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

void SegmentStore::clear() {
    std::lock_guard lock(mutex_);
    segments_.clear();
    by_name_.clear();
    bytes_held_ = 0;
    discontinuity_sequence_ = 0;
    ended_ = false;
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

} // namespace rtmp_server::hls
