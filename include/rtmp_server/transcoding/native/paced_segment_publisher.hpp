#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "rtmp_server/hls/segment_store.hpp"

namespace rtmp_server::transcoding::native {

struct PacedSegmentPublisherConfig {
    // Keep a complete upstream HLS window in reserve before exposing the
    // first output segment. This absorbs the source's irregular 8-15 second
    // publish cadence without making that cadence visible to viewers.
    std::chrono::milliseconds startup_buffer{30000};

    // Once playback has started, an upstream hiccup must not force viewers
    // to wait for the entire cold-start runway again. Refill a smaller
    // recovery runway after an underrun, then resume real-time pacing.
    std::chrono::milliseconds recovery_buffer{10000};

    // Used only for a malformed/empty-duration segment. Normal publication
    // is paced by each segment's actual EXTINF duration.
    std::chrono::milliseconds fallback_interval{1000};

    // Upper bound on how much media may sit queued ahead of the viewer.
    // Strict real-time pacing only holds the buffer steady when the source
    // delivers at exactly real time; anything that hands over a burst it
    // never gives back -- an upstream window several minutes long, a source
    // clock running slightly fast, a catch-up after a reconnect -- adds that
    // surplus to the queue permanently. The viewer then watches the whole
    // stream that far behind the live edge and the queue's memory is never
    // reclaimed. Past this bound release runs slightly faster than real time
    // (see drain_ratio) until the backlog is back under it.
    std::chrono::milliseconds max_buffer{45000};

    // Fraction of a segment's own duration to wait before releasing the next
    // one while over max_buffer. 0.9 drains 10% faster than real time: a
    // viewer's buffer still fills faster than it empties, so nothing stalls,
    // and a 30s surplus is absorbed over a few minutes rather than never.
    // Deliberately not an immediate flush -- dumping the backlog at once is
    // exactly the burst this class exists to hide.
    double drain_ratio = 0.9;
};

// Converts the bursty completion cadence of an HLS pull/transcode pipeline
// into a real-time output cadence. Segmenter can turn one 10-second upstream
// chunk into five 2-second output segments almost instantly; publishing those
// five at a fixed 1-second rate drains 10 seconds of media in 5 seconds and
// guarantees a viewer stall. This publisher waits for a bounded runway, then
// schedules each release by the actual media duration of the segment released.
class PacedSegmentPublisher {
public:
    explicit PacedSegmentPublisher(std::shared_ptr<hls::SegmentStore> store,
                                   PacedSegmentPublisherConfig config = {});
    ~PacedSegmentPublisher();
    PacedSegmentPublisher(const PacedSegmentPublisher&) = delete;
    PacedSegmentPublisher& operator=(const PacedSegmentPublisher&) = delete;

    void push(hls::SegmentPtr segment);

    // Stops pacing and publishes the remaining tail in sequence order. Used
    // only when the source ends or the puller shuts down.
    void flush();
    void stop();

    [[nodiscard]] std::size_t queue_size() const;
    [[nodiscard]] std::chrono::milliseconds buffered_duration() const;

private:
    [[nodiscard]] std::chrono::milliseconds duration_of(const hls::SegmentPtr& segment) const;
    void run();

    std::shared_ptr<hls::SegmentStore> store_;
    PacedSegmentPublisherConfig config_;
    std::deque<hls::SegmentPtr> queue_;
    std::chrono::milliseconds buffered_duration_{0};
    std::chrono::steady_clock::time_point next_publish_{};
    bool primed_ = false;
    bool ever_published_ = false;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

} // namespace rtmp_server::transcoding::native
