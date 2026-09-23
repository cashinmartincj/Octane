#pragma once

#include "HttpLimits.h"
#include "IoUringContext.h"
#include "Router.h"
#include "ExecutionQueue.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
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

struct TcpServerOptions {
    unsigned int ring_queue_depth{256};
    bool pin_workers{true};
    std::string bind_address{"0.0.0.0"};
    ExecutionQueueOptions execution_queues{};

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

class TcpServer {
public:
    TcpServer(std::shared_ptr<octane::Router> router, const HttpLimits& limits,
              const TcpServerOptions& options = {})
        : router_(std::move(router)), limits_(limits), options_(options) {
        limits_.validate();
        options_.validate();
    }

    TcpServer(octane::Router& router, const HttpLimits& limits,
              const TcpServerOptions& options = {})
        : router_(std::shared_ptr<octane::Router>(&router, [](octane::Router*) {})),
          limits_(limits), options_(options) {
        limits_.validate();
        options_.validate();
    }

    void stop() noexcept {
        stop_requested_->store(true, std::memory_order_release);
    }

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
                std::cout << "\033[1;32m● Octane (io_uring) listening on port "
                          << port_ << "\033[0m" << std::endl;
            } else {
                // Keep redirected logs machine-readable and free of ANSI bytes.
                std::cout << "Octane (io_uring) listening on port "
                          << port_ << std::endl;
            }
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
    int port_{0};
    size_t num_threads_{1};
    std::shared_ptr<octane::Router> router_;
    HttpLimits limits_;
    TcpServerOptions options_;
    std::vector<std::thread> workers_;
    std::shared_ptr<std::atomic_size_t> active_connections_ =
        std::make_shared<std::atomic_size_t>(0);
    std::shared_ptr<std::atomic_bool> stop_requested_ =
        std::make_shared<std::atomic_bool>(false);
    inline static std::atomic_bool signal_owner_{false};
    std::mutex worker_error_mutex_;
    std::exception_ptr worker_error_;

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

    static void restore_signal_mask(const sigset_t& previous_mask) noexcept {
        pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        signal_owner_.store(false, std::memory_order_release);
    }

    static int socket_port(int fd) {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0)
            throw std::runtime_error("getsockname failed: " +
                                     std::string(std::strerror(errno)));
        return ntohs(address.sin_port);
    }

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

    void run_worker(int server_fd, int cpu) noexcept {
        try {
            pin_current_worker(cpu);
            IoUringContext ring_context(server_fd, router_, limits_,
                                        active_connections_, stop_requested_.get(),
                                        options_.ring_queue_depth,
                                        options_.execution_queues);
            ring_context.run();
        } catch (const std::exception& error) {
            std::cerr << "io_uring worker stopped: " << error.what() << '\n';
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
        ::close(server_fd);
    }
};

} // namespace octane::transport
