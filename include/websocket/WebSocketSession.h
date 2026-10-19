#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <functional>
#include "WebSocket.h"
#include "WebSocketFrame.h"

namespace octane::websocket {

// All session methods and callbacks run on the connection's owning worker.
// Callback payload views expire when the callback returns.
class WebSocketSession : public WebSocket {
public:
    using WriteCallback = std::function<void(std::string)>;
    using CloseCallback = std::function<void(CloseCode, std::string_view)>;

    explicit WebSocketSession(WebSocketConfig config, WriteCallback write_cb,
                              CloseCallback close_cb, WebSocketSender sender = {})
        : config_(std::move(config)), sender_(std::move(sender)),
          write_cb_(std::move(write_cb)), close_cb_(std::move(close_cb)) {}

    void on_open() { invoke([&] { if (config_.on_open) config_.on_open(*this); }); }
    void on_open(const HttpRequest& request) {
        invoke([&] {
            if (config_.on_connect) config_.on_connect(*this, request);
            if (is_open() && config_.on_open) config_.on_open(*this);
        });
    }
    WebSocketSender sender() const override { return sender_; }

    void feed(std::string_view data) {
        // Buffer only one frame, validating its header before accepting payload.
        while (!closed_ && !data.empty()) {
            if (!header_) {
                read_buffer_.push_back(data.front());
                data.remove_prefix(1);
                try { header_ = parse_frame_header(read_buffer_); }
                catch (const std::runtime_error&) {
                    fail(CloseCode::ProtocolError, "Invalid frame header");
                    return;
                }
                if (read_buffer_.size() >= 2 && !(static_cast<uint8_t>(read_buffer_[1]) & 0x80)) {
                    fail(CloseCode::ProtocolError, "Client frames must be masked");
                    return;
                }
                if (!header_) continue;
                if (!validate_header(*header_)) return;
            }
            const size_t total = header_->header_size + static_cast<size_t>(header_->payload_length);
            const size_t count = std::min(data.size(), total - read_buffer_.size());
            read_buffer_.append(data.substr(0, count));
            data.remove_prefix(count);
            if (read_buffer_.size() < total) return;

            const auto hdr = *header_;
            std::string frame = std::move(read_buffer_);
            read_buffer_.clear();
            header_.reset();
            char* bytes = frame.data() + hdr.header_size;
            unmask_payload(bytes, static_cast<size_t>(hdr.payload_length), hdr.mask_key);
            const std::string_view payload(bytes, static_cast<size_t>(hdr.payload_length));
            if (is_control_opcode(hdr.opcode)) handle_control_frame(hdr, payload);
            else if (!closing_) handle_data_frame(hdr, payload);
        }
    }

    void send_text(std::string_view text) override {
        if (!valid_utf8(text)) throw std::invalid_argument("Outbound text must be UTF-8");
        send_frame(Opcode::Text, text);
    }
    void send_binary(std::string_view data) override { send_frame(Opcode::Binary, data); }
    void send_ping(std::string_view payload = {}) override { send_frame(Opcode::Ping, payload); }
    void send_pong(std::string_view payload = {}) override { send_frame(Opcode::Pong, payload); }

    void close(CloseCode code = CloseCode::NormalClosure, std::string_view reason = {}) override {
        if (!is_open()) return;
        // Validate before changing state. Keep receiving until peer Close or timeout.
        auto frame = serialize_close(code, reason);
        closing_ = true;
        close_code_ = code;
        close_reason_ = std::string(reason);
        if (write_cb_) write_cb_(std::move(frame));
        signal_close();
    }

    bool is_open() const noexcept override { return !closing_ && !closed_; }
    bool is_readable() const noexcept { return !closed_; }
    bool should_close_transport() const noexcept { return closed_; }
    bool has_pending_input() const noexcept { return !read_buffer_.empty() || fragment_in_progress_; }

    // Called by the transport exactly once on EOF, timeout, or shutdown. Does
    // not write to a dead socket; also releases state and notifies applications.
    void transport_closed() noexcept {
        if (!closed_) {
            closed_ = true;
            close_code_ = CloseCode::AbnormalClosure;
            close_reason_.clear();
        }
        read_buffer_.clear();
        fragment_buffer_.clear();
        notify_close();
    }

private:
    template<class Fn> void invoke(Fn&& fn) {
        try { fn(); }
        catch (...) {
            if (config_.on_error) {
                try { config_.on_error(*this, "WebSocket handler failed"); } catch (...) {}
            }
            fail(CloseCode::InternalServerError, "Handler failed");
        }
    }

    void signal_close() {
        if (close_cb_) close_cb_(close_code_, close_reason_);
    }
    void notify_close() noexcept {
        if (close_notified_) return;
        close_notified_ = true;
        if (config_.on_close) {
            try { config_.on_close(*this, static_cast<uint16_t>(close_code_), close_reason_); }
            catch (...) {}
        }
    }
    void fail(CloseCode code, std::string_view reason) {
        if (closed_) return;
        const bool send_close = !closing_;
        closing_ = closed_ = true;
        close_code_ = code;
        close_reason_ = std::string(reason);
        if (send_close && write_cb_) write_cb_(serialize_close(code, reason));
        signal_close();
        notify_close();
    }

    void send_frame(Opcode opcode, std::string_view payload) {
        if (!is_open()) return;
        if (!is_control_opcode(opcode) && payload.size() > config_.max_payload_bytes)
            throw std::length_error("Outbound message exceeds payload limit");
        auto frame = serialize_frame(opcode, payload);
        if (write_cb_) write_cb_(std::move(frame));
    }

    bool validate_header(const FrameHeader& hdr) {
        if (hdr.payload_length > std::numeric_limits<size_t>::max() - hdr.header_size ||
            (!is_control_opcode(hdr.opcode) && hdr.payload_length > config_.max_payload_bytes)) {
            fail(CloseCode::MessageTooBig, "Frame exceeds payload limit");
            return false;
        }
        if (closing_ || is_control_opcode(hdr.opcode)) return true;
        if ((hdr.opcode == Opcode::Continuation) != fragment_in_progress_) {
            fail(CloseCode::ProtocolError, "Invalid fragmentation sequence");
            return false;
        }
        if (fragment_in_progress_ && hdr.payload_length > config_.max_payload_bytes - fragment_buffer_.size()) {
            fail(CloseCode::MessageTooBig, "Message exceeds payload limit");
            return false;
        }
        return true;
    }

    void handle_control_frame(const FrameHeader& hdr, std::string_view payload) {
        switch (hdr.opcode) {
            case Opcode::Ping:
                if (!closing_) {
                    invoke([&] { if (config_.on_ping) config_.on_ping(*this, payload); });
                    if (config_.auto_pong) send_pong(payload);
                }
                break;
            case Opcode::Pong:
                if (!closing_) invoke([&] { if (config_.on_pong) config_.on_pong(*this, payload); });
                break;
            case Opcode::Close: {
                if (payload.size() == 1) { fail(CloseCode::ProtocolError, "Invalid close payload"); return; }
                uint16_t code = static_cast<uint16_t>(CloseCode::NoStatusReceived);
                std::string_view reason;
                if (payload.size() >= 2) {
                    code = (static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]);
                    reason = payload.substr(2);
                    if (!valid_close_code(code)) { fail(CloseCode::ProtocolError, "Invalid close code"); return; }
                    if (!valid_utf8(reason)) { fail(CloseCode::InvalidFramePayloadData, "Invalid close reason"); return; }
                }
                const bool reply = !closing_;
                closed_ = closing_ = true;
                close_code_ = static_cast<CloseCode>(code);
                close_reason_ = std::string(reason);
                // Echo an empty Close without putting the reserved 1005 code on the wire.
                if (reply && write_cb_) write_cb_(serialize_frame(Opcode::Close, payload));
                signal_close();
                notify_close();
                break;
            }
            default: break; // Header parser rejects unknown control opcodes.
        }
    }

    void handle_data_frame(const FrameHeader& hdr, std::string_view payload) {
        if (hdr.opcode == Opcode::Continuation) {
            fragment_buffer_.append(payload);
            if (hdr.fin) {
                fragment_in_progress_ = false;
                std::string message = std::move(fragment_buffer_);
                fragment_buffer_.clear();
                dispatch_message(message, fragment_is_binary_);
            }
        } else if (hdr.fin) {
            dispatch_message(payload, hdr.opcode == Opcode::Binary);
        } else {
            fragment_in_progress_ = true;
            fragment_is_binary_ = hdr.opcode == Opcode::Binary;
            fragment_buffer_.assign(payload);
        }
    }
    void dispatch_message(std::string_view payload, bool is_binary) {
        if (!is_binary && !valid_utf8(payload)) {
            fail(CloseCode::InvalidFramePayloadData, "Text must be UTF-8");
            return;
        }
        invoke([&] { if (config_.on_message) config_.on_message(*this, payload, is_binary); });
    }

    WebSocketConfig config_;
    WebSocketSender sender_;
    WriteCallback write_cb_;
    CloseCallback close_cb_;
    bool closing_{false}, closed_{false}, close_notified_{false};
    CloseCode close_code_{CloseCode::NormalClosure};
    std::string close_reason_;
    std::string read_buffer_;
    std::optional<FrameHeader> header_;
    bool fragment_in_progress_{false}, fragment_is_binary_{false};
    std::string fragment_buffer_;
};
} // namespace octane::websocket
