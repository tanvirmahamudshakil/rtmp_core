#include "rtmp_server/transcoding/native/paced_segment_publisher.hpp"

#include <algorithm>
#include <utility>

namespace rtmp_server::transcoding::native {

PacedSegmentPublisher::PacedSegmentPublisher(std::shared_ptr<hls::SegmentStore> store,
                                             PacedSegmentPublisherConfig config)
    : store_(std::move(store)), config_(config) {
    if (config_.fallback_interval <= std::chrono::milliseconds::zero()) {
        config_.fallback_interval = std::chrono::milliseconds(1000);
    }
    if (config_.startup_buffer < std::chrono::milliseconds::zero()) {
        config_.startup_buffer = std::chrono::milliseconds::zero();
    }
    if (config_.recovery_buffer < std::chrono::milliseconds::zero()) {
        config_.recovery_buffer = std::chrono::milliseconds::zero();
    }
    thread_ = std::thread([this] { run(); });
}

PacedSegmentPublisher::~PacedSegmentPublisher() { stop(); }

std::chrono::milliseconds
PacedSegmentPublisher::duration_of(const hls::SegmentPtr& segment) const {
    if (segment && segment->duration > std::chrono::milliseconds::zero()) {
        return segment->duration;
    }
    return config_.fallback_interval;
}

void PacedSegmentPublisher::push(hls::SegmentPtr segment) {
    if (!segment || stopped_.load()) return;
    {
        std::lock_guard lock(mutex_);
        if (stopped_.load()) return;
        buffered_duration_ += duration_of(segment);
        queue_.push_back(std::move(segment));
    }
    wake_.notify_one();
}

void PacedSegmentPublisher::run() {
    std::unique_lock lock(mutex_);
    while (!stopped_.load()) {
        if (queue_.empty()) {
            // If an upstream outage consumes the full runway, refill it
            // before resuming. That prevents a recovered source from falling
            // into a permanent burst/stall loop after the first underrun.
            primed_ = false;
            wake_.wait(lock, [this] { return stopped_.load() || !queue_.empty(); });
            continue;
        }

        if (!primed_) {
            const auto required_buffer =
                ever_published_ ? config_.recovery_buffer : config_.startup_buffer;
            if (buffered_duration_ < required_buffer) {
                wake_.wait(lock, [this] {
                    const auto required =
                        ever_published_ ? config_.recovery_buffer : config_.startup_buffer;
                    return stopped_.load() || buffered_duration_ >= required;
                });
                continue;
            }
            primed_ = true;
            next_publish_ = std::chrono::steady_clock::now();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next_publish_) {
            wake_.wait_until(lock, next_publish_, [this] { return stopped_.load(); });
            continue;
        }

        auto segment = std::move(queue_.front());
        queue_.pop_front();
        const auto media_duration = duration_of(segment);
        buffered_duration_ -= std::min(buffered_duration_, media_duration);

        lock.unlock();
        store_->add_segment(std::move(segment));
        lock.lock();
        ever_published_ = true;

        // Schedule from the actual release time. Trying to "catch up" after
        // a delayed wake by emitting several segments together would recreate
        // the exact burst that this class exists to hide.
        next_publish_ = std::chrono::steady_clock::now() + media_duration;
    }
}

void PacedSegmentPublisher::stop() {
    if (stopped_.exchange(true)) return;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PacedSegmentPublisher::flush() {
    // Join first so the pacing thread cannot have popped an earlier segment
    // while this thread publishes the remaining tail out of order.
    stop();
    std::deque<hls::SegmentPtr> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(queue_);
        buffered_duration_ = std::chrono::milliseconds::zero();
    }
    for (auto& segment : pending) store_->add_segment(std::move(segment));
}

std::size_t PacedSegmentPublisher::queue_size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::chrono::milliseconds PacedSegmentPublisher::buffered_duration() const {
    std::lock_guard lock(mutex_);
    return buffered_duration_;
}

} // namespace rtmp_server::transcoding::native
