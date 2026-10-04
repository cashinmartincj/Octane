/**
 * @file ExecutionQueue.h
 * @brief Thread-safe bounded execution queues for offloading blocking I/O and CPU workloads.
 *
 * @details
 * `ExecutionQueue.h` defines configuration structures and bounded thread pool queues
 * designed to isolate slow operations (such as PostgreSQL database queries, file encryption,
 * PDF generation, or third-party HTTP calls) from the latency-sensitive `io_uring` event loop.
 *
 * Architecture:
 * - **Shard-Local Isolation:** Queues can be bound per I/O worker thread or shared across workers.
 *   This avoids cross-core cache invalidation and thread thrashing under high concurrency.
 * - **Bounded Backpressure:** When queue depth reaches `capacity`, `submit()` returns `false`,
 *   enabling the server to return immediate HTTP 503 Service Unavailable / 429 Too Many Requests
 *   rather than exhausting host RAM.
 * - **Graceful Termination:** Destructor signals workers, drains notifications, and joins all threads.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "transport/ExecutionQueue.h"`
 * - Server Engine: Included in `include/transport/TcpServer.h` where `TcpServer` manages worker
 *   thread offloading pools.
 * - Connection Handling: Referenced when a matched route specifies `HandlerExecution::Mode::SharedBlocking`
 *   or `HandlerExecution::Mode::NamedQueue`.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

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

/**
 * @struct ExecutionQueueConfig
 * @brief Configuration settings for a named dedicated execution queue.
 */
struct ExecutionQueueConfig {
    std::string name;                     ///< Unique identifier for this named queue (e.g. "db-pool", "pdf-gen")
    std::size_t threads_per_shard{1};    ///< Number of worker threads spawned per network I/O shard
    std::size_t capacity{1024};          ///< Maximum pending jobs permitted before rejecting submissions
};

/**
 * @struct ExecutionQueueOptions
 * @brief Aggregated execution queue options configuring shared and named thread pools.
 */
struct ExecutionQueueOptions {
    std::size_t shared_threads_per_shard{1};    ///< Threads allocated to default shared_blocking queue
    std::size_t shared_capacity{1024};         ///< Maximum queue capacity for default shared pool
    std::vector<ExecutionQueueConfig> named;    ///< List of dedicated named queue configurations
};

namespace detail {

/**
 * @class BoundedExecutionQueue
 * @brief Thread-safe bounded FIFO task queue served by a fixed-size worker thread pool.
 */
class BoundedExecutionQueue {
public:
    /**
     * @brief Constructs and spawns a bounded queue with worker threads.
     * @param thread_count Number of worker threads to start.
     * @param capacity Maximum number of enqueued jobs allowed.
     */
    BoundedExecutionQueue(std::size_t thread_count, std::size_t capacity)
        : capacity_(capacity) {
        workers_.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    /**
     * @brief Gracefully terminates the queue and joins all active worker threads.
     */
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

    /**
     * @brief Enqueues a job for background execution.
     * @param job Void callable task to execute.
     * @return True if enqueued successfully; false if queue is saturated or shutting down.
     */
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
    std::size_t capacity_;                     ///< Maximum task capacity limit
    std::mutex mutex_;                         ///< Protects task queue and shutdown flag
    std::condition_variable ready_;            ///< Signals available work or shutdown
    std::deque<std::function<void()>> jobs_;   ///< FIFO queue of pending jobs
    std::vector<std::thread> workers_;         ///< Pool of worker threads
    bool stopping_{false};                     ///< Shutdown flag

    /**
     * @brief Continuous worker thread event loop pulling and running tasks.
     */
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
