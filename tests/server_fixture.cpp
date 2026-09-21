#include "transport/TcpServer.h"
#include <string>
int main(int argc, char** argv) {
    octane::Router router;
    router.get("/", [](auto&, auto& res) { res.text("ok"); });
    router.post("/echo", [](auto& req, auto& res) { res.text(req.body); });
    router.get("/throw", [](auto&, auto&) { throw std::runtime_error("secret"); });
    router.get("/large", [](auto&, auto& res) { res.text(std::string(32 * 1024 * 1024, 'x')); });
    octane::HttpLimits limits;
    if (argc == 1) {
        limits.max_header_bytes = 256;
        limits.max_body_bytes = 64;
        limits.max_connections = 2;
        limits.max_requests_per_connection = 3;
        limits.read_timeout = std::chrono::milliseconds(300);
        limits.write_timeout = std::chrono::milliseconds(300);
        limits.shutdown_timeout = std::chrono::milliseconds(600);
    }
    if (argc > 2) {
        limits.read_timeout = std::chrono::seconds(3);
        limits.shutdown_timeout = std::chrono::milliseconds(150);
    }
    router.get("/close", [](auto&, auto& res) { res.header("Connection", "close").text("bye"); });
    router.head("/", [](auto&, auto& res) { res.text("suppressed"); });
    octane::transport::TcpServer server(router, limits);
    server.listen(argc > 1 ? std::stoi(argv[1]) : 0, 4);
}
