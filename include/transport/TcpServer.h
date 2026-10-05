/**
 * @file TcpServer.h
 * @brief Multi-threaded Linux io_uring TCP server with SO_REUSEPORT and CPU core affinity pinning.
 *
 * @details
 * `TcpServer` orchestrates the multi-worker runtime of Octane. It maximizes multi-core CPU utilization
 * on modern Linux kernels through several architectural techniques:
 *
 * Architectural Features:
 * - **SO_REUSEPORT Multi-Socket Sharding:** Each worker thread creates its own listening socket file
 *   descriptor bound to the same port. The Linux kernel's BPF-driven socket hash distributes incoming
 *   TCP SYN handshakes across workers with zero user-space lock contention.
 * - **CPU Core Pinning (`pthread_setaffinity_np`):** When `options.pin_workers` is enabled, each worker
 *   thread is pinned to a dedicated CPU core derived from `sched_getaffinity`. This guarantees that
 *   network packet processing, user callbacks, and L1/L2 CPU caches remain localized on the core,
 *   completely avoiding cross-NUMA bus traffic.
 * - **Per-Thread IoUringContext:** Each worker runs an independent `IoUringContext` with its own `io_uring`
 *   ring and dedicated `ExecutionQueue` thread pools.
 * - **Clean Signal Lifecycle Management:** Uses `pthread_sigmask` to block `SIGINT` and `SIGTERM` in
 *   worker threads and dedicates a monitor thread (`sigtimedwait`) to perform clean shutdown and socket drainage.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "transport/TcpServer.h"`
 * - Application Core: Instantiated inside `octane::init::listen` (`include/Octane.h`) to boot the web application.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once

#include "HttpLimits.h"
#include "IoUringContext.h"
#include "EpollContext.h"
#include "Router.h"
#include "ExecutionQueue.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <print>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace octane::transport {

enum class TransportBackend { Automatic, IoUring, Epoll };

/**
 * @struct TcpServerOptions
 * @brief Runtime options configuring Linux kernel io_uring, worker pinning, and execution pools.
 */
struct TcpServerOptions {
    /// Automatic probes io_uring once and falls back to epoll when unavailable.
    TransportBackend transport_backend{TransportBackend::Automatic};
    /// Depth of the io_uring submission and completion queues per worker (default: 256)
    unsigned int ring_queue_depth{256};

    /// When true, binds each worker thread to a dedicated physical CPU core (default: true)
    bool pin_workers{true};

    /// IPv4 network interface address to bind (default: "0.0.0.0")
    std::string bind_address{"0.0.0.0"};

    /// Offloaded worker thread pool options for blocking I/O and background jobs
    ExecutionQueueOptions execution_queues{};

    /**
     * @brief Validates options settings and bounds.
     * @throws std::invalid_argument If queue depth is < 8, IP address is invalid, or queues are misconfigured.
     */
    void validate() const {
        if (ring_queue_depth < 8)
            throw std::invalid_argument("io_uring queue depth must be at least 8");
        in_addr parsed_address{};
        if (bind_address.empty() ||
            inet_pton(AF_INET, bind_address.c_str(), &parsed_address) != 1)
            throw std::invalid_argument("bind_address must be a valid IPv4 address");
        if (!execution_queues.shared_threads_per_shard ||
            !execution_queues.shared_capacity)
            throw std::invalid_argument("Invalid shared execution queue configuration");
        std::unordered_set<std::string> names;
        for (const auto& queue : execution_queues.named) {
            if (queue.name.empty() || !queue.threads_per_shard ||
                !queue.capacity || !names.insert(queue.name).second)
                throw std::invalid_argument("Invalid named execution queue configuration");
        }
    }
};

/**
 * @class TcpServer
 * @brief Multi-worker HTTP server managing listening sockets, CPU affinity, and worker loops.
 */
class TcpServer {
public:
    /**
     * @brief Constructs a TcpServer with a shared Router.
     * @param router Shared pointer to application route table.
     * @param limits Protocol limits and boundaries.
     * @param options Tuning and queue options.
     */
    TcpServer(std::shared_ptr<octane::Router> router, const HttpLimits& limits,
              const TcpServerOptions& options = {})
        : router_(std::move(router)), limits_(limits), options_(options) {
        limits_.validate();
        options_.validate();
    }

    /**
     * @brief Constructs a TcpServer referencing an existing Router.
     * @param router Reference to application route table.
     * @param limits Protocol limits and boundaries.
     * @param options Tuning and queue options.
     */
    TcpServer(octane::Router& router, const HttpLimits& limits,
              const TcpServerOptions& options = {})
        : router_(std::shared_ptr<octane::Router>(&router, [](octane::Router*) {})),
          limits_(limits), options_(options) {
        limits_.validate();
        options_.validate();
    }

    /**
     * @brief Requests graceful termination of all server worker loops.
     */
    void stop() noexcept {
        stop_requested_->store(true, std::memory_order_release);
    }

    /**
     * @brief Starts the server listening on the specified port across worker threads.
     * @details Blocks the calling thread until SIGINT / SIGTERM is received or stop() is invoked.
     * @param port TCP port to bind (e.g. 8080). If 0, an ephemeral OS port is selected.
     * @param num_threads Number of worker threads / io_uring rings to spawn.
     * @throws std::invalid_argument If port or thread count is invalid.
     * @throws std::runtime_error If socket creation, bind, or CPU pinning fails.
     */
    void listen(int port, size_t num_threads) {
        if (port < 0 || port > 65535 || num_threads == 0)
            throw std::invalid_argument("Invalid listen configuration");

        bool expected = false;
        if (!signal_owner_.compare_exchange_strong(expected, true))
            throw std::runtime_error(
                "Only one signal-managed TcpServer may listen per process");

        port_ = port;
        num_threads_ = num_threads;
        stop_requested_->store(false, std::memory_order_release);
        worker_error_ = nullptr;
        active_connections_->store(0, std::memory_order_relaxed);
        selected_backend_ = select_backend(num_threads_);

        sigset_t shutdown_signals;
        sigemptyset(&shutdown_signals);
        sigaddset(&shutdown_signals, SIGINT);
        sigaddset(&shutdown_signals, SIGTERM);
        sigset_t previous_mask;
        const int mask_result = pthread_sigmask(
            SIG_BLOCK, &shutdown_signals, &previous_mask);
        if (mask_result != 0) {
            signal_owner_.store(false, std::memory_order_release);
            throw std::runtime_error("Failed to block shutdown signals: " +
                                     std::string(std::strerror(mask_result)));
        }

        std::vector<int> listening_sockets;
        std::thread signal_waiter;

        try {
            signal_waiter = std::thread(
                [stop = stop_requested_, shutdown_signals]() mutable {
                    wait_for_shutdown_signal(stop, shutdown_signals);
                });

            listening_sockets.reserve(num_threads_);
            for (size_t i = 0; i < num_threads_; ++i) {
                int fd = create_listening_socket(port_, options_.bind_address);
                listening_sockets.push_back(fd);
                if (i == 0 && port_ == 0) port_ = socket_port(fd);
            }

            if (::isatty(STDOUT_FILENO)) {
                std::println("\033[1;32m● Octane ({}) listening on port {}\033[0m", backend_name(), port_);
            } else {
                // Keep redirected logs machine-readable and free of ANSI bytes.
                std::println("Octane ({}) listening on port {}", backend_name(), port_);
            }
            std::fflush(stdout);
            const std::vector<int> worker_cpus =
                options_.pin_workers ? available_cpus() : std::vector<int>{};
            workers_.reserve(num_threads_);
            for (std::size_t i = 0; i < listening_sockets.size(); ++i) {
                const int fd = listening_sockets[i];
                const int cpu = worker_cpus.empty()
                    ? -1 : worker_cpus[i % worker_cpus.size()];
                workers_.emplace_back(
                    [this, fd, cpu]() { run_worker(fd, cpu); });
            }
            listening_sockets.clear();

            for (auto& worker : workers_)
                if (worker.joinable()) worker.join();
            workers_.clear();
            stop_requested_->store(true, std::memory_order_release);
            if (signal_waiter.joinable()) signal_waiter.join();
        } catch (...) {
            stop_requested_->store(true, std::memory_order_release);
            for (int fd : listening_sockets) ::close(fd);
            for (auto& worker : workers_)
                if (worker.joinable()) worker.join();
            workers_.clear();
            if (signal_waiter.joinable()) signal_waiter.join();
            restore_signal_mask(previous_mask);
            throw;
        }
        restore_signal_mask(previous_mask);
        if (worker_error_) std::rethrow_exception(worker_error_);
    }

private:
    int port_{0};                                      ///< Bound TCP port number
    size_t num_threads_{1};                           ///< Active worker thread count
    std::shared_ptr<octane::Router> router_;          ///< Application route table
    HttpLimits limits_;                               ///< Protocol boundary constraints
    TcpServerOptions options_;                        ///< Server configuration options
    std::vector<std::thread> workers_;                 ///< Active worker threads
    std::shared_ptr<std::atomic_size_t> active_connections_ =
        std::make_shared<std::atomic_size_t>(0);      ///< Concurrent connection counter
    std::shared_ptr<std::atomic_bool> stop_requested_ =
        std::make_shared<std::atomic_bool>(false);    ///< Shutdown notification flag
    inline static std::atomic_bool signal_owner_{false}; ///< Single server signal lock
    std::mutex worker_error_mutex_;                   ///< Protects worker_error_
    std::exception_ptr worker_error_;                 ///< Captures worker thread exceptions
    TransportBackend selected_backend_{TransportBackend::IoUring};

    TransportBackend select_backend(std::size_t worker_count) const {
        if (options_.transport_backend != TransportBackend::Automatic)
            return options_.transport_backend;
        if (const char* requested = std::getenv("OCTANE_TRANSPORT")) {
            const std::string_view value(requested);
            if (value == "epoll") return TransportBackend::Epoll;
            if (value == "io_uring") return TransportBackend::IoUring;
            if (!value.empty() && value != "auto")
                throw std::invalid_argument(
                    "OCTANE_TRANSPORT must be auto, io_uring, or epoll");
        }
        std::vector<io_uring> probes(worker_count);
        std::size_t initialized = 0;
        int result = 0;
        for (; initialized < probes.size(); ++initialized) {
            result = io_uring_queue_init(options_.ring_queue_depth,
                                         &probes[initialized], 0);
            if (result < 0) break;
        }
        for (std::size_t i = 0; i < initialized; ++i)
            io_uring_queue_exit(&probes[i]);
        if (result == 0) return TransportBackend::IoUring;
        std::println(stderr, "io_uring unavailable ({}); falling back to epoll",
                     std::strerror(-result));
        return TransportBackend::Epoll;
    }

    const char* backend_name() const noexcept {
        return selected_backend_ == TransportBackend::IoUring ? "io_uring" : "epoll";
    }

    /**
     * @brief Dedicated thread waiting synchronously for SIGINT or SIGTERM via sigtimedwait.
     */
    static void wait_for_shutdown_signal(
        const std::shared_ptr<std::atomic_bool>& stop,
        sigset_t shutdown_signals) noexcept {
        while (!stop->load(std::memory_order_acquire)) {
            timespec timeout{0, 50'000'000};
            const int result = sigtimedwait(
                &shutdown_signals, nullptr, &timeout);
            if (result == SIGINT || result == SIGTERM) {
                stop->store(true, std::memory_order_release);
                return;
            }
            if (result < 0 && errno != EAGAIN && errno != EINTR) {
                stop->store(true, std::memory_order_release);
                return;
            }
        }
    }

    /**
     * @brief Restores process signal mask to pre-listen state.
     */
    static void restore_signal_mask(const sigset_t& previous_mask) noexcept {
        pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        signal_owner_.store(false, std::memory_order_release);
    }

    /**
     * @brief Queries OS assigned port for an open socket file descriptor.
     */
    static int socket_port(int fd) {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0)
            throw std::runtime_error("getsockname failed: " +
                                     std::string(std::strerror(errno)));
        return ntohs(address.sin_port);
    }

    /**
     * @brief Creates a non-blocking TCP socket configured with SO_REUSEADDR and SO_REUSEPORT.
     */
    static int create_listening_socket(int port, const std::string& bind_address) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            throw std::runtime_error("socket failed: " +
                                     std::string(std::strerror(errno)));

        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) < 0) {
            const int error = errno;
            ::close(fd);
            throw std::runtime_error("setsockopt failed: " +
                                     std::string(std::strerror(error)));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
            ::close(fd);
            throw std::invalid_argument("bind_address must be a valid IPv4 address");
        }
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            const int error = errno;
            ::close(fd);
            throw std::runtime_error("bind failed: " +
                                     std::string(std::strerror(error)));
        }
        if (::listen(fd, SOMAXCONN) < 0) {
            const int error = errno;
            ::close(fd);
            throw std::runtime_error("listen failed: " +
                                     std::string(std::strerror(error)));
        }
        return fd;
    }

    /**
     * @brief Discovers CPU cores permitted for this process via sched_getaffinity.
     */
    static std::vector<int> available_cpus() {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
            throw std::runtime_error("sched_getaffinity failed: " +
                                     std::string(std::strerror(errno)));

        std::vector<int> cpus;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
        }
        if (cpus.empty())
            throw std::runtime_error("No CPUs available for worker affinity");
        return cpus;
    }

    /**
     * @brief Pins the calling worker thread to a specific CPU core ID.
     */
    static void pin_current_worker(int cpu) {
        if (cpu < 0) return;
        cpu_set_t affinity;
        CPU_ZERO(&affinity);
        CPU_SET(cpu, &affinity);
        const int result = pthread_setaffinity_np(
            pthread_self(), sizeof(affinity), &affinity);
        if (result != 0)
            throw std::runtime_error("pthread_setaffinity_np failed: " +
                                     std::string(std::strerror(result)));
    }

    /**
     * @brief Worker thread entry function initializing affinity, IoUringContext, and event loop.
     */
    void run_worker(int server_fd, int cpu) noexcept {
        try {
            pin_current_worker(cpu);
            if (selected_backend_ == TransportBackend::IoUring) {
                IoUringContext ring_context(server_fd, router_, limits_,
                                            active_connections_, stop_requested_.get(),
                                            options_.ring_queue_depth,
                                            options_.execution_queues);
                ring_context.run();
            } else {
                EpollContext epoll_context(server_fd, router_, limits_,
                                           active_connections_, stop_requested_.get(),
                                           options_.execution_queues);
                epoll_context.run();
                server_fd = -1; // the Asio acceptor owns and closes the descriptor
            }
        } catch (const std::exception& error) {
            std::println(stderr, "{} worker stopped: {}", backend_name(), error.what());
            {
                std::lock_guard lock(worker_error_mutex_);
                if (!worker_error_) worker_error_ = std::current_exception();
            }
            stop_requested_->store(true, std::memory_order_release);
        } catch (...) {
            {
                std::lock_guard lock(worker_error_mutex_);
                if (!worker_error_) worker_error_ = std::current_exception();
            }
            stop_requested_->store(true, std::memory_order_release);
        }
        if (server_fd >= 0) ::close(server_fd);
    }
};

} // namespace octane::transport
