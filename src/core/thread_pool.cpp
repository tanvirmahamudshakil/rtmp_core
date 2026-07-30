#include "rtmp_server/core/thread_pool.hpp"

namespace rtmp_server::core {

ThreadPool::ThreadPool(std::size_t worker_count) {
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    task_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::size_t index = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            task_cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
            if (pending_.empty()) {
                if (stop_) return;
                continue;
            }
            index = pending_.front();
            pending_.pop();
        }
        (*current_fn_)(index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (--outstanding_ == 0) done_cv_.notify_one();
        }
    }
}

void ThreadPool::parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) return;
    if (workers_.empty() || count == 1) {
        for (std::size_t i = 0; i < count; ++i) fn(i);
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    current_fn_ = &fn;
    outstanding_ = count;
    for (std::size_t i = 0; i < count; ++i) pending_.push(i);
    lock.unlock();
    task_cv_.notify_all();

    lock.lock();
    done_cv_.wait(lock, [this] { return outstanding_ == 0; });
    current_fn_ = nullptr;
}

} // namespace rtmp_server::core
