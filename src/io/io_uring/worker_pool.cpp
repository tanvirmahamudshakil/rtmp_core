#include "rtmp_server/io/io_uring/worker_pool.hpp"

#include <pthread.h>
#include <sched.h>

#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::io::io_uring {

using observability::LogLevel;

WorkerPool::WorkerPool(core::ServerConfig config, protocol::commands::StreamRegistry& stream_registry,
                        protocol::commands::StreamIdRegistry& stream_id_registry)
    : config_(std::move(config)), stream_registry_(stream_registry), stream_id_registry_(stream_id_registry) {}

std::uint32_t WorkerPool::effective_worker_count(const core::ServerConfig& config) {
    std::uint32_t count = config.worker_ring_count;
    if (count == 0) {
        unsigned hardware = std::thread::hardware_concurrency();
        count = hardware == 0 ? 1 : hardware;
    }
    if (count == 0) count = 1;
    if (config.max_worker_ring_count > 0 && count > config.max_worker_ring_count) {
        count = config.max_worker_ring_count;
    }
    return count;
}

core::Result<void> WorkerPool::run() {
    std::uint32_t count = effective_worker_count(config_);
    router_ = std::make_unique<CrossWorkerRouter>(count);

    RTMP_LOG(LogLevel::Info, "worker_pool", "starting",
             {{"worker_count", std::to_string(count)}, {"cpu_pinning", config_.worker_cpu_pinning_enabled ? "true" : "false"}});

    for (std::uint32_t i = 0; i < count; ++i) {
        IoUringContextOptions ring_options;
        ring_options.queue_depth = config_.ring_queue_depth;
        ring_options.enable_sqpoll = config_.enable_sqpoll;
        ring_options.sqpoll_idle_ms = config_.sqpoll_idle_ms;

        auto context_result = IoUringContext::create(ring_options);
        if (!context_result) {
            // Fail fast, before any worker thread has started: mirrors how a
            // single Phase 1-3 loop's context-creation failure was returned
            // straight to main() rather than silently starting a degraded
            // server.
            return context_result.error();
        }

        // Every worker is SO_REUSEPORT-bound, even the sole worker in a
        // count==1 configuration: harmless there, and keeps the bind path
        // identical regardless of worker_ring_count so no separate
        // single-worker code path needs to exist or be tested.
        auto loop = std::make_unique<IoUringEventLoop>(std::move(context_result).value(), config_, stream_registry_,
                                                         stream_id_registry_,
                                                         static_cast<CrossWorkerRouter::WorkerId>(i), router_.get(),
                                                         /*enable_reuseport=*/true);
        workers_.push_back(Worker{std::move(loop), std::thread()});
    }

    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 1;
    bool pin = config_.worker_cpu_pinning_enabled;

    for (std::uint32_t i = 0; i < count; ++i) {
        IoUringEventLoop* loop_ptr = workers_[i].loop.get();
        workers_[i].thread = std::thread([loop_ptr, i, pin, hardware]() {
            if (pin) {
                // Task 12 ("may be configurable but must not be mandatory"):
                // best-effort only — a failure here (e.g. cgroup-restricted
                // cpuset on a container host) must not prevent the worker
                // from running, so its return value is deliberately ignored.
                cpu_set_t cpu_set;
                CPU_ZERO(&cpu_set);
                CPU_SET(i % hardware, &cpu_set);
                [[maybe_unused]] int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpu_set);
            }

            auto result = loop_ptr->run();
            if (!result) {
                RTMP_LOG(LogLevel::Error, "worker_pool", "worker_exited_with_error",
                         {{"worker_id", std::to_string(i)}, {"error", result.error().message()}});
            }
        });
    }

    for (auto& worker : workers_) {
        if (worker.thread.joinable()) worker.thread.join();
    }

    RTMP_LOG(LogLevel::Info, "worker_pool", "stopped", {});
    return {};
}

void WorkerPool::stop() {
    for (auto& worker : workers_) {
        if (worker.loop) worker.loop->stop();
    }
}

} // namespace rtmp_server::io::io_uring
