#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <chrono>
#include <vector>
#include "../HttpRequest.h"
#include "WebSocketFrame.h"

namespace octane {

class WebSocket;

using WsOpenHandler    = std::function<void(WebSocket&)>;
using WsMessageHandler = std::function<void(WebSocket&, std::string_view, bool /* is_binary */)>;
using WsCloseHandler   = std::function<void(WebSocket&, uint16_t /* code */, std::string_view /* reason */)>;
using WsErrorHandler   = std::function<void(WebSocket&, std::string_view /* error */)>;
using WsPingHandler    = std::function<void(WebSocket&, std::string_view /* payload */)>;
using WsPongHandler    = std::function<void(WebSocket&, std::string_view /* payload */)>;
struct WebSocketSender {
    // May be called from another thread; transports marshal onto their owner.
    std::function<bool(std::string)> text;
    std::function<void()> close;
};

/**
 * @struct WebSocketConfig
 * @brief Configuration and event callbacks for a WebSocket connection.
 */
struct WebSocketConfig {
    std::function<void(WebSocket&, const HttpRequest&)> on_connect;
    WsOpenHandler    on_open;
    WsMessageHandler on_message;
    WsCloseHandler   on_close;
    WsErrorHandler   on_error;
    WsPingHandler    on_ping;
    WsPongHandler    on_pong;

    size_t max_payload_bytes{16 * 1024 * 1024}; // Default 16MB max message
    bool auto_pong{true};                        // Automatically respond to ping frames
    size_t max_pending_write_bytes{16 * 1024 * 1024 + 14};
    size_t max_pending_write_frames{256};
    std::chrono::milliseconds read_timeout{60000}; // Absolute idle/incomplete-message deadline
    std::chrono::milliseconds close_timeout{5000};
    // Browser origins must be explicitly allowed; missing Origin is permitted
    // for non-browser clients unless require_origin is set.
    std::vector<std::string> allowed_origins;
    bool require_origin{false};
    std::string subprotocol;
    std::function<bool(const HttpRequest&)> authorize;
};

/**
 * @class WebSocket
 * @brief Represents an active full-duplex WebSocket connection.
 */
class WebSocket {
public:
    virtual ~WebSocket() = default;

    virtual void send_text(std::string_view text) = 0;
    virtual void send_binary(std::string_view data) = 0;
    virtual void send_ping(std::string_view payload = {}) = 0;
    virtual void send_pong(std::string_view payload = {}) = 0;
    virtual void close(websocket::CloseCode code = websocket::CloseCode::NormalClosure, std::string_view reason = {}) = 0;

    virtual bool is_open() const noexcept = 0;
    virtual WebSocketSender sender() const { return {}; }

    // Attach custom user-defined data pointer / context
    void* user_data() const noexcept { return user_data_; }
    void set_user_data(void* ptr) noexcept { user_data_ = ptr; }

protected:
    void* user_data_{nullptr};
};

} // namespace octane
