#include "transport/TcpServer.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <mutex>

int main() {
    std::mutex pushes_mutex;
    std::vector<std::jthread> pushes;
    octane::Router router;
    static std::atomic_size_t opened{0}, closed{0};
    router.get("/health", [](auto&, auto& res) { res.text("ok"); });
    router.get("/stats", [](auto&, auto& res) {
        res.text(std::to_string(opened.load()) + ":" + std::to_string(closed.load()));
    });
    octane::WebSocketConfig config;
    config.max_payload_bytes = 1024;
    config.max_pending_write_bytes = 2048;
    config.max_pending_write_frames = 4;
    config.read_timeout = std::chrono::milliseconds(300);
    config.close_timeout = std::chrono::milliseconds(150);
    config.allowed_origins = {"https://app.example"};
    config.on_open = [&](auto&) { ++opened; };
    config.on_close = [&](auto&, uint16_t, auto) { ++closed; };
    config.on_message = [](auto& ws, std::string_view msg, bool binary) {
        if (msg == "throw") throw std::runtime_error("private error");
        if (msg == "close") { ws.close(octane::websocket::CloseCode::NormalClosure, "bye"); return; }
        if (msg == "flood") {
            for (int i = 0; i < 10; ++i) ws.send_text(std::string(600, 'x'));
            return;
        }
        if (binary) ws.send_binary(msg);
        else ws.send_text(msg);
    };
    router.ws("/ws", config);
    auto greeting = config;
    greeting.on_open = [&](auto& ws) { ++opened; ws.send_text("hello"); };
    router.ws("/ws/open", greeting);
    greeting.on_open = [&](auto& ws) { ++opened; ws.close(); };
    router.ws("/ws/close-on-open", greeting);
    greeting.on_open = [&](auto&) { ++opened; throw 42; };
    router.ws("/ws/throw-on-open", greeting);
    auto control = config;
    control.on_ping = [](auto&, auto) { throw 42; };
    control.on_pong = [](auto&, auto) { throw 42; };
    control.on_error = [](auto&, auto) { throw 42; };
    router.ws("/ws/throw-control", control);
    auto slow = config;
    slow.max_payload_bytes = 4 * 1024 * 1024;
    slow.max_pending_write_bytes = slow.max_payload_bytes + 14;
    slow.read_timeout = std::chrono::seconds(2);
    slow.on_open = [&](auto& ws) { ++opened; ws.send_binary(std::string(4 * 1024 * 1024, 'x')); };
    router.ws("/ws/slow-reader", slow);
    auto async = config;
    async.on_connect = [&](auto& ws, const auto& req) {
        if (req.header("X-Connect") != "context") { ws.close(); return; }
        auto sender=ws.sender();
        std::lock_guard lock(pushes_mutex);
        pushes.emplace_back([sender] {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            sender.text("server push");
        });
    };
    router.ws("/ws/async", async);
    config.require_origin = true;
    config.authorize = [](const auto& req) { return req.header("Authorization") == "Bearer test"; };
    router.ws("/ws/private", config);
    config.authorize = [](const auto&) -> bool { throw 42; };
    router.ws("/ws/throw-auth", config);
    octane::HttpLimits limits;
    limits.max_connections = 16;
    limits.write_timeout = std::chrono::milliseconds(300);
    limits.shutdown_timeout = std::chrono::milliseconds(200);
    octane::transport::TcpServerOptions options;
    options.bind_address = "127.0.0.1";
    options.pin_workers = false;
    try {
        octane::transport::TcpServer server(router, limits, options);
        server.listen(0, 2);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
