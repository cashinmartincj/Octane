#pragma once
#include <asio.hpp>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <unordered_map>
#include "TcpConnection.h"

#if defined(__linux__)
#include <sys/socket.h>
#endif

namespace octane::transport {

class WorkerContext {
public:
    asio::io_context io{1};
    std::unique_ptr<tcp::acceptor> acceptor;
    std::unordered_map<std::size_t, std::shared_ptr<TcpConnection>> connections;
    std::size_t next_id = 0;
    bool stopping = false;
};

class TcpServer {
    Router& router_;
    HttpLimits limits_;
    std::vector<std::unique_ptr<WorkerContext>> workers_;
    asio::io_context signal_io_{1};
    asio::signal_set signals_{signal_io_};

public:
    explicit TcpServer(Router& router, const HttpLimits& limits = {})
        : router_(router), limits_(limits) {
        limits_.validate();
    }

    void stop() {
        for (auto& worker : workers_) {
            asio::post(worker->io, [&worker] {
                if (worker->stopping) return;
                worker->stopping = true;
                asio::error_code ec;
                if (worker->acceptor) worker->acceptor->close(ec);
                for (auto& [id, conn] : worker->connections) {
                    conn->stop(true);
                }
            });
        }
        signal_io_.stop();
    }

    void listen(int port, int threads = std::max(1u, std::thread::hardware_concurrency())) {
        if (port < 0 || port > 65535) throw std::invalid_argument("Invalid port");
        unsigned short listen_port = static_cast<unsigned short>(port);

        workers_.clear();
        workers_.reserve(threads);

        for (int i = 0; i < threads; ++i) {
            auto worker = std::make_unique<WorkerContext>();
            worker->acceptor = std::make_unique<tcp::acceptor>(worker->io);
            
            tcp::endpoint endpoint(tcp::v4(), listen_port);
            worker->acceptor->open(endpoint.protocol());
            
            asio::error_code ec;
            worker->acceptor->set_option(tcp::acceptor::reuse_address(true), ec);

#if defined(SO_REUSEPORT)
            // Enable kernel-level load balancing across threads
            int one = 1;
            ::setsockopt(worker->acceptor->native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

            worker->acceptor->bind(endpoint, ec);
            if (ec) throw std::system_error(ec, "Failed to bind port");

            worker->acceptor->listen(asio::socket_base::max_listen_connections, ec);
            if (ec) throw std::system_error(ec, "Failed to listen on port");

            if (i == 0) {
                listen_port = worker->acceptor->local_endpoint().port();
            }

            start_accept(*worker);
            workers_.push_back(std::move(worker));
        }

        signals_.add(SIGINT);
        signals_.add(SIGTERM);
        signals_.async_wait([this](const asio::error_code& ec, int) {
            if (!ec) stop();
        });

        std::cout << "\033[1;32m● \033[32mOctane listening on port " << listen_port 
                << " across " << threads << " independent event loop(s) [SO_REUSEPORT]\033[0m" << std::endl;

        std::vector<std::jthread> pool;
        pool.reserve(threads - 1);
        for (std::size_t i = 1; i < workers_.size(); ++i) {
            pool.emplace_back([this, i] { workers_[i]->io.run(); });
        }

        std::jthread sig_thread([this] { signal_io_.run(); });

        workers_[0]->io.run();
    }

private:
    void start_accept(WorkerContext& worker) {
        if (worker.stopping || !worker.acceptor->is_open()) return;

        worker.acceptor->async_accept(
            [&worker, this](const asio::error_code& ec, tcp::socket socket) {
                if (!ec) {
                    if (worker.connections.size() < worker_limit()) {
                        const auto id = worker.next_id++;
                        auto conn = std::make_shared<TcpConnection>(
                            std::move(socket), router_, limits_, id,
                            [&worker](std::size_t conn_id) {
                                worker.connections.erase(conn_id);
                            });
                        worker.connections.emplace(id, conn);
                        conn->start();
                    }
                }

                if (!worker.stopping && worker.acceptor->is_open()) {
                    start_accept(worker);
                }
            });
    }

    std::size_t worker_limit() const noexcept {
        return std::max<std::size_t>(1024, limits_.max_connections / std::max<std::size_t>(1, workers_.size()));
    }
};

} // namespace octane::transport