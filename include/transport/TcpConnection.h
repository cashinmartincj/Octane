/**
 * @file TcpConnection.h
 * @brief Asio-backed asynchronous TCP connection handler with rolling ring-buffer parsing.
 *
 * @details
 * `TcpConnection` provides an alternative / fallback TCP connection handler utilizing
 * standalone Boost.Asio asynchronous sockets. It demonstrates the same zero-overhead protocol
 * semantics as the primary `IoUringContext` engine:
 *
 * Architectural Features:
 * - **Rolling Ring Buffer:** Maintains a 64KB connection buffer (`buffer_`) with in-place memmove
 *   compaction, parsing request headers and streaming bodies with minimal heap churn.
 * - **HTTP Pipelining:** If additional request bytes already reside in the buffer after dispatching
 *   a response, `process_input()` immediately cycles to fulfill the next request.
 * - **Scatter-Gather Vectored Writes:** Emits HTTP response headers and bodies using `asio::async_write`
 *   with a 2-element `asio::const_buffer` array, avoiding extra intermediate buffer copies.
 * - **Keep-Alive Lifecycles:** Tracks `limits_.max_requests_per_connection` and connection headers
 *   to gracefully terminate or reuse sockets.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "transport/TcpConnection.h"`
 * - Testing & Fallbacks: Used in Asio-based connection test cases and non-Linux deployment environments.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once
#include <asio.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include "../HttpLimits.h"
#include "../HttpParser.h"
#include "../core/RequestDispatcher.h"
#include "ExecutionQueue.h"

namespace octane::transport {
using asio::ip::tcp;

class EpollExecutionQueues {
public:
    explicit EpollExecutionQueues(const ExecutionQueueOptions& options)
        : options_(options) {}

    bool submit(const HandlerExecution& execution, std::function<void()> job) {
        std::string key;
        std::size_t threads = 0;
        std::size_t capacity = 0;
        if (execution.mode == HandlerExecutionMode::SharedBlocking) {
            key = "shared";
            threads = options_.shared_threads_per_shard;
            capacity = options_.shared_capacity;
        } else if (execution.mode == HandlerExecutionMode::Named) {
            key = "named:" + execution.queue_name;
            for (const auto& configured : options_.named) {
                if (configured.name == execution.queue_name) {
                    threads = configured.threads_per_shard;
                    capacity = configured.capacity;
                    break;
                }
            }
        }
        if (!threads || !capacity) return false;
        auto& queue = queues_[key];
        if (!queue) queue = std::make_unique<detail::BoundedExecutionQueue>(threads, capacity);
        return queue->submit(std::move(job));
    }

private:
    ExecutionQueueOptions options_;
    std::unordered_map<std::string, std::unique_ptr<detail::BoundedExecutionQueue>> queues_;
};

/**
 * @class TcpConnection
 * @brief Manages the state and I/O lifecycle of a single Asio-based HTTP client connection.
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    tcp::socket socket_;                     ///< Asio TCP socket
    asio::steady_timer deadline_timer_;
    core::RequestDispatcher dispatcher_;     ///< Request-to-route dispatch pipeline
    Router& router_;
    HttpLimits limits_;                      ///< Protocol and sizing boundaries
    std::function<void(std::size_t)> on_close_; ///< Callback invoked upon connection termination
    std::size_t id_ = 0;                     ///< Monotonic connection identifier
    std::shared_ptr<EpollExecutionQueues> execution_queues_;

    // Rolling ring buffer
    static constexpr std::size_t BUFFER_SIZE = 65536; ///< 64KB initial socket read buffer
    std::vector<char> buffer_;              ///< Contiguous socket buffer
    std::size_t read_pos_ = 0;              ///< Read offset of unconsumed bytes
    std::size_t write_pos_ = 0;             ///< Offset of valid received bytes

    std::string header_payload_;            ///< Serialized response headers
    std::string_view body_payload_;         ///< Active response body view
    std::string owned_body_storage_;        ///< Storage for moved dynamic body responses

    HttpRequest request_;                   ///< Inbound parsed request
    std::size_t header_size_ = 0;           ///< Header bytes size including CRLFCRLF
    std::size_t requests_ = 0;              ///< Requests processed on this connection

    enum class Phase : uint8_t { headers, body, handling, writing, closed } phase_ = Phase::headers;
    bool draining_ = false;                 ///< Server shutdown draining flag
    bool close_after_write_ = false;        ///< Connection close flag

public:
    /**
     * @brief Constructs a TcpConnection wrapping an accepted Asio TCP socket.
     * @param socket Connected client TCP socket.
     * @param router Application Router instance.
     * @param limits Protocol boundaries and timeouts.
     * @param id Monotonic connection identifier.
     * @param on_close Callback invoked upon socket closure.
     */
    TcpConnection(tcp::socket socket, Router& router, const HttpLimits& limits,
                  std::size_t id, std::function<void(std::size_t)> on_close,
                  std::shared_ptr<EpollExecutionQueues> execution_queues = nullptr)
        : socket_(std::move(socket)),
          deadline_timer_(socket_.get_executor()),
          dispatcher_(router),
          router_(router),
          limits_(limits),
          on_close_(std::move(on_close)),
          id_(id),
          execution_queues_(std::move(execution_queues)),
          buffer_(BUFFER_SIZE) {
        
        asio::error_code ec;
        socket_.set_option(tcp::no_delay(true), ec);
        socket_.set_option(asio::socket_base::send_buffer_size(65536), ec);
        socket_.set_option(asio::socket_base::receive_buffer_size(65536), ec);
    }

    /**
     * @brief Starts the asynchronous connection read loop.
     */
    void start() {
        begin_request();
    }

    /**
     * @brief Requests graceful or immediate shutdown of the connection.
     * @param force When true, terminates immediately without draining active writes.
     */
    void stop(bool force = false) {
        draining_ = true;
        if (force || phase_ == Phase::headers) close();
    }

private:
    /**
     * @brief Closes the underlying socket and notifies the parent server.
     */
    void close() {
        if (phase_ == Phase::closed) return;
        phase_ = Phase::closed;
        
        asio::error_code ec;
        deadline_timer_.cancel(ec);
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        
        if (on_close_) {
            auto cb = std::move(on_close_);
            cb(id_);
        }
    }

    /**
     * @brief Initializes state for a new incoming HTTP request on a keep-alive connection.
     */
    void begin_request() {
        if (phase_ == Phase::closed) return;
        if (draining_) { close(); return; }

        phase_ = Phase::headers;
        header_size_ = 0;
        request_ = {};
        arm_deadline(limits_.read_timeout);
        process_input();
    }

    void arm_deadline(std::chrono::milliseconds timeout) {
        deadline_timer_.expires_after(timeout);
        auto self = shared_from_this();
        deadline_timer_.async_wait([self](const asio::error_code& error) {
            if (!error) self->close();
        });
    }

    /**
     * @brief Inspects buffered bytes, parses HTTP headers/body, and dispatches to handler.
     */
    void process_input() {
        while (phase_ != Phase::closed && phase_ != Phase::writing && phase_ != Phase::handling) {
            std::string_view unconsumed(buffer_.data() + read_pos_, write_pos_ - read_pos_);

            if (phase_ == Phase::headers) {
                const auto end = unconsumed.find("\r\n\r\n");
                if (end == std::string_view::npos) {
                    if (unconsumed.size() >= limits_.max_header_bytes) { fail(431); return; }
                    read_more();
                    return;
                }

                header_size_ = end + 4;
                try {
                    request_ = HttpParser::parse_headers(unconsumed.substr(0, header_size_), limits_);
                } catch (const HttpParseError& e) {
                    fail(e.status);
                    return;
                } catch (...) {
                    fail(400);
                    return;
                }
                phase_ = Phase::body;
            }

            const auto total_req_bytes = header_size_ + request_.content_length;
            if (unconsumed.size() < total_req_bytes) {
                read_more();
                return;
            }

            // Assign view directly into existing contiguous buffer memory
            request_.body = std::string(unconsumed.substr(header_size_, request_.content_length));

            close_after_write_ = draining_ || !request_.keep_alive ||
                                 (++requests_ >= limits_.max_requests_per_connection);

            read_pos_ += total_req_bytes;
            if (read_pos_ == write_pos_) {
                read_pos_ = 0;
                write_pos_ = 0;
            }

            const bool is_head = (request_.method == HttpMethod::HEAD);
            asio::error_code ignored;
            deadline_timer_.cancel(ignored);
            const HandlerRoute* route = nullptr;
            if (router_.match(request_, route) &&
                route->execution.mode != HandlerExecutionMode::Inline) {
                std::string raw_request(unconsumed.substr(0, total_req_bytes));
                phase_ = Phase::handling;
                auto self = shared_from_this();
                const bool submitted = execution_queues_ && execution_queues_->submit(
                    route->execution,
                    [self, raw = std::move(raw_request), is_head]() mutable {
                        HttpResponse response;
                        bool failed = false;
                        try {
                            const auto header_end = raw.find("\r\n\r\n");
                            const auto header_size = header_end + 4;
                            auto request = HttpParser::parse_headers(
                                std::string_view(raw).substr(0, header_size), self->limits_);
                            HttpParser::set_body(request, std::string_view(raw).substr(
                                header_size, request.content_length));
                            const HandlerRoute* matched = nullptr;
                            if (!self->router_.match(request, matched) || !matched)
                                response.status(404).text("Not Found");
                            else
                                matched->handler(request, response);
                        } catch (...) {
                            response.status(500).text("Internal Server Error");
                            failed = true;
                        }
                        asio::post(self->socket_.get_executor(),
                            [self, response = std::move(response), is_head, failed]() mutable {
                                if (self->phase_ == Phase::closed) return;
                                self->close_after_write_ = self->close_after_write_ || failed;
                                self->prepare_response(std::move(response), is_head);
                            });
                    });
                if (!submitted) {
                    phase_ = Phase::body;
                    fail(503);
                }
                return;
            }

            HttpResponse response = dispatcher_.dispatch_response(request_, close_after_write_);
            prepare_response(std::move(response), is_head);
            return;
        }
    }

    void prepare_response(HttpResponse response, bool is_head) {
            for (const auto& [name, value] : response.headers) {
                if (CaseInsensitiveEqual{}(name, "connection") &&
                    HttpParser::contains_token(value, "close")) {
                    close_after_write_ = true;
                    break;
                }
            }
            header_payload_ = response.serialize_headers(close_after_write_);

            if (is_head || response.status_code == 204 || response.status_code == 304) {
                body_payload_ = {};
                owned_body_storage_.clear();
            } else if (response.body_type == HttpResponse::BodyType::Mapped) {
                body_payload_ = response.mapped_body;
                owned_body_storage_.clear();
            } else if (response.body_type == HttpResponse::BodyType::Owned) {
                owned_body_storage_ = std::move(response.owned_body);
                body_payload_ = owned_body_storage_;
            } else {
                owned_body_storage_ = std::move(response.body);
                body_payload_ = owned_body_storage_;
            }

            write();
    }

    /**
     * @brief Compacts buffer and triggers async_read_some from the client socket.
     */
    void read_more() {
        if (read_pos_ > 0 && (buffer_.size() - write_pos_ < 4096)) {
            std::size_t remaining = write_pos_ - read_pos_;
            if (remaining > 0) {
                std::memmove(buffer_.data(), buffer_.data() + read_pos_, remaining);
            }
            read_pos_ = 0;
            write_pos_ = remaining;
        }

        if (buffer_.size() == write_pos_) {
            buffer_.resize(buffer_.size() * 2);
        }

        socket_.async_read_some(
            asio::buffer(buffer_.data() + write_pos_, buffer_.size() - write_pos_),
            [self = shared_from_this()](const asio::error_code& ec, std::size_t bytes) {
                if (self->phase_ == Phase::closed) return;
                if (ec) {
                    self->close();
                    return;
                }
                self->write_pos_ += bytes;
                self->process_input();
            });
    }

    /**
     * @brief Transmits an immediate HTTP error response and flags socket for termination.
     * @param status HTTP error status code (e.g. 400, 431).
     */
    void fail(int status) {
        if (phase_ == Phase::closed) return;
        try {
            HttpResponse response;
            response.status(status).text("Request failed");
            header_payload_ = response.serialize_headers(true);
            owned_body_storage_ = std::move(response.body);
            body_payload_ = owned_body_storage_;
            close_after_write_ = true;
            write();
        } catch (...) {
            close();
        }
    }

    /**
     * @brief Transmits response headers and body fragments asynchronously over the socket.
     */
    void write() {
        phase_ = Phase::writing;
        arm_deadline(limits_.write_timeout);

        std::array<asio::const_buffer, 2> write_buffers = {
            asio::buffer(header_payload_),
            asio::buffer(body_payload_.data(), body_payload_.size())
        };

        asio::async_write(
            socket_, write_buffers,
            [self = shared_from_this()](const asio::error_code& ec, std::size_t) {
                if (self->phase_ == Phase::closed) return;

                asio::error_code ignored;
                self->deadline_timer_.cancel(ignored);

                self->header_payload_.clear();
                self->owned_body_storage_.clear();
                self->body_payload_ = {};

                if (ec || self->close_after_write_ || self->draining_) {
                    self->close();
                    return;
                }

                self->phase_ = Phase::headers;
                self->header_size_ = 0;
                self->request_ = {};

                // Pipeline check: process immediately if extra requests already sit in buffer
                if (self->write_pos_ > self->read_pos_) {
                    self->process_input();
                } else {
                    self->read_more();
                }
            });
    }
};

} // namespace octane::transport
