#include "rtmp_server/io/io_uring/cross_worker_router.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

namespace rtmp_server::io::io_uring {

CrossWorkerRouter::CrossWorkerRouter(std::size_t worker_count, std::size_t max_queue_frames_per_worker,
                                     std::size_t max_queue_bytes_per_worker)
    : worker_count_(worker_count),
      max_queue_frames_per_worker_(max_queue_frames_per_worker),
      max_queue_bytes_per_worker_(max_queue_bytes_per_worker) {
    queues_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
        auto queue = std::make_unique<PerWorkerQueue>();
        int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        queue->wake_fd = core::FileDescriptor(fd);
        queues_.push_back(std::move(queue));
    }
}

void CrossWorkerRouter::note_subscription(WorkerId worker, StreamId stream_id, int delta) {
    if (worker >= worker_count_) return;

    std::shared_ptr<SubscriberCounters> counters;
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        auto it = counts_.find(stream_id.raw());
        if (it == counts_.end()) {
            it = counts_.emplace(stream_id.raw(), std::make_shared<SubscriberCounters>(worker_count_)).first;
        }
        counters = it->second;
    }

    if (delta > 0) {
        (*counters)[worker].fetch_add(static_cast<std::uint32_t>(delta), std::memory_order_relaxed);
    } else if (delta < 0) {
        // Never underflow below zero: unsubscribe/eviction notifications are
        // best-effort bookkeeping, not a precise refcount contract.
        auto& counter = (*counters)[worker];
        std::uint32_t current = counter.load(std::memory_order_relaxed);
        while (current > 0 &&
               !counter.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
        }
    }
}

void CrossWorkerRouter::forward(WorkerId source_worker, StreamId stream_id, const SharedMediaFrame& frame,
                                 bool is_video, bool is_audio, bool is_sticky, bool is_keyframe) {
    if (source_worker >= worker_count_) return;

    std::shared_ptr<SubscriberCounters> counters;
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        auto it = counts_.find(stream_id.raw());
        // Non-sticky media with nobody subscribed anywhere is dropped. Sticky
        // decoder-init frames are still fanned out to every worker so their
        // LiveFanout state is primed for a viewer that subscribes later.
        if (it == counts_.end()) {
            if (!is_sticky) return;
        } else {
            counters = it->second;
        }
    }

    FrameKind kind = is_video ? FrameKind::Video : (is_audio ? FrameKind::Audio : FrameKind::Metadata);

    for (WorkerId destination = 0; destination < worker_count_; ++destination) {
        if (destination == source_worker) continue;
        // Demand-gate ordinary media; sticky init frames go to every worker.
        if (!is_sticky && (counters == nullptr || (*counters)[destination].load(std::memory_order_relaxed) == 0))
            continue;

        PerWorkerQueue& queue = *queues_[destination];
        bool pushed = false;
        bool should_wake = false;
        std::uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            const auto stream_raw = stream_id.raw();
            const std::size_t frame_bytes = frame.payload.size();

            if (is_sticky) {
                // Sticky state is replaceable, not an event log. Keep only
                // the newest metadata/video-header/audio-header of each kind
                // while a destination worker is busy, so repeated encoder
                // reconfiguration cannot consume the entire bounded queue.
                for (auto it = queue.frames.begin(); it != queue.frames.end();) {
                    if (it->stream_id == stream_id && it->is_sticky && it->kind == kind) {
                        queue.queued_bytes -= std::min(queue.queued_bytes, it->frame.payload.size());
                        it = queue.frames.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Once any part of a stream is lost, forwarding its delta/audio
            // frames would preserve low latency but corrupt decoder state.
            // Skip directly to its next video keyframe. Sticky codec state is
            // still admitted and retained while waiting.
            const bool waiting = queue.waiting_for_keyframe.contains(stream_raw);
            if (waiting && !is_sticky && !(is_video && is_keyframe)) {
                ++dropped;
            } else {
                if (waiting && is_video && is_keyframe) {
                    queue.waiting_for_keyframe.erase(stream_raw);
                }

                const auto would_overflow = [&] {
                    const bool frames_full =
                        max_queue_frames_per_worker_ != 0 && queue.frames.size() >= max_queue_frames_per_worker_;
                    const bool bytes_full =
                        max_queue_bytes_per_worker_ != 0 &&
                        frame_bytes > max_queue_bytes_per_worker_ -
                                          std::min(queue.queued_bytes, max_queue_bytes_per_worker_);
                    return frames_full || bytes_full;
                };

                if (would_overflow()) {
                    // Drop the affected stream's queued partial GOP as one
                    // unit. This bounds latency and guarantees the
                    // destination never receives a hole followed by P/B
                    // frames that depend on the missing data.
                    for (auto it = queue.frames.begin(); it != queue.frames.end();) {
                        if (it->stream_id == stream_id && !it->is_sticky) {
                            queue.queued_bytes -= std::min(queue.queued_bytes, it->frame.payload.size());
                            it = queue.frames.erase(it);
                            ++dropped;
                        } else {
                            ++it;
                        }
                    }
                    if (!is_sticky && !(is_video && is_keyframe)) {
                        queue.waiting_for_keyframe.insert(stream_raw);
                    }
                }

                if (!would_overflow() && (is_sticky || !queue.waiting_for_keyframe.contains(stream_raw))) {
                    should_wake = queue.frames.empty();
                    queue.queued_bytes += frame_bytes;
                    queue.frames.push_back(QueuedFrame{stream_id, frame, kind, is_sticky, is_keyframe});
                    pushed = true;
                } else {
                    ++dropped;
                    // If even a keyframe cannot fit (for example another
                    // stream occupies the worker queue), remain gated until
                    // a later keyframe is successfully admitted.
                    if (!is_sticky) queue.waiting_for_keyframe.insert(stream_raw);
                }
            }
        }
        if (dropped > 0) dropped_frames_.fetch_add(dropped, std::memory_order_relaxed);
        if (!pushed) {
            continue;
        }
        // eventfd is level-triggered for this queue: once a non-empty queue
        // has signalled its worker, one syscall per additional media frame is
        // pure overhead. Signal only on the empty -> non-empty transition.
        if (should_wake && queue.wake_fd.valid()) {
            const std::uint64_t one = 1;
            [[maybe_unused]] auto written = ::write(queue.wake_fd.get(), &one, sizeof(one));
        }
    }
}

void CrossWorkerRouter::on_stream_end(WorkerId source_worker, StreamId stream_id) {
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        counts_.erase(stream_id.raw());
    }

    for (WorkerId destination = 0; destination < worker_count_; ++destination) {
        auto& queue = *queues_[destination];
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            queue.waiting_for_keyframe.erase(stream_id.raw());
            // Replace any already-queued end marker atomically with the new
            // prioritized marker below. Leaving the coalescing bit set while
            // erasing its frame would lose a repeated teardown notification.
            queue.queued_stream_ends.erase(stream_id.raw());
            for (auto it = queue.frames.begin(); it != queue.frames.end();) {
                if (it->stream_id == stream_id) {
                    queue.queued_bytes -= std::min(queue.queued_bytes, it->frame.payload.size());
                    it = queue.frames.erase(it);
                } else {
                    ++it;
                }
            }

            if (destination != source_worker && queue.queued_stream_ends.insert(stream_id.raw()).second) {
                // Control-plane cleanup takes priority over queued media from
                // unrelated streams. It has no payload bytes and is
                // coalesced per StreamId, so it cannot be duplicated by
                // repeated teardown paths.
                queue.frames.push_front(
                    QueuedFrame{stream_id, SharedMediaFrame{}, FrameKind::StreamEnd, false, false});
                wake = true;
            }
        }
        if (wake && queue.wake_fd.valid()) {
            const std::uint64_t one = 1;
            [[maybe_unused]] auto written = ::write(queue.wake_fd.get(), &one, sizeof(one));
        }
    }
}

void CrossWorkerRouter::on_stream_end(StreamId stream_id) {
    on_stream_end(static_cast<WorkerId>(worker_count_), stream_id);
}

std::vector<CrossWorkerRouter::QueuedFrame> CrossWorkerRouter::drain(WorkerId worker, std::size_t max_frames) {
    if (worker >= worker_count_) return {};
    PerWorkerQueue& queue = *queues_[worker];
    std::vector<QueuedFrame> drained;
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        const std::size_t count = std::min(max_frames, queue.frames.size());
        drained.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            queue.queued_bytes -= std::min(queue.queued_bytes, queue.frames.front().frame.payload.size());
            if (queue.frames.front().kind == FrameKind::StreamEnd) {
                queue.queued_stream_ends.erase(queue.frames.front().stream_id.raw());
            }
            drained.push_back(std::move(queue.frames.front()));
            queue.frames.pop_front();
        }
        if (!queue.frames.empty() && queue.wake_fd.valid()) {
            const std::uint64_t one = 1;
            [[maybe_unused]] auto written = ::write(queue.wake_fd.get(), &one, sizeof(one));
        }
    }
    return drained;
}

int CrossWorkerRouter::wake_fd(WorkerId worker) const {
    if (worker >= worker_count_) return -1;
    return queues_[worker]->wake_fd.get();
}

} // namespace rtmp_server::io::io_uring
