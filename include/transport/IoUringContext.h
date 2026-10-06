/**
 * @file IoUringContext.h
 * @brief High-throughput Linux io_uring kernel event reactor for HTTP/1.1 transport.
 *
 * @details
 * `IoUringContext` represents the ultra-performance asynchronous core of the Octane framework.
 * It manages an independent Linux `io_uring` ring instance per worker thread, interacting directly
 * with the Linux kernel submission and completion queues without system call overhead on hot paths.
 *
 * Core Architectural Highlights:
 * - **Zero-Syscall Multi-Shot Accept:** Utilizes `io_uring_prep_multishot_accept` where supported to
 *   accept multiple incoming client TCP sockets continuously with a single submission queue entry (SQE).
 * - **Allocation-Free Context Block Pool (`ContextPool`):** Preallocates slabs of 256 `AsyncContext`
 *   descriptors, completely eliminating heap allocations when scheduling operations.
 * - **Zero-Allocation Binary Min-Heap Deadlines:** Maintains an indexed priority queue (`deadlines_`)
 *   for active connection timeouts (read, write, idle keep-alive). Nodes update in O(log N) in-place
 *   without timerfd allocations or dynamic memory allocations.
 * - **Zero-Copy Vectored I/O (`iovec`):** Assembles response headers and response body buffers directly
 *   into scatter-gather I/O vectors for transmission via `io_uring_prep_sendmsg`.
 * - **Inter-Thread Handler Wakeup:** Integrates a non-blocking Linux `eventfd` (`wake_fd_`) monitored via
 *   `POLLIN` in the ring to wake the reactor loop when background queue workers complete offloaded requests.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "transport/IoUringContext.h"`
 * - Worker Engine: Instantiated inside `octane::transport::TcpServer::run_worker` (`include/transport/TcpServer.h`),
 *   running one isolated `IoUringContext` per CPU core.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once

#include <liburing.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "HttpLimits.h"
#include "HttpParser.h"
#include "HttpResponse.h"
#include "Router.h"
#include "transport/ExecutionQueue.h"
#include "websocket/WebSocket.h"
#include "websocket/WebSocketHandshake.h"
#include "websocket/WebSocketSession.h"

namespace octane::transport {

/**
 * @enum OperationType
 * @brief Identifies the asynchronous operation currently tracked by an SQE context.
 */
enum class OperationType {
    Accept,       ///< Listening socket accept
    AcceptRetry,  ///< Single-shot accept fallback
    Read,         ///< Socket receive operation
    Write,        ///< Socket send / writev operation
    Wake,         ///< Worker thread completion eventfd poll
    Cancel        ///< SQE cancellation request
};

struct AsyncContext;

/**
 * @struct ConnectionState
 * @brief Represents the complete lifecycle state of an active TCP client connection.
 */
struct ConnectionState {
    int fd{-1};                              ///< Active client socket file descriptor
    std::string input;                      ///< Dynamic buffer for accumulated HTTP request bytes
    std::size_t read_position{0};           ///< Offset of unparsed bytes in `input`
    std::array<char, 4096> read_buffer{};   ///< Static 4KB scratch buffer for kernel recv

    std::string response_headers;           ///< Serialized HTTP/1.1 response headers
    std::string owned_body;                 ///< Transferred dynamic body payload
    std::string_view response_body;         ///< Active response body view (owned or mmap)
    std::array<iovec, 2> write_iov{};       ///< Scatter-gather vector: [0]=headers, [1]=body
    msghdr write_message{};                 ///< Linux message header for `sendmsg`
    std::size_t write_offset{0};            ///< Bytes transmitted so far
    std::size_t write_size{0};              ///< Total bytes to transmit
    std::size_t request_count{0};           ///< Total HTTP requests served on this keep-alive connection
    std::uint64_t id{0};                    ///< Unique per-connection identifier
    static constexpr std::size_t no_deadline =
        std::numeric_limits<std::size_t>::max();
    std::size_t deadline_index{no_deadline};///< Slot in binary min-heap `deadlines_`
    OperationType deadline_operation{OperationType::Read}; ///< Operation associated with current deadline
    std::chrono::steady_clock::time_point deadline;        ///< Expiration time point

    bool close_after_write{false};          ///< Close connection once write buffer drains
    bool closed{false};                     ///< True if socket has been closed
    bool cancel_requested{false};           ///< True if cancellation SQE has been issued
    bool handler_pending{false};            ///< True if offloaded to background ExecutionQueue
    AsyncContext* pending{nullptr};         ///< Pointer to current in-flight SQE context
    std::chrono::steady_clock::time_point read_deadline;  ///< Read timeout limit
    std::chrono::steady_clock::time_point write_deadline; ///< Write timeout limit

    bool is_websocket{false};               ///< True if connection has upgraded to WebSocket
    bool is_websocket_upgrade{false};       ///< True if pending write is 101 WS upgrade
    const WebSocketConfig* active_ws_config{nullptr};
    std::shared_ptr<websocket::WebSocketSession> ws_session;
    std::deque<std::string> ws_write_queue;
    bool ws_writing{false};

    /// Returns unconsumed input bytes available for parsing
    [[nodiscard]] std::size_t available_input() const noexcept {
        return input.size() - read_position;
    }

    /// Returns non-owning string view of unconsumed input bytes
    [[nodiscard]] std::string_view input_view() const noexcept {
        return std::string_view(input).substr(read_position);
    }
};

/**
 * @struct AsyncContext
 * @brief User data descriptor attached to each io_uring SQE (`sqe->user_data`).
 */
struct AsyncContext {
    OperationType operation{OperationType::Cancel}; ///< Type of active operation
    ConnectionState* connection{nullptr};            ///< Associated connection state (or null)
    __kernel_timespec timeout{};                     ///< Kernel timeout specification
};

/**
 * @class ContextPool
 * @brief Slab-allocated pool for AsyncContext objects ensuring zero heap allocations on hot path.
 */
class ContextPool {
public:
    /**
     * @brief Acquires an AsyncContext initialized with operation and connection pointers.
     */
    AsyncContext* acquire(OperationType operation,
                          ConnectionState* connection = nullptr) {
        if (free_.empty()) grow();
        AsyncContext* context = free_.back();
        free_.pop_back();
        context->operation = operation;
        context->connection = connection;
        context->timeout = {};
        return context;
    }

    /**
     * @brief Returns an AsyncContext to the free list for immediate reuse.
     */
    void release(AsyncContext* context) noexcept {
        context->connection = nullptr;
        free_.push_back(context);
    }

private:
    static constexpr std::size_t block_size = 256;
    std::vector<std::unique_ptr<AsyncContext[]>> blocks_;
    std::vector<AsyncContext*> free_;

    void grow() {
        auto block = std::make_unique<AsyncContext[]>(block_size);
        free_.reserve(free_.size() + block_size);
        for (std::size_t i = 0; i < block_size; ++i)
            free_.push_back(&block[i]);
        blocks_.push_back(std::move(block));
    }
};

/**
 * @class IoUringContext
 * @brief Single-threaded Linux io_uring reactor driving socket I/O, parsing, and execution.
 */
class IoUringContext {
public:
    /**
     * @brief Initializes an io_uring ring and eventfd wakeup notification channel.
     * @param server_fd Pre-configured listening socket file descriptor.
     * @param router Shared pointer to application route table.
     * @param limits Protocol and timeout boundaries.
     * @param global_connections Shared atomic tracking global concurrent connections.
     * @param external_stop Shared atomic flag signaling graceful server shutdown.
     * @param queue_depth Number of SQE/CQE entries in ring (default: 128).
     * @param execution_options Offload thread pool queue configurations.
     */
    explicit IoUringContext(
        int server_fd,
        std::shared_ptr<octane::Router> router,
        const HttpLimits& limits,
        std::shared_ptr<std::atomic_size_t> global_connections = nullptr,
        const std::atomic_bool* external_stop = nullptr,
        unsigned int queue_depth = 128,
        const ExecutionQueueOptions& execution_options = {})
        : server_fd_(server_fd), router_(std::move(router)), limits_(limits),
          global_connections_(std::move(global_connections)),
          external_stop_(external_stop),
          execution_options_(execution_options) {
        limits_.validate();
        const int result = io_uring_queue_init(queue_depth, &ring_, 0);
        if (result < 0) {
            throw std::runtime_error("Failed to initialize io_uring: " +
                                     std::string(strerror(-result)));
        }
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            const int error = errno;
            io_uring_queue_exit(&ring_);
            throw std::runtime_error("Failed to initialize handler wakeup: " +
                                     std::string(strerror(error)));
        }
    }

    /**
     * @brief Destructor tearing down active connections, eventfd, and the io_uring ring.
     */
    ~IoUringContext() {
        executors_.clear();
        for (auto& [fd, connection] : connections_) {
            if (!connection->closed) ::close(fd);
        }
        if (wake_fd_ >= 0) ::close(wake_fd_);
        io_uring_queue_exit(&ring_);
    }

    IoUringContext(const IoUringContext&) = delete;
    IoUringContext& operator=(const IoUringContext&) = delete;

    /**
     * @brief Main reactor event loop processing ring CQEs and scheduling requests.
     */
    void run() {
        if (!queue_accept()) throw std::runtime_error("Failed to queue accept");
        if (!queue_wake_poll())
            throw std::runtime_error("Failed to queue handler wakeup poll");
        flush_submissions();

        while (!shutting_down_ || outstanding_ != 0) {
            update_deadlines_and_shutdown();
            flush_submissions();
            if (shutting_down_ && outstanding_ == 0) break;

            io_uring_cqe* first = nullptr;
            __kernel_timespec poll_interval =
                next_wait_timeout(std::chrono::steady_clock::now());
            const int result = io_uring_wait_cqe_timeout(
                &ring_, &first, &poll_interval);
            if (result == -ETIME || result == -EINTR) continue;
            if (result < 0) {
                throw std::runtime_error("io_uring completion wait failed: " +
                                         std::string(strerror(-result)));
            }

            handle_completion(first);
            std::array<io_uring_cqe*, 63> completions{};
            const unsigned count = io_uring_peek_batch_cqe(
                &ring_, completions.data(), completions.size());
            for (unsigned i = 0; i < count; ++i)
                handle_completion(completions[i]);
        }
    }

    void stop() noexcept {
        stop_requested_.store(true, std::memory_order_relaxed);
    }

private:
    io_uring ring_{};
    int server_fd_;
    std::shared_ptr<octane::Router> router_;
    HttpLimits limits_;
    std::shared_ptr<std::atomic_size_t> global_connections_;
    const std::atomic_bool* external_stop_{nullptr};
    std::unordered_map<int, std::unique_ptr<ConnectionState>> connections_;
    ContextPool context_pool_;
    std::atomic_bool stop_requested_{false};
    bool shutting_down_{false};
    bool multishot_accept_{true};
    std::chrono::steady_clock::time_point shutdown_deadline_{};
    AsyncContext* accept_context_{nullptr};
    bool accept_cancel_requested_{false};
    int wake_fd_{-1};
    AsyncContext* wake_context_{nullptr};
    bool wake_cancel_requested_{false};
    std::size_t outstanding_{0};
    std::size_t handler_jobs_{0};
    std::uint64_t next_connection_id_{1};
    ExecutionQueueOptions execution_options_;

    struct HandlerCompletion {
        int fd{-1};
        std::uint64_t connection_id{0};
        HttpResponse response;
        bool suppress_body{false};
        bool base_close{false};
        bool handler_failed{false};
    };

    std::mutex completion_mutex_;
    std::deque<HandlerCompletion> handler_completions_;
    std::unordered_map<std::string,
        std::unique_ptr<detail::BoundedExecutionQueue>> executors_;

    // Each live connection owns at most one indexed heap slot. This avoids a
    // heap allocation, weak_ptr reference traffic, and stale timer entries on
    // every keep-alive request.
    std::vector<ConnectionState*> deadlines_;

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_requested_.load(std::memory_order_relaxed) ||
               (external_stop_ &&
                external_stop_->load(std::memory_order_acquire));
    }

    void flush_submissions() {
        unsigned stalled_attempts = 0;
        while (io_uring_sq_ready(&ring_) != 0) {
            const int result = io_uring_submit(&ring_);
            if (result > 0) {
                stalled_attempts = 0;
                continue;
            }
            if (result == -EINTR || result == -EAGAIN || result == 0) {
                if (++stalled_attempts < 8) continue;
            }
            const int error = result < 0 ? -result : EIO;
            throw std::runtime_error("io_uring submission failed: " +
                                     std::string(strerror(error)));
        }
    }

    void set_deadline(ConnectionState* connection,
                      OperationType operation,
                      std::chrono::steady_clock::time_point deadline) {
        const auto previous = connection->deadline;
        connection->deadline = deadline;
        connection->deadline_operation = operation;
        if (connection->deadline_index == ConnectionState::no_deadline) {
            connection->deadline_index = deadlines_.size();
            deadlines_.push_back(connection);
            sift_deadline_up(connection->deadline_index);
        } else if (deadline < previous) {
            sift_deadline_up(connection->deadline_index);
        } else {
            sift_deadline_down(connection->deadline_index);
        }
    }

    void swap_deadlines(std::size_t left, std::size_t right) noexcept {
        std::swap(deadlines_[left], deadlines_[right]);
        deadlines_[left]->deadline_index = left;
        deadlines_[right]->deadline_index = right;
    }

    void sift_deadline_up(std::size_t index) noexcept {
        while (index != 0) {
            const std::size_t parent = (index - 1) / 2;
            if (deadlines_[parent]->deadline <= deadlines_[index]->deadline)
                break;
            swap_deadlines(parent, index);
            index = parent;
        }
    }

    void sift_deadline_down(std::size_t index) noexcept {
        for (;;) {
            const std::size_t left = index * 2 + 1;
            if (left >= deadlines_.size()) return;
            const std::size_t right = left + 1;
            const std::size_t earliest =
                right < deadlines_.size() &&
                        deadlines_[right]->deadline < deadlines_[left]->deadline
                    ? right : left;
            if (deadlines_[index]->deadline <=
                deadlines_[earliest]->deadline) return;
            swap_deadlines(index, earliest);
            index = earliest;
        }
    }

    void remove_deadline(ConnectionState& connection) noexcept {
        const std::size_t index = connection.deadline_index;
        if (index == ConnectionState::no_deadline) return;
        connection.deadline_index = ConnectionState::no_deadline;
        const std::size_t last = deadlines_.size() - 1;
        if (index == last) {
            deadlines_.pop_back();
            return;
        }
        deadlines_[index] = deadlines_[last];
        deadlines_[index]->deadline_index = index;
        deadlines_.pop_back();
        if (index != 0 && deadlines_[index]->deadline <
                              deadlines_[(index - 1) / 2]->deadline)
            sift_deadline_up(index);
        else
            sift_deadline_down(index);
    }

    void expire_deadlines(std::chrono::steady_clock::time_point now) {
        while (!deadlines_.empty() && deadlines_.front()->deadline <= now) {
            ConnectionState* connection = deadlines_.front();
            const OperationType operation = connection->deadline_operation;
            const auto deadline = connection->deadline;
            remove_deadline(*connection);
            if (connection->closed || !connection->pending ||
                connection->cancel_requested ||
                connection->pending->operation != operation) {
                continue;
            }
            if (request_cancel(connection->pending)) {
                connection->cancel_requested = true;
            } else {
                // Preserve an expired deadline if the submission queue is
                // temporarily full. The next event-loop pass retries it.
                auto owner = connections_.find(connection->fd);
                if (owner != connections_.end())
                    set_deadline(owner->second.get(), operation, deadline);
                break;
            }
        }
    }

    [[nodiscard]] __kernel_timespec next_wait_timeout(
        std::chrono::steady_clock::time_point now) const noexcept {
        constexpr auto maximum_poll = std::chrono::milliseconds(25);
        auto wait = std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(maximum_poll);
        if (!deadlines_.empty()) {
            const auto until_deadline = deadlines_.front()->deadline - now;
            if (until_deadline < wait) wait = until_deadline;
        }
        if (wait <= std::chrono::steady_clock::duration::zero())
            return {0, 1};
        const auto nanoseconds =
            std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count();
        return {nanoseconds / 1'000'000'000,
                nanoseconds % 1'000'000'000};
    }

    bool reserve_sqes(unsigned count) {
        if (io_uring_sq_space_left(&ring_) >= count) return true;
        flush_submissions();
        return io_uring_sq_space_left(&ring_) >= count;
    }

    bool queue_accept() {
        if (shutting_down_ || !reserve_sqes(1)) return false;
        AsyncContext* context = context_pool_.acquire(OperationType::Accept);
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (multishot_accept_) {
            io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr,
                                            SOCK_NONBLOCK | SOCK_CLOEXEC);
        } else {
            io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        }
        io_uring_sqe_set_data(sqe, context);
        accept_context_ = context;
        ++outstanding_;
        return true;
    }

    bool queue_wake_poll() {
        if (wake_context_ || !reserve_sqes(1)) return false;
        AsyncContext* context = context_pool_.acquire(OperationType::Wake);
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_poll_add(sqe, wake_fd_, POLLIN);
        io_uring_sqe_set_data(sqe, context);
        wake_context_ = context;
        ++outstanding_;
        return true;
    }

    void queue_accept_retry() {
        if (shutting_down_ || !reserve_sqes(1)) return;
        AsyncContext* context = context_pool_.acquire(OperationType::AcceptRetry);
        context->timeout = {0, 50'000'000};
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_timeout(sqe, &context->timeout, 0, 0);
        io_uring_sqe_set_data(sqe, context);
        ++outstanding_;
    }

    bool queue_read(ConnectionState* connection) {
        if (connection->closed || !reserve_sqes(1)) return false;
        AsyncContext* context = context_pool_.acquire(
            OperationType::Read, connection);
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_recv(sqe, connection->fd, connection->read_buffer.data(),
                           connection->read_buffer.size(), 0);
        io_uring_sqe_set_data(sqe, context);
        connection->pending = context;
        ++outstanding_;
        return true;
    }

    bool queue_write(ConnectionState* connection) {
        if (connection->closed || !reserve_sqes(1)) return false;
        prepare_write_message(*connection);
        AsyncContext* context = context_pool_.acquire(
            OperationType::Write, connection);
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_sendmsg(sqe, connection->fd,
                              &connection->write_message, MSG_NOSIGNAL);
        io_uring_sqe_set_data(sqe, context);
        connection->pending = context;
        ++outstanding_;
        return true;
    }

    bool request_cancel(AsyncContext* target) {
        if (!target || !reserve_sqes(1)) return false;
        AsyncContext* context = context_pool_.acquire(OperationType::Cancel);
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_cancel(sqe, target, 0);
        io_uring_sqe_set_data(sqe, context);
        ++outstanding_;
        return true;
    }

    void prepare_write_message(ConnectionState& connection) noexcept {
        const std::size_t header_size = connection.response_headers.size();
        unsigned count = 0;
        if (connection.write_offset < header_size) {
            connection.write_iov[count++] = {
                connection.response_headers.data() + connection.write_offset,
                header_size - connection.write_offset};
            if (!connection.response_body.empty()) {
                connection.write_iov[count++] = {
                    const_cast<char*>(connection.response_body.data()),
                    connection.response_body.size()};
            }
        } else {
            const std::size_t body_offset = connection.write_offset - header_size;
            if (body_offset < connection.response_body.size()) {
                connection.write_iov[count++] = {
                    const_cast<char*>(connection.response_body.data()) + body_offset,
                    connection.response_body.size() - body_offset};
            }
        }
        connection.write_message = {};
        connection.write_message.msg_iov = connection.write_iov.data();
        connection.write_message.msg_iovlen = count;
    }

    void handle_completion(io_uring_cqe* cqe) {
        AsyncContext* context = static_cast<AsyncContext*>(
            io_uring_cqe_get_data(cqe));
        const int result = cqe->res;
        const unsigned flags = cqe->flags;
        io_uring_cqe_seen(&ring_, cqe);
        if (!context) return;

        const bool still_active =
            context->operation == OperationType::Accept &&
            (flags & IORING_CQE_F_MORE) != 0;
        if (!still_active) {
            if (context == accept_context_) accept_context_ = nullptr;
            if (context == wake_context_) wake_context_ = nullptr;
            if (context->connection && context->connection->pending == context) {
                context->connection->pending = nullptr;
                context->connection->cancel_requested = false;
            }
            if (outstanding_ > 0) --outstanding_;
        }

        dispatch_event(context, result, still_active);
        if (!still_active) context_pool_.release(context);
    }

    void begin_shutdown() noexcept {
        if (shutting_down_) return;
        shutting_down_ = true;
        shutdown_deadline_ = std::chrono::steady_clock::now() +
                             limits_.shutdown_timeout;
    }

    void update_deadlines_and_shutdown() {
        if (stop_requested()) begin_shutdown();
        const auto now = std::chrono::steady_clock::now();

        expire_deadlines(now);

        if (shutting_down_ && accept_context_ && !accept_cancel_requested_) {
            if (request_cancel(accept_context_)) accept_cancel_requested_ = true;
        }
        if (shutting_down_ && handler_jobs_ == 0 && wake_context_ &&
            !wake_cancel_requested_) {
            if (request_cancel(wake_context_)) wake_cancel_requested_ = true;
        }

        // Shutdown is cold-path work. During normal operation deadlines are
        // driven by the heap above instead of scanning every connection on
        // every completion-loop pass.
        if (!shutting_down_) return;
        for (auto& [fd, connection] : connections_) {
            (void)fd;
            if (!connection->pending || connection->cancel_requested) continue;
            const bool reading = connection->pending->operation == OperationType::Read;
            const bool idle_during_shutdown =
                shutting_down_ && reading && connection->available_input() == 0;
            const bool shutdown_expired =
                shutting_down_ && now >= shutdown_deadline_;
            if ((idle_during_shutdown || shutdown_expired) &&
                request_cancel(connection->pending)) {
                connection->cancel_requested = true;
            }
        }
    }

    void close_connection(ConnectionState* connection) noexcept {
        if (!connection || connection->closed) return;
        connection->closed = true;
        remove_deadline(*connection);
        ::close(connection->fd);
        connections_.erase(connection->fd);
        if (global_connections_)
            global_connections_->fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool response_closes(
        const HttpResponse& response) const noexcept {
        for (const auto& [name, value] : response.headers) {
            if (CaseInsensitiveEqual{}(name, "connection") &&
                HttpParser::contains_token(value, "close")) return true;
        }
        return false;
    }

    static void store_response_body(ConnectionState& connection,
                                    HttpResponse& response,
                                    bool suppress_body) {
        connection.owned_body.clear();
        connection.response_body = {};
        if (suppress_body || response.status_code == 204 ||
            response.status_code == 304) return;

        if (response.body_type == HttpResponse::BodyType::Mapped) {
            connection.response_body = response.active_body();
        } else if (response.body_type == HttpResponse::BodyType::Owned) {
            connection.owned_body = std::move(response.owned_body);
            connection.response_body = connection.owned_body;
        } else {
            connection.owned_body = std::move(response.body);
            connection.response_body = connection.owned_body;
        }
    }

    void begin_response(ConnectionState* connection,
                        HttpResponse& response, bool suppress_body,
                        bool close_after_write) {
        connection->close_after_write = close_after_write;
        connection->response_headers = response.serialize_headers(close_after_write);
        store_response_body(*connection, response, suppress_body);
        connection->write_offset = 0;
        connection->write_size = connection->response_headers.size() +
                                 connection->response_body.size();
        connection->write_deadline = std::chrono::steady_clock::now() +
                                     limits_.write_timeout;
        set_deadline(connection, OperationType::Write,
                     connection->write_deadline);
        if (!queue_write(connection)) close_connection(connection);
    }

    void send_error(ConnectionState* connection,
                    int status) {
        HttpResponse response;
        response.status(status).text(get_status_text_sv(status));
        connection->input.clear();
        connection->read_position = 0;
        begin_response(connection, response, false, true);
    }

    void compact_input(ConnectionState& connection) {
        if (connection.read_position == 0) return;
        if (connection.read_position == connection.input.size()) {
            connection.input.clear();
            connection.read_position = 0;
        } else if (connection.read_position >= 4096 ||
                   connection.read_position * 2 >= connection.input.size()) {
            connection.input.erase(0, connection.read_position);
            connection.read_position = 0;
        }
    }

    detail::BoundedExecutionQueue* executor_for(
        const HandlerExecution& execution) {
        std::string key;
        std::size_t threads = 0;
        std::size_t capacity = 0;
        if (execution.mode == HandlerExecutionMode::SharedBlocking) {
            key = "\x1fshared";
            threads = execution_options_.shared_threads_per_shard;
            capacity = execution_options_.shared_capacity;
        } else if (execution.mode == HandlerExecutionMode::Named) {
            key = "\x1fnamed:" + execution.queue_name;
            for (const auto& configured : execution_options_.named) {
                if (configured.name == execution.queue_name) {
                    threads = configured.threads_per_shard;
                    capacity = configured.capacity;
                    break;
                }
            }
            if (threads == 0 || capacity == 0) return nullptr;
        } else {
            return nullptr;
        }

        auto existing = executors_.find(key);
        if (existing != executors_.end()) return existing->second.get();
        auto executor = std::make_unique<detail::BoundedExecutionQueue>(
            threads, capacity);
        auto* result = executor.get();
        executors_.emplace(std::move(key), std::move(executor));
        return result;
    }

    void post_handler_completion(HandlerCompletion completion) {
        {
            std::lock_guard lock(completion_mutex_);
            handler_completions_.push_back(std::move(completion));
        }
        std::uint64_t signal = 1;
        while (::write(wake_fd_, &signal, sizeof(signal)) < 0 &&
               errno == EINTR) {}
    }

    void execute_offloaded_request(std::string raw_request,
                                   int fd,
                                   std::uint64_t connection_id,
                                   bool suppress_body,
                                   bool base_close) {
        HandlerCompletion completion;
        completion.fd = fd;
        completion.connection_id = connection_id;
        completion.suppress_body = suppress_body;
        completion.base_close = base_close;
        try {
            const std::size_t header_end = raw_request.find("\r\n\r\n");
            if (header_end == std::string::npos) throw HttpParseError(400);
            const std::size_t header_size = header_end + 4;
            HttpRequest request = HttpParser::parse_headers(
                std::string_view(raw_request).substr(0, header_size), limits_);
            HttpParser::set_body(request, std::string_view(raw_request).substr(
                header_size, request.content_length));

            const HandlerRoute* route = nullptr;
            if (!router_ || !router_->match(request, route)) {
                completion.response.status(404).text("Not Found");
            } else {
                route->handler(request, completion.response);
            }
        } catch (...) {
            completion.response = HttpResponse{};
            completion.response.status(500).text("Internal Server Error");
            completion.handler_failed = true;
        }
        post_handler_completion(std::move(completion));
    }

    bool queue_handler(ConnectionState* connection,
                       const HandlerExecution& execution,
                       std::string raw_request,
                       bool suppress_body,
                       bool base_close) {
        detail::BoundedExecutionQueue* executor = nullptr;
        try {
            executor = executor_for(execution);
        } catch (...) {
            return false;
        }
        if (!executor) return false;

        connection->handler_pending = true;
        ++handler_jobs_;
        const int fd = connection->fd;
        const std::uint64_t connection_id = connection->id;
        const bool accepted = executor->submit(
            [this, raw = std::move(raw_request), fd, connection_id,
             suppress_body, base_close]() mutable {
                execute_offloaded_request(std::move(raw), fd, connection_id,
                                          suppress_body, base_close);
            });
        if (!accepted) {
            --handler_jobs_;
            connection->handler_pending = false;
        }
        return accepted;
    }

    void drain_handler_completions() {
        std::uint64_t wakeups = 0;
        while (::read(wake_fd_, &wakeups, sizeof(wakeups)) < 0 &&
               errno == EINTR) {}

        std::deque<HandlerCompletion> completed;
        {
            std::lock_guard lock(completion_mutex_);
            completed.swap(handler_completions_);
        }
        for (auto& completion : completed) {
            if (handler_jobs_ > 0) --handler_jobs_;
            auto position = connections_.find(completion.fd);
            if (position == connections_.end()) continue;
            ConnectionState* connection = position->second.get();
            if (connection->id != completion.connection_id ||
                !connection->handler_pending) continue;
            connection->handler_pending = false;
            const bool close_after_write =
                shutting_down_ || completion.base_close ||
                completion.handler_failed ||
                response_closes(completion.response);
            try {
                begin_response(connection, completion.response,
                               completion.suppress_body, close_after_write);
            } catch (...) {
                send_error(connection, 500);
            }
        }
    }

    void process_input(ConnectionState* connection) {
        const std::string_view available = connection->input_view();
        const std::size_t header_end = available.find("\r\n\r\n");
        if (header_end == std::string_view::npos) {
            if (available.size() > limits_.max_header_bytes)
                send_error(connection, 431);
            else if (!queue_read(connection))
                close_connection(connection);
            return;
        }

        const std::size_t header_size = header_end + 4;
        if (header_size > limits_.max_header_bytes) {
            send_error(connection, 431);
            return;
        }

        try {
            HttpRequest request = HttpParser::parse_headers(
                available.substr(0, header_size), limits_);
            const std::size_t request_size = header_size + request.content_length;
            if (available.size() < request_size) {
                if (!queue_read(connection)) close_connection(connection);
                return;
            }
            HttpParser::set_body(request, available.substr(
                header_size, request.content_length));

            // WebSocket Upgrade Detection
            const WebSocketConfig* ws_config = router_ ? router_->find_ws(request.path) : nullptr;
            if (ws_config && websocket::is_upgrade_request(request)) {
                HttpResponse ws_response;
                if (websocket::make_upgrade_response(request, ws_response)) {
                    connection->is_websocket_upgrade = true;
                    connection->active_ws_config = ws_config;
                    connection->read_position += request_size;
                    begin_response(connection, ws_response, false, false);
                    return;
                }
            }

            HttpResponse response;
            bool handler_failed = false;
            const HandlerRoute* route = nullptr;
            if (!router_ || !router_->match(request, route)) {
                response.status(404).text("Not Found");
            } else if (route->execution.mode !=
                       HandlerExecutionMode::Inline) {
                ++connection->request_count;
                const bool base_close =
                    shutting_down_ || !request.keep_alive ||
                    connection->request_count >=
                        limits_.max_requests_per_connection;
                std::string owned_request(available.substr(0, request_size));
                connection->read_position += request_size;
                remove_deadline(*connection);
                if (!queue_handler(connection, route->execution,
                                   std::move(owned_request),
                                   request.method == HttpMethod::HEAD,
                                   base_close)) {
                    send_error(connection, 503);
                }
                return;
            } else {
                try {
                    route->handler(request, response);
                } catch (...) {
                    response = HttpResponse{};
                    response.status(500).text("Internal Server Error");
                    handler_failed = true;
                }
            }

            ++connection->request_count;
            const bool close_after_write =
                shutting_down_ || handler_failed || !request.keep_alive ||
                response_closes(response) ||
                connection->request_count >= limits_.max_requests_per_connection;
            connection->read_position += request_size;
            begin_response(connection, response,
                           request.method == HttpMethod::HEAD,
                           close_after_write);
        } catch (const HttpParseError& error) {
            send_error(connection, error.status);
        } catch (...) {
            send_error(connection, 500);
        }
    }

    void start_websocket(ConnectionState* connection) {
        connection->is_websocket_upgrade = false;
        connection->is_websocket = true;
        connection->response_headers.clear();
        connection->owned_body.clear();
        connection->response_body = {};
        connection->write_offset = 0;
        connection->write_size = 0;

        const int fd = connection->fd;
        connection->ws_session = std::make_shared<websocket::WebSocketSession>(
            connection->active_ws_config ? *connection->active_ws_config : WebSocketConfig{},
            [this, fd](std::string frame) {
                auto it = connections_.find(fd);
                if (it != connections_.end() && !it->second->closed) {
                    it->second->ws_write_queue.push_back(std::move(frame));
                    flush_websocket_writes(it->second.get());
                }
            },
            [this, fd](websocket::CloseCode, std::string_view) {
                auto it = connections_.find(fd);
                if (it != connections_.end() && !it->second->closed) {
                    if (it->second->ws_write_queue.empty() && !it->second->ws_writing) {
                        close_connection(it->second.get());
                    } else {
                        it->second->close_after_write = true;
                    }
                }
            }
        );

        connection->ws_session->on_open();
        process_websocket_input(connection);
    }

    void process_websocket_input(ConnectionState* connection) {
        if (!connection || connection->closed || !connection->ws_session) return;

        if (connection->available_input() > 0) {
            std::string unconsumed(connection->input_view());
            connection->input.clear();
            connection->read_position = 0;
            connection->ws_session->feed(unconsumed);
        }

        if (!connection->closed && connection->ws_session->is_open() && !connection->pending) {
            if (!queue_read(connection)) {
                close_connection(connection);
            }
        }
    }

    void flush_websocket_writes(ConnectionState* connection) {
        if (!connection || connection->closed || connection->ws_writing ||
            connection->ws_write_queue.empty() || connection->pending) return;

        connection->ws_writing = true;
        connection->response_headers = std::move(connection->ws_write_queue.front());
        connection->ws_write_queue.pop_front();
        connection->response_body = {};
        connection->owned_body.clear();
        connection->write_offset = 0;
        connection->write_size = connection->response_headers.size();

        connection->write_deadline = std::chrono::steady_clock::now() + limits_.write_timeout;
        set_deadline(connection, OperationType::Write, connection->write_deadline);
        if (!queue_write(connection)) {
            close_connection(connection);
        }
    }

    void admit_connection(int fd) {
        bool admitted = !shutting_down_;
        if (admitted && global_connections_) {
            const std::size_t previous = global_connections_->fetch_add(
                1, std::memory_order_relaxed);
            if (previous >= limits_.max_connections) {
                global_connections_->fetch_sub(1, std::memory_order_relaxed);
                admitted = false;
            }
        }
        if (!admitted) {
            ::close(fd);
            return;
        }

        int enabled = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                       &enabled, sizeof(enabled)) < 0) {
            if (global_connections_)
                global_connections_->fetch_sub(1, std::memory_order_relaxed);
            ::close(fd);
            return;
        }

        auto owned_connection = std::make_unique<ConnectionState>();
        owned_connection->fd = fd;
        owned_connection->id = next_connection_id_++;
        owned_connection->input.reserve(4096);
        auto [position, inserted] = connections_.emplace(
            fd, std::move(owned_connection));
        if (!inserted) {
            if (global_connections_)
                global_connections_->fetch_sub(1, std::memory_order_relaxed);
            ::close(fd);
            return;
        }
        ConnectionState* connection = position->second.get();
        connection->read_deadline = std::chrono::steady_clock::now() +
                                    limits_.read_timeout;
        set_deadline(connection, OperationType::Read,
                     connection->read_deadline);
        if (!queue_read(connection)) close_connection(connection);
    }

    void dispatch_event(AsyncContext* context, int result, bool still_active) {
        switch (context->operation) {
            case OperationType::Accept:
                if (result >= 0) admit_connection(result);
                if (!still_active && !shutting_down_) {
                    if (result == -EINVAL && multishot_accept_) {
                        multishot_accept_ = false;
                        queue_accept();
                    } else if (result < 0 && result != -ECANCELED) {
                        queue_accept_retry();
                    } else {
                        queue_accept();
                    }
                }
                break;
            case OperationType::AcceptRetry:
                if (!shutting_down_) queue_accept();
                break;
            case OperationType::Read: {
                ConnectionState* connection = context->connection;
                if (!connection || connection->closed) break;
                if (result <= 0) {
                    close_connection(connection);
                    break;
                }
                compact_input(*connection);
                connection->input.append(connection->read_buffer.data(),
                                         static_cast<std::size_t>(result));
                if (connection->is_websocket) {
                    process_websocket_input(connection);
                } else {
                    process_input(connection);
                }
                break;
            }
            case OperationType::Write: {
                ConnectionState* connection = context->connection;
                if (!connection || connection->closed) break;
                if (result <= 0) {
                    close_connection(connection);
                    break;
                }
                connection->write_offset += static_cast<std::size_t>(result);
                if (connection->write_offset < connection->write_size) {
                    if (!queue_write(connection)) close_connection(connection);
                } else if (connection->close_after_write || shutting_down_) {
                    close_connection(connection);
                } else if (connection->is_websocket_upgrade) {
                    start_websocket(connection);
                } else if (connection->is_websocket) {
                    connection->ws_writing = false;
                    connection->response_headers.clear();
                    connection->write_offset = 0;
                    connection->write_size = 0;
                    if (!connection->ws_write_queue.empty()) {
                        flush_websocket_writes(connection);
                    } else if (connection->close_after_write) {
                        close_connection(connection);
                    } else if (!connection->pending && connection->ws_session && connection->ws_session->is_open()) {
                        if (!queue_read(connection)) close_connection(connection);
                    }
                } else {
                    connection->response_headers.clear();
                    connection->owned_body.clear();
                    connection->response_body = {};
                    connection->write_offset = 0;
                    connection->write_size = 0;
                    connection->read_deadline =
                        std::chrono::steady_clock::now() + limits_.read_timeout;
                    set_deadline(connection, OperationType::Read,
                                 connection->read_deadline);
                    compact_input(*connection);
                    if (connection->available_input() == 0) {
                        if (!queue_read(connection)) close_connection(connection);
                    } else {
                        process_input(connection);
                    }
                }
                break;
            }
            case OperationType::Wake:
                if (result >= 0) {
                    drain_handler_completions();
                } else if (result != -ECANCELED) {
                    throw std::runtime_error("Handler wakeup poll failed: " +
                                             std::string(strerror(-result)));
                }
                if (!shutting_down_ || handler_jobs_ != 0) {
                    if (!queue_wake_poll())
                        throw std::runtime_error(
                            "Failed to requeue handler wakeup poll");
                }
                break;
            case OperationType::Cancel:
                break;
        }
    }
};

} // namespace octane::transport
