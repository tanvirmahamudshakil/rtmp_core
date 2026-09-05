#include "rtmp_server/media/media_handoff_queue.hpp"

#include <utility>

namespace rtmp_server::media {

bool MediaHandoffQueue::over_limit_locked(std::size_t incoming_bytes) const {
    return queued_bytes_ + incoming_bytes > limits_.max_bytes ||
           queue_.size() + 1 > limits_.max_messages;
}

void MediaHandoffQueue::clear_locked() {
    queue_.clear();
    queued_bytes_ = 0;
}

bool MediaHandoffQueue::push(HandoffMessage message) {
    const auto size = message.payload.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;

        // A keyframe is the one message that can end a drop streak, and it is
        // worth making room for: dropping it would extend the outage by a
        // whole GOP for no memory saving.
        if (message.video && message.keyframe) {
            if (over_limit_locked(size)) {
                // The pending frames belong to a GOP the worker has already
                // fallen behind on. Publishing them late would only push it
                // further behind, so the backlog is abandoned in favour of
                // the fresh decodable point.
                dropped_ += queue_.size();
                clear_locked();
                resync_pending_ = true;
                resyncs_ += 1;
            } else if (awaiting_keyframe_) {
                resync_pending_ = true;
                resyncs_ += 1;
            }
            awaiting_keyframe_ = false;
        } else if (message.video && !message.sequence_header) {
            // Mid-GOP video: droppable, and dropped unconditionally while the
            // queue is resynchronising, because its reference frames are gone.
            if (awaiting_keyframe_ || over_limit_locked(size)) {
                awaiting_keyframe_ = true;
                dropped_ += 1;
                return false;
            }
        } else if (over_limit_locked(size)) {
            // Audio, metadata and sequence headers. These are small and
            // stateful -- a lost sequence header breaks every frame after it,
            // and a lost audio frame is an audible gap the ladder cannot
            // recover from -- so they are admitted past the ceiling rather
            // than dropped while video is what actually bounds growth.
            //
            // A worker that is not consuming at all is the exception: audio
            // alone is only tens of KiB per second, but it still grows without
            // end, so a hard multiple of the ceiling stops it. Reaching this
            // means the transcode worker is wedged, not merely behind.
            constexpr std::size_t kHardCapMultiple = 4;
            if (queued_bytes_ + size > limits_.max_bytes * kHardCapMultiple) {
                dropped_ += 1;
                return false;
            }
        }

        queued_bytes_ += size;
        pushed_ += 1;
        queue_.push_back(std::move(message));
    }
    cv_.notify_one();
    return true;
}

bool MediaHandoffQueue::pop(HandoffMessage& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return false; // closed and drained
    out = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= out.payload.size();
    return true;
}

bool MediaHandoffQueue::pop_for(HandoffMessage& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= out.payload.size();
    return true;
}

void MediaHandoffQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

bool MediaHandoffQueue::take_resync() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(resync_pending_, false);
}

HandoffStats MediaHandoffQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    HandoffStats stats;
    stats.pushed = pushed_;
    stats.dropped = dropped_;
    stats.resyncs = resyncs_;
    stats.queued_bytes = queued_bytes_;
    stats.queued_messages = queue_.size();
    return stats;
}

} // namespace rtmp_server::media
