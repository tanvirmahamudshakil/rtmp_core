#include "rtmp_server/dash/segment_store.hpp"

namespace rtmp_server::dash {

void SegmentStore::set_init_segment(InitSegmentPtr init) {
    std::lock_guard lock(init_mutex_);
    init_segment_ = std::move(init);
}

InitSegmentPtr SegmentStore::current_init() const {
    std::lock_guard lock(init_mutex_);
    return init_segment_;
}

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
        bytes_held_ -= std::min<std::uint64_t>(bytes_held_, front->size_bytes());
        by_name_.erase(front->name);
        segments_.pop_front();
        stats_.segments_evicted += 1;
    };

    while (segments_.size() > max_retained) drop_front();
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
    return it->second;
}

SegmentPtr SegmentStore::find_segment_by_number(std::uint64_t number) {
    std::lock_guard lock(mutex_);
    // Segments are appended in increasing-number order and the deque holds
    // only the live window plus grace, so a linear scan from the back (the
    // common case: a player requesting a recent number) is bounded by a
    // small constant, not worth a second index for.
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if ((*it)->number == number) {
            stats_.segment_hits += 1;
            return *it;
        }
    }
    stats_.segment_misses += 1;
    return nullptr;
}

std::uint64_t SegmentStore::start_number() const {
    std::lock_guard lock(mutex_);
    return segments_.empty() ? 0 : segments_.front()->number;
}

std::uint64_t SegmentStore::next_number() const {
    std::lock_guard lock(mutex_);
    if (segments_.empty()) return 0;
    return segments_.back()->number + 1;
}

void SegmentStore::mark_ended() {
    std::lock_guard lock(mutex_);
    ended_ = true;
}

bool SegmentStore::ended() const {
    std::lock_guard lock(mutex_);
    return ended_;
}

void SegmentStore::clear() {
    std::lock_guard lock(mutex_);
    segments_.clear();
    by_name_.clear();
    bytes_held_ = 0;
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

} // namespace rtmp_server::dash
