#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "rtmp_server/core/cpu_partition.hpp"

namespace rtmp_server::core {

// Fixed-size worker pool for synchronous fan-out/fan-in of CPU-bound work
// (e.g. encoding several renditions from one decoded frame). parallel_for
// blocks the calling thread until every task completes, so callers get
// parallelism across cores without paying thread-creation cost per frame.
// Only one parallel_for call may be in flight at a time per pool instance.
class ThreadPool {
public:
    // `pinned_cores` restricts every worker thread's affinity to that core
    // set (best-effort, see core::pin_current_thread_to_cores). Empty (the
    // default) leaves workers unpinned, exactly as before this parameter
    // existed.
    explicit ThreadPool(std::size_t worker_count, std::vector<unsigned> pinned_cores = {});
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Invokes fn(i) for each i in [0, count), spread across the pool's
    // workers, blocking until every invocation has returned. With zero
    // workers (e.g. a single-core host or count == 1) it just runs inline.
    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn);

private:
    void worker_loop();

    std::vector<unsigned> pinned_cores_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable task_cv_;
    std::condition_variable done_cv_;
    std::queue<std::size_t> pending_;
    const std::function<void(std::size_t)>* current_fn_ = nullptr;
    std::size_t outstanding_ = 0;
    bool stop_ = false;
};

} // namespace rtmp_server::core
