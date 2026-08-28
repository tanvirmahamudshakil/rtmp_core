#include "rtmp_server/io/io_uring/worker_pool.hpp"

#include <pthread.h>
#include <sched.h>

#include "rtmp_server/core/cpu_partition.hpp"
#include "rtmp_server/observability/logger.hpp"

namespace rtmp_server::io::io_uring {

using observability::LogLevel;

WorkerPool::WorkerPool(core::ServerConfig config, protocol::commands::StreamRegistry& stream_registry,
                        protocol::commands::StreamIdRegistry& stream_id_registry, EventLoopServices services)
    : config_(std::move(config)),
      stream_registry_(stream_registry),
      stream_id_registry_(stream_id_registry),
      services_(std::move(services)) {}

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

std::uint32_t WorkerPool::per_worker_connection_limit(std::uint32_t process_limit,
                                                       std::uint32_t worker_count) {
    if (worker_count == 0) worker_count = 1;
    const auto wide_limit = static_cast<std::uint64_t>(process_limit);
    return static_cast<std::uint32_t>((wide_limit + worker_count - 1U) / worker_count);
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
        auto worker_config = config_;
        // maximum_connections is a process-level operator budget. Each loop
        // owns a private registry, so handing the full value to every worker
        // would multiply the advertised cap by worker count.
        worker_config.maximum_connections = per_worker_connection_limit(config_.maximum_connections, count);
        auto loop = std::make_unique<IoUringEventLoop>(
            std::move(context_result).value(), std::move(worker_config), stream_registry_, stream_id_registry_,
            static_cast<CrossWorkerRouter::WorkerId>(i), router_.get(), /*enable_reuseport=*/true, services_);
        workers_.push_back(Worker{std::move(loop), std::thread()});
    }
    workers_ready_.store(true, std::memory_order_release);

    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 1;
    bool pin = config_.worker_cpu_pinning_enabled;

    // A configured reservation confines ingest workers to the cores left
    // over for "everything else" so they can never contend with
    // source-transcode encoder threads for the same core -- independent of
    // worker_cpu_pinning_enabled, which only controls the round-robin
    // cache-locality spread below when no reservation is set. See
    // SourceJobManager's mirrored transcode_cpu_reservation_percent, which
    // confines the transcode side to the complementary set.
    const auto partition = core::compute_cpu_partition(config_.transcode_cpu_reservation_percent);
    const bool reserved = !partition.other_cores.empty();

    for (std::uint32_t i = 0; i < count; ++i) {
        IoUringEventLoop* loop_ptr = workers_[i].loop.get();
        if (stop_requested_.load(std::memory_order_acquire)) loop_ptr->stop();
        workers_[i].thread = std::thread([loop_ptr, i, pin, hardware, reserved, other_cores = partition.other_cores]() {
            if (reserved) {
                core::pin_current_thread_to_cores({other_cores[i % other_cores.size()]});
            } else if (pin) {
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
    stop_requested_.store(true, std::memory_order_release);
    // run() constructs workers on the calling thread before publishing
    // workers_ready_. If shutdown arrives during that initialization,
    // iterating workers_ here would race the vector mutation. run() checks
    // stop_requested_ before starting each completed worker instead.
    if (!workers_ready_.load(std::memory_order_acquire)) return;
    for (auto& worker : workers_) {
        if (worker.loop) worker.loop->stop();
    }
}

std::size_t WorkerPool::subscriber_count(protocol::commands::StreamId stream_id) const {
    if (!workers_ready_.load(std::memory_order_acquire)) return 0;
    std::size_t total = 0;
    for (const auto& worker : workers_) {
        if (worker.loop) total += worker.loop->subscriber_count(stream_id);
    }
    return total;
}

std::uint64_t WorkerPool::egress_bytes_total(protocol::commands::StreamId stream_id) const {
    if (!workers_ready_.load(std::memory_order_acquire)) return 0;
    std::uint64_t total = 0;
    for (const auto& worker : workers_) {
        if (worker.loop) total += worker.loop->egress_bytes_total(stream_id);
    }
    return total;
}

} // namespace rtmp_server::io::io_uring
