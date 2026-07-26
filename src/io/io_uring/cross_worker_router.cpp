#include "rtmp_server/io/io_uring/cross_worker_router.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>

namespace rtmp_server::io::io_uring {

CrossWorkerRouter::CrossWorkerRouter(std::size_t worker_count, std::size_t max_queue_frames_per_worker)
    : worker_count_(worker_count), max_queue_frames_per_worker_(max_queue_frames_per_worker) {
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

    std::vector<std::atomic<std::uint32_t>>* counters = nullptr;
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        auto it = counts_.find(stream_id.raw());
        if (it == counts_.end()) {
            std::vector<std::atomic<std::uint32_t>> fresh(worker_count_);
            it = counts_.emplace(stream_id.raw(), std::move(fresh)).first;
        }
        counters = &it->second;
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
                                 bool is_video, bool is_audio) {
    if (source_worker >= worker_count_) return;

    std::vector<std::atomic<std::uint32_t>>* counters = nullptr;
    {
        std::lock_guard<std::mutex> lock(counts_mutex_);
        auto it = counts_.find(stream_id.raw());
        if (it == counts_.end()) return; // nobody anywhere subscribed to this stream
        counters = &it->second;
    }

    FrameKind kind = is_video ? FrameKind::Video : (is_audio ? FrameKind::Audio : FrameKind::Metadata);

    for (WorkerId destination = 0; destination < worker_count_; ++destination) {
        if (destination == source_worker) continue;
        if ((*counters)[destination].load(std::memory_order_relaxed) == 0) continue;

        PerWorkerQueue& queue = *queues_[destination];
        bool pushed = false;
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            if (queue.frames.size() < max_queue_frames_per_worker_) {
                queue.frames.push_back(QueuedFrame{stream_id, frame, kind});
                pushed = true;
            }
        }
        if (!pushed) {
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (queue.wake_fd.valid()) {
            const std::uint64_t one = 1;
            [[maybe_unused]] auto written = ::write(queue.wake_fd.get(), &one, sizeof(one));
        }
    }
}

void CrossWorkerRouter::on_stream_end(StreamId stream_id) {
    std::lock_guard<std::mutex> lock(counts_mutex_);
    counts_.erase(stream_id.raw());
}

std::vector<CrossWorkerRouter::QueuedFrame> CrossWorkerRouter::drain(WorkerId worker) {
    if (worker >= worker_count_) return {};
    PerWorkerQueue& queue = *queues_[worker];
    std::vector<QueuedFrame> drained;
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        drained.reserve(queue.frames.size());
        for (auto& frame : queue.frames) drained.push_back(std::move(frame));
        queue.frames.clear();
    }
    return drained;
}

int CrossWorkerRouter::wake_fd(WorkerId worker) const {
    if (worker >= worker_count_) return -1;
    return queues_[worker]->wake_fd.get();
}

} // namespace rtmp_server::io::io_uring
