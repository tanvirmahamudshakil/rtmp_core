#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rtmp_server/core/file_descriptor.hpp"
#include "rtmp_server/protocol/commands/shared_media_frame.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"

namespace rtmp_server::io::io_uring {

using protocol::commands::SharedMediaFrame;
using protocol::commands::StreamId;

// Phase 4 (docs/v2_promot.md "Multi-core io_uring worker architecture"):
// process-wide router that lets each worker's own, single-threaded
// LiveFanout forward media to *other* workers' LiveFanout instances without
// those workers ever sharing a stream's subscriber table, GOP cache, or
// mutex directly (GopCache/ViewerQueue have no locking of their own and must
// only ever be touched from one thread — see live_fanout.hpp).
//
// Design, matching the doc's explicit requirements:
//   - "Route each shared media frame once per target egress worker": forward()
//     pushes exactly one SharedMediaFrame reference (a shared_ptr copy, no
//     byte copy) into each *other* worker's bounded inbound queue, never one
//     push per viewer.
//   - "Implement bounded inter-worker queues": each worker's inbound queue
//     has a fixed capacity; forward() drops (does not block) when full.
//   - "Avoid cross-worker access to mutable connection state": frames are
//     handed off by value through the queue; the destination worker decides
//     entirely on its own thread what to do with them (typically:
//     LiveFanout::on_video(..., is_replayed=true) etc.).
//
// Thread-safety: note_subscription()/forward()/on_stream_end() may be called
// concurrently from any worker thread. drain() must only ever be called by
// the owning worker's own thread (the one identified by `worker`).
class CrossWorkerRouter {
public:
    using WorkerId = std::uint32_t;

    enum class FrameKind : std::uint8_t { Video, Audio, Metadata, StreamEnd };

    struct QueuedFrame {
        StreamId stream_id;
        SharedMediaFrame frame;
        FrameKind kind;
        bool is_sticky = false;
        bool is_keyframe = false;
    };

    // `worker_count` must match the number of workers the caller will ever
    // pass as a `worker`/`source_worker` argument (workers are identified by
    // a dense [0, worker_count) index). `max_queue_frames_per_worker` bounds
    // each worker's inbound queue independently of any other worker's.
    explicit CrossWorkerRouter(std::size_t worker_count, std::size_t max_queue_frames_per_worker = 4096,
                               std::size_t max_queue_bytes_per_worker = 32 * 1024 * 1024);

    CrossWorkerRouter(const CrossWorkerRouter&) = delete;
    CrossWorkerRouter& operator=(const CrossWorkerRouter&) = delete;

    // Bound to LiveFanout::set_subscription_hook() on each worker's local
    // LiveFanout, with `worker` fixed to that worker's own id via a lambda
    // capture. delta is +1 on subscribe, -1 on unsubscribe/eviction.
    void note_subscription(WorkerId worker, StreamId stream_id, int delta);

    // Bound to LiveFanout::set_forward_hook() on each worker's local
    // LiveFanout, with `source_worker` fixed via lambda capture. Pushes one
    // frame reference into every other worker's inbound queue that
    // currently has at least one local subscriber for `stream_id`.
    //
    // is_sticky frames (onMetadata, AVC/AAC sequence headers) are pushed to
    // EVERY other worker unconditionally, ignoring subscriber counts, so each
    // worker's LiveFanout always holds the decoder-init state a future
    // subscriber needs. Without this, a viewer that lands on a different
    // worker than the publisher receives media it cannot decode because the
    // sequence header was sent before that worker had any subscriber.
    void forward(WorkerId source_worker, StreamId stream_id, const SharedMediaFrame& frame, bool is_video,
                 bool is_audio, bool is_sticky = false, bool is_keyframe = false);

    // Bound to LiveFanout::set_stream_end_hook(). Purges queued media and
    // sends a lightweight StreamEnd control to every other worker so their
    // local viewers are notified and their GOP/header cache is discarded.
    // The one-argument overload broadcasts to every worker and is retained
    // for embedders/tests that do not track a source worker.
    void on_stream_end(WorkerId source_worker, StreamId stream_id);
    void on_stream_end(StreamId stream_id);

    // Drains every frame currently queued for `worker` (FIFO). Must only be
    // called from that worker's own thread — typically once per completion-
    // loop iteration, after the worker's inbound wake eventfd fires.
    [[nodiscard]] std::vector<QueuedFrame> drain(
        WorkerId worker, std::size_t max_frames = static_cast<std::size_t>(-1));

    // Readable/pollable fd that becomes ready (via a byte written by
    // forward()) whenever a frame is pushed for `worker`. Owned by the
    // router; workers register it with their own io_uring ring for a
    // (re-armed) read/poll completion and must read the pending bytes after
    // draining to reset it for the next wake-up.
    [[nodiscard]] int wake_fd(WorkerId worker) const;

    // Number of frames dropped so far across all workers because a
    // destination's inbound queue was full — exposed for tests/metrics, not
    // meant to be read on any hot path.
    [[nodiscard]] std::uint64_t dropped_frame_count() const noexcept { return dropped_frames_.load(); }

private:
    struct PerWorkerQueue {
        std::mutex mutex;
        std::deque<QueuedFrame> frames;
        std::unordered_set<std::uint64_t> waiting_for_keyframe;
        std::unordered_set<std::uint64_t> queued_stream_ends;
        std::size_t queued_bytes = 0;
        core::FileDescriptor wake_fd;
    };

    // Guards only insert of new counts_ entries / erase from on_stream_end;
    // per-worker counters inside an entry are plain std::atomic and read/
    // written without holding this mutex once the entry exists.
    std::mutex counts_mutex_;
    using SubscriberCounters = std::vector<std::atomic<std::uint32_t>>;
    std::unordered_map<std::uint64_t, std::shared_ptr<SubscriberCounters>> counts_; // key = StreamId::raw()

    std::size_t worker_count_;
    std::size_t max_queue_frames_per_worker_;
    std::size_t max_queue_bytes_per_worker_;
    std::vector<std::unique_ptr<PerWorkerQueue>> queues_;
    std::atomic<std::uint64_t> dropped_frames_{0};
};

} // namespace rtmp_server::io::io_uring
