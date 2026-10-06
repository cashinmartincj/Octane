#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include "WebSocketFrame.h"

namespace octane {

class WebSocket;

using WsOpenHandler    = std::function<void(WebSocket&)>;
using WsMessageHandler = std::function<void(WebSocket&, std::string_view, bool /* is_binary */)>;
using WsCloseHandler   = std::function<void(WebSocket&, uint16_t /* code */, std::string_view /* reason */)>;
using WsErrorHandler   = std::function<void(WebSocket&, std::string_view /* error */)>;
using WsPingHandler    = std::function<void(WebSocket&, std::string_view /* payload */)>;
using WsPongHandler    = std::function<void(WebSocket&, std::string_view /* payload */)>;

/**
 * @struct WebSocketConfig
 * @brief Configuration and event callbacks for a WebSocket connection.
 */
struct WebSocketConfig {
    WsOpenHandler    on_open;
    WsMessageHandler on_message;
    WsCloseHandler   on_close;
    WsErrorHandler   on_error;
    WsPingHandler    on_ping;
    WsPongHandler    on_pong;

    size_t max_payload_bytes{16 * 1024 * 1024}; // Default 16MB max message
    bool auto_pong{true};                        // Automatically respond to ping frames
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

    // Attach custom user-defined data pointer / context
    void* user_data() const noexcept { return user_data_; }
    void set_user_data(void* ptr) noexcept { user_data_ = ptr; }

protected:
    void* user_data_{nullptr};
};

} // namespace octane
