#pragma once

#include <string>
#include <string_view>
#include <deque>
#include <functional>
#include <memory>
#include "WebSocket.h"
#include "WebSocketFrame.h"

namespace octane::websocket {

class WebSocketSession : public WebSocket {
public:
    using WriteCallback = std::function<void(std::string)>;
    using CloseCallback = std::function<void(CloseCode, std::string_view)>;

    explicit WebSocketSession(WebSocketConfig config,
                              WriteCallback write_cb,
                              CloseCallback close_cb)
        : config_(std::move(config)),
          write_cb_(std::move(write_cb)),
          close_cb_(std::move(close_cb)),
          is_open_(true) {}

    void on_open() {
        if (config_.on_open) {
            config_.on_open(*this);
        }
    }

    void feed(std::string_view data) {
        if (!is_open_) return;
        read_buffer_.append(data);
        process_frames();
    }

    void send_text(std::string_view text) override {
        send_frame(Opcode::Text, text);
    }

    void send_binary(std::string_view data) override {
        send_frame(Opcode::Binary, data);
    }

    void send_ping(std::string_view payload = {}) override {
        send_frame(Opcode::Ping, payload);
    }

    void send_pong(std::string_view payload = {}) override {
        send_frame(Opcode::Pong, payload);
    }

    void close(CloseCode code = CloseCode::NormalClosure, std::string_view reason = {}) override {
        if (!is_open_) return;
        is_open_ = false;
        std::string close_frame = serialize_close(code, reason);
        if (write_cb_) write_cb_(std::move(close_frame));
        if (close_cb_) close_cb_(code, reason);
        if (config_.on_close) config_.on_close(*this, static_cast<uint16_t>(code), reason);
    }

    bool is_open() const noexcept override {
        return is_open_;
    }

private:
    void send_frame(Opcode opcode, std::string_view payload) {
        if (!is_open_) return;
        std::string frame = serialize_frame(opcode, payload, true);
        if (write_cb_) write_cb_(std::move(frame));
    }

    void process_frames() {
        while (is_open_ && !read_buffer_.empty()) {
            auto opt_hdr = parse_frame_header(read_buffer_);
            if (!opt_hdr) return; // Need more bytes

            const auto& hdr = *opt_hdr;
            const size_t total_frame_size = hdr.header_size + hdr.payload_length;
            if (read_buffer_.size() < total_frame_size) return; // Need more payload bytes

            // Check payload limits
            if (hdr.payload_length > config_.max_payload_bytes) {
                close(CloseCode::MessageTooBig, "Frame payload exceeds maximum allowed size");
                return;
            }

            // Unmask payload if masked (RFC 6455 requires client-to-server frames to be masked)
            char* payload_ptr = read_buffer_.data() + hdr.header_size;
            if (hdr.masked) {
                unmask_payload(payload_ptr, hdr.payload_length, hdr.mask_key);
            }

            std::string_view payload(payload_ptr, hdr.payload_length);

            // Handle control frames
            if (is_control_opcode(hdr.opcode)) {
                handle_control_frame(hdr, payload);
            } else {
                handle_data_frame(hdr, payload);
            }

            // Consume frame from buffer
            read_buffer_.erase(0, total_frame_size);
        }
    }

    void handle_control_frame(const FrameHeader& hdr, std::string_view payload) {
        switch (hdr.opcode) {
            case Opcode::Ping:
                if (config_.on_ping) config_.on_ping(*this, payload);
                if (config_.auto_pong) send_pong(payload);
                break;
            case Opcode::Pong:
                if (config_.on_pong) config_.on_pong(*this, payload);
                break;
            case Opcode::Close: {
                uint16_t code = static_cast<uint16_t>(CloseCode::NormalClosure);
                std::string_view reason;
                if (payload.size() >= 2) {
                    code = (static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]);
                    if (payload.size() > 2) reason = payload.substr(2);
                }
                close(static_cast<CloseCode>(code), reason);
                break;
            }
            default:
                close(CloseCode::ProtocolError, "Invalid control opcode");
                break;
        }
    }

    void handle_data_frame(const FrameHeader& hdr, std::string_view payload) {
        if (hdr.opcode == Opcode::Continuation) {
            if (!fragment_in_progress_) {
                close(CloseCode::ProtocolError, "Unexpected continuation frame");
                return;
            }
            if (fragment_buffer_.size() + payload.size() > config_.max_payload_bytes) {
                close(CloseCode::MessageTooBig, "Fragmented message exceeds maximum allowed size");
                return;
            }
            fragment_buffer_.append(payload);
            if (hdr.fin) {
                fragment_in_progress_ = false;
                dispatch_message(fragment_buffer_, fragment_is_binary_);
                fragment_buffer_.clear();
            }
        } else if (hdr.opcode == Opcode::Text || hdr.opcode == Opcode::Binary) {
            const bool is_binary = (hdr.opcode == Opcode::Binary);
            if (hdr.fin) {
                dispatch_message(payload, is_binary);
            } else {
                fragment_in_progress_ = true;
                fragment_is_binary_ = is_binary;
                fragment_buffer_.assign(payload);
            }
        } else {
            close(CloseCode::ProtocolError, "Unknown data opcode");
        }
    }

    void dispatch_message(std::string_view payload, bool is_binary) {
        if (config_.on_message) {
            try {
                config_.on_message(*this, payload, is_binary);
            } catch (const std::exception& e) {
                if (config_.on_error) config_.on_error(*this, e.what());
            } catch (...) {
                if (config_.on_error) config_.on_error(*this, "Unknown handler exception");
            }
        }
    }

    WebSocketConfig config_;
    WriteCallback write_cb_;
    CloseCallback close_cb_;
    bool is_open_{false};
    std::string read_buffer_;

    bool fragment_in_progress_{false};
    bool fragment_is_binary_{false};
    std::string fragment_buffer_;
};

} // namespace octane::websocket
