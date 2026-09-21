#pragma once

#include <liburing.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <memory>
#include "Router.h"
#include "HttpParser.h"
#include "HttpResponse.h"

namespace octane::transport {

enum class OperationType {
    Accept,
    Read,
    Write
};

struct AsyncContext {
    OperationType op_type;
    int client_fd{-1};
    char read_buffer[4096];
    std::vector<char> write_buffer;
};

class IoUringContext {
public:
    explicit IoUringContext(int server_fd, std::shared_ptr<octane::Router> router = nullptr, unsigned int queue_depth = 128) 
        : server_fd_(server_fd), router_(router) {
        int ret = io_uring_queue_init(queue_depth, &ring_, 0);
        if (ret < 0) {
            throw std::runtime_error("Failed to initialize io_uring: " + std::string(strerror(-ret)));
        }
    }

    ~IoUringContext() {
        io_uring_queue_exit(&ring_);
    }

    IoUringContext(const IoUringContext&) = delete;
    IoUringContext& operator=(const IoUringContext&) = delete;

    void run() {
        queue_accept();

        io_uring_cqe* cqe;
        while (running_) {
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) {
                if (-ret == EINTR) continue;
                break; 
            }

            auto* ctx = static_cast<AsyncContext*>(io_uring_cqe_get_data(cqe));
            int res = cqe->res;

            if (ctx) {
                dispatch_event(ctx, res);
                
                // CRITICAL: Delete the context HERE and ONLY here for all operations 
                // to prevent double free or memory leaks.
                delete ctx;
            }

            io_uring_cqe_seen(&ring_, cqe);
        }
    }

    void stop() {
        running_ = false;
    }

    void queue_write(int client_fd, const std::string& data) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return;

        // Allocate persistent write context
        auto* ctx = new AsyncContext{OperationType::Write, client_fd, {}, std::vector<char>(data.begin(), data.end())};

        io_uring_prep_write(sqe, client_fd, ctx->write_buffer.data(), ctx->write_buffer.size(), 0);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(&ring_);
    }

private:
    io_uring ring_;
    int server_fd_;
    std::shared_ptr<octane::Router> router_;
    bool running_{true};

    void queue_accept() {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return;

        auto* ctx = new AsyncContext{OperationType::Accept, -1, {}, {}};
        
        io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, SOCK_NONBLOCK);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(&ring_);
    }

    void queue_read(int client_fd) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) return;

        auto* ctx = new AsyncContext{OperationType::Read, client_fd, {}, {}};
        
        io_uring_prep_recv(sqe, client_fd, ctx->read_buffer, sizeof(ctx->read_buffer), 0);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(&ring_);
    }

    void dispatch_event(AsyncContext* ctx, int res) {
        switch (ctx->op_type) {
            case OperationType::Accept: {
                // 1. Immediately re-queue accept for subsequent connections
                queue_accept();
                
                // 2. If accept succeeded, start reading from the new client socket
                if (res >= 0) {
                    queue_read(res);
                }
                break;
            }
            case OperationType::Read: {
                if (res > 0) {
                    HttpRequest request = HttpParser::parse(std::string_view(ctx->read_buffer, res));
                    HttpResponse response;

                    // 1. Resolve through the router
                    if (router_) {
                        if (!router_->resolve(request, response)) {
                            response.status(404).text("Not Found");
                        }
                    } else {
                        response.status(404).text("Not Found");
                    }

                    // 2. Use HttpResponse's built-in serialization (supports MappedFile views, correct Content-Type, Content-Length, etc.)
                    std::string raw_response = response.serialize(false); // false = keep-alive
                    
                    queue_write(ctx->client_fd, raw_response);
                } else {
                    // Client disconnected or read error
                    close(ctx->client_fd);
                }
                break;
            }
            case OperationType::Write: {
                if (res < 0) {
                    close(ctx->client_fd);
                } else {
                    // Keep-alive or close logic after successful write
                    bool keep_alive = true; 
                    if (keep_alive) {
                        queue_read(ctx->client_fd);
                    } else {
                        close(ctx->client_fd);
                    }
                }
                break;
            }
        }
    }
};

} // namespace octane::transport