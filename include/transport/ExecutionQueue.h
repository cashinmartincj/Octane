#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace octane::transport {

struct ExecutionQueueConfig {
    std::string name;
    std::size_t threads_per_shard{1};
    std::size_t capacity{1024};
};

struct ExecutionQueueOptions {
    std::size_t shared_threads_per_shard{1};
    std::size_t shared_capacity{1024};
    std::vector<ExecutionQueueConfig> named;
};

namespace detail {

class BoundedExecutionQueue {
public:
    BoundedExecutionQueue(std::size_t thread_count, std::size_t capacity)
        : capacity_(capacity) {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~BoundedExecutionQueue() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    BoundedExecutionQueue(const BoundedExecutionQueue&) = delete;
    BoundedExecutionQueue& operator=(const BoundedExecutionQueue&) = delete;

    [[nodiscard]] bool submit(std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || jobs_.size() >= capacity_) return false;
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
        return true;
    }

private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_{false};

    void worker_loop() noexcept {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopping_ || !jobs_.empty();
                });
                if (jobs_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                job();
            } catch (...) {
                // Transport jobs convert handler failures to HTTP responses.
                // Keep a queue worker alive if an unexpected callback escapes.
            }
        }
    }
};

} // namespace detail
} // namespace octane::transport
