#pragma once

#include "IoUringContext.h"
#include "Router.h"
#include "HttpLimits.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <thread>
#include <vector>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace octane::transport {

class TcpServer {
public:
    TcpServer(std::shared_ptr<octane::Router> router, const HttpLimits& limits)
        : router_(router), limits_(limits) {}

    TcpServer(octane::Router& router, const HttpLimits& limits)
        : router_(std::shared_ptr<octane::Router>(&router, [](octane::Router*){})), limits_(limits) {}

    void listen(int port, size_t num_threads) {
        port_ = port;
        num_threads_ = num_threads;

        std::cout << "\033[1;32m● \033[32mOctane (io_uring) starting on port " << port_ 
                  << " across " << num_threads_ << " worker thread(s) [SO_REUSEPORT]\033[0m" << std::endl;

        for (size_t i = 0; i < num_threads_; ++i) {
            workers_.emplace_back([this]() {
                run_worker();
            });
        }

        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

private:
    int port_{0};
    size_t num_threads_{1};
    std::shared_ptr<octane::Router> router_;
    HttpLimits limits_;
    std::vector<std::thread> workers_;

    void run_worker() {
        // Each worker thread creates its own listening socket with SO_REUSEPORT
        int thread_server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (thread_server_fd < 0) return;

        int opt = 1;
        setsockopt(thread_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(thread_server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(thread_server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            close(thread_server_fd);
            return;
        }

        if (::listen(thread_server_fd, SOMAXCONN) < 0) {
            close(thread_server_fd);
            return;
        }

        // Initialize ring bound exclusively to this worker's socket
        IoUringContext ring_ctx(thread_server_fd, router_, 128);
        ring_ctx.run();

        close(thread_server_fd);
    }
};

} // namespace octane::transport