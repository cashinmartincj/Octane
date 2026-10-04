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
#include "../HttpLimits.h"
#include "../HttpParser.h"
#include "../core/RequestDispatcher.h"

namespace octane::transport {
using asio::ip::tcp;

/**
 * @class TcpConnection
 * @brief Manages the state and I/O lifecycle of a single Asio-based HTTP client connection.
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    tcp::socket socket_;                     ///< Asio TCP socket
    core::RequestDispatcher dispatcher_;     ///< Request-to-route dispatch pipeline
    HttpLimits limits_;                      ///< Protocol and sizing boundaries
    std::function<void(std::size_t)> on_close_; ///< Callback invoked upon connection termination
    std::size_t id_ = 0;                     ///< Monotonic connection identifier

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

    enum class Phase : uint8_t { headers, body, writing, closed } phase_ = Phase::headers;
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
                  std::size_t id, std::function<void(std::size_t)> on_close)
        : socket_(std::move(socket)),
          dispatcher_(router),
          limits_(limits),
          on_close_(std::move(on_close)),
          id_(id),
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
        process_input();
    }

    /**
     * @brief Inspects buffered bytes, parses HTTP headers/body, and dispatches to handler.
     */
    void process_input() {
        while (phase_ != Phase::closed && phase_ != Phase::writing) {
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

            HttpResponse response = dispatcher_.dispatch_response(request_, close_after_write_);

            read_pos_ += total_req_bytes;
            if (read_pos_ == write_pos_) {
                read_pos_ = 0;
                write_pos_ = 0;
            }

            const bool is_head = (request_.method == HttpMethod::HEAD);
            header_payload_ = response.serialize_headers(close_after_write_);

            if (is_head) {
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
            return;
        }
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

        std::array<asio::const_buffer, 2> write_buffers = {
            asio::buffer(header_payload_),
            asio::buffer(body_payload_.data(), body_payload_.size())
        };

        asio::async_write(
            socket_, write_buffers,
            [self = shared_from_this()](const asio::error_code& ec, std::size_t) {
                if (self->phase_ == Phase::closed) return;

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