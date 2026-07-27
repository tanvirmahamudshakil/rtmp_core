#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "rtmp_server/core/config.hpp"
#include "rtmp_server/core/result.hpp"
#include "rtmp_server/io/io_uring/cross_worker_router.hpp"
#include "rtmp_server/io/io_uring/event_loop.hpp"
#include "rtmp_server/protocol/commands/stream_ids.hpp"
#include "rtmp_server/protocol/commands/stream_registry.hpp"

namespace rtmp_server::io::io_uring {

// Phase 4 (docs/v2_promot.md "Multi-core io_uring worker architecture"):
// owns and runs `effective_worker_count()` independent IoUringEventLoops,
// each on its own std::thread with its own IoUringContext/ring/
// ConnectionRegistry/BufferPool/LiveFanout, all bound to the same port via
// SO_REUSEPORT (see IoUringEventLoop's class doc for why a shared
// LiveFanout was rejected in favour of per-worker LiveFanout +
// CrossWorkerRouter).
//
// `stream_registry`/`stream_id_registry` are owned by the caller (typically
// main()) and must outlive the WorkerPool — they are the one piece of
// state every worker shares directly, by reference, since both are
// internally mutex-guarded and cheap to touch (publish/playback-name
// resolution, not the per-packet media path).
class WorkerPool {
public:
    WorkerPool(core::ServerConfig config, protocol::commands::StreamRegistry& stream_registry,
               protocol::commands::StreamIdRegistry& stream_id_registry, EventLoopServices services = {});

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Creates the CrossWorkerRouter and every worker's (IoUringContext,
    // IoUringEventLoop) pair, spawns one thread per worker, and blocks
    // until every worker thread returns (i.e. until stop() completes each
    // worker's graceful shutdown). Returns an error only if a worker's
    // IoUringContext itself could not be created (before any thread starts)
    // — a worker's run() failing after it has started is logged, not
    // propagated, exactly as a single Phase 1-3 loop's run() failure was
    // only ever surfaced via its own return value to a single caller.
    [[nodiscard]] core::Result<void> run();

    // Thread-safe: signals every worker to begin graceful shutdown. Safe to
    // call from a signal handler in the same restricted sense
    // IoUringEventLoop::stop() already is (only touches atomics).
    void stop();

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
    [[nodiscard]] std::size_t subscriber_count(protocol::commands::StreamId stream_id) const;

    // Task 2 ("Default worker count based on hardware concurrency, with a
    // configurable upper bound"): 0 means auto-detect via
    // std::thread::hardware_concurrency() (falling back to 1 if the
    // platform can't report it); any value is clamped to
    // [1, config.max_worker_ring_count].
    [[nodiscard]] static std::uint32_t effective_worker_count(const core::ServerConfig& config);

private:
    struct Worker {
        std::unique_ptr<IoUringEventLoop> loop;
        std::thread thread;
    };

    core::ServerConfig config_;
    protocol::commands::StreamRegistry& stream_registry_;
    protocol::commands::StreamIdRegistry& stream_id_registry_;
    EventLoopServices services_;
    std::unique_ptr<CrossWorkerRouter> router_;
    std::vector<Worker> workers_;
    std::atomic<bool> workers_ready_{false};
};

} // namespace rtmp_server::io::io_uring
