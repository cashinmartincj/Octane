#pragma once

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include "HttpLimits.h"
#include "Router.h"
#include "transport/TcpConnection.h"
#include "transport/ExecutionQueue.h"

namespace octane::transport {

// Asio uses epoll on Linux. This context owns one already-bound SO_REUSEPORT
// listener, matching the worker-per-core topology used by IoUringContext.
class EpollContext {
public:
    EpollContext(int server_fd,
                 std::shared_ptr<octane::Router> router,
                 const HttpLimits& limits,
                 std::shared_ptr<std::atomic_size_t> global_connections,
                 const std::atomic_bool* external_stop,
                 const ExecutionQueueOptions& execution_options)
        : server_fd_(server_fd), router_(std::move(router)), limits_(limits),
          global_connections_(std::move(global_connections)),
          external_stop_(external_stop), acceptor_(io_), stop_timer_(io_),
          execution_queues_(std::make_shared<EpollExecutionQueues>(execution_options)) {
        asio::error_code error;
        acceptor_.assign(asio::ip::tcp::v4(), server_fd_, error);
        if (error) throw std::runtime_error("Failed to initialize epoll listener: " + error.message());
    }

    void run() {
        accept_next();
        check_stop();
        io_.run();
    }

private:
    int server_fd_;
    std::shared_ptr<octane::Router> router_;
    HttpLimits limits_;
    std::shared_ptr<std::atomic_size_t> global_connections_;
    const std::atomic_bool* external_stop_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer stop_timer_;
    std::unordered_map<std::size_t, std::shared_ptr<TcpConnection>> connections_;
    std::size_t next_connection_id_{1};
    std::shared_ptr<EpollExecutionQueues> execution_queues_;

    void accept_next() {
        acceptor_.async_accept([this](const asio::error_code& error, asio::ip::tcp::socket socket) {
            if (!error && (!external_stop_ || !external_stop_->load(std::memory_order_acquire))) {
                const auto current = global_connections_->fetch_add(1, std::memory_order_acq_rel);
                if (current < limits_.max_connections) {
                    const auto id = next_connection_id_++;
                    auto connection = std::make_shared<TcpConnection>(
                        std::move(socket), *router_, limits_, id,
                        [this](std::size_t closed_id) {
                            connections_.erase(closed_id);
                            global_connections_->fetch_sub(1, std::memory_order_acq_rel);
                        }, execution_queues_);
                    connections_.emplace(id, connection);
                    connection->start();
                } else {
                    global_connections_->fetch_sub(1, std::memory_order_acq_rel);
                    asio::error_code ignored;
                    socket.close(ignored);
                }
            }
            if (acceptor_.is_open()) accept_next();
        });
    }

    void check_stop() {
        stop_timer_.expires_after(std::chrono::milliseconds(50));
        stop_timer_.async_wait([this](const asio::error_code& error) {
            if (error) return;
            if (external_stop_ && external_stop_->load(std::memory_order_acquire)) {
                asio::error_code ignored;
                acceptor_.close(ignored);
                std::vector<std::shared_ptr<TcpConnection>> closing;
                closing.reserve(connections_.size());
                for (auto& [id, connection] : connections_) closing.push_back(connection);
                for (auto& connection : closing) connection->stop(false);
                stop_timer_.expires_after(limits_.shutdown_timeout);
                stop_timer_.async_wait([this](const asio::error_code& timeout_error) {
                    if (timeout_error) return;
                    std::vector<std::shared_ptr<TcpConnection>> remaining;
                    remaining.reserve(connections_.size());
                    for (auto& [id, connection] : connections_) remaining.push_back(connection);
                    for (auto& connection : remaining) connection->stop(true);
                    io_.stop();
                });
                return;
            }
            check_stop();
        });
    }
};

} // namespace octane::transport
