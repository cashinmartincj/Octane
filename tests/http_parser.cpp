#include "HttpParser.h"
#include "core/RequestDispatcher.h"
#include <cstdlib>
#include <iostream>

void check(bool ok) { if (!ok) std::abort(); }
void rejects(std::string_view raw, int status = 400, const octane::HttpLimits& limits = {}) {
    try { octane::HttpParser::parse_headers(raw, limits); }
    catch (const octane::HttpParseError& error) { check(error.status == status); return; }
    std::cerr << "Unexpectedly accepted: " << raw << '\n'; std::abort();
}
int main() {
    rejects("GET / HTTP/1.1\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: a b\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost : x\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\ncontent-length: 2\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    rejects("GET / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n", 417);
    for (auto length : {"-1", "+1", "1, 1", "1x", "", "999999999999999999999999999"})
        rejects(std::string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: ") + length + "\r\n\r\n");
    rejects("GET\r\n\r\n");
    rejects("GET / HTTP/9.9\r\nHost: x\r\n\r\n", 505);
    rejects("GET /?q=%xx HTTP/1.1\r\nHost: x\r\n\r\n");
    rejects("GET /% HTTP/1.1\r\nHost: x\r\n\r\n");
    rejects("GET / HTTP/1.1\r\nHost: x\nBad: y\r\n\r\n");
    octane::HttpLimits limits;
    limits.max_body_bytes = 3;
    rejects("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n", 413, limits);
    limits.max_header_bytes = 8;
    rejects("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 431, limits);
    auto req = octane::HttpParser::parse_headers("GET / HTTP/1.0\r\n\r\n");
    check(!req.keep_alive);
    req = octane::HttpParser::parse_headers("GET / HTTP/1.1\r\nHost: x\r\nConnection: KEEP-ALIVE, Close\r\n\r\n");
    check(!req.keep_alive);
    req = octane::HttpParser::parse_headers("GET /?q=a%20b HTTP/1.1\r\nHost: x\r\n\r\n");
    check(req.q("q") == "a b");
    octane::Router router;
    router.get("/", [](auto&, auto&) { throw std::runtime_error("private detail"); });
    octane::core::RequestDispatcher dispatcher(router);
    bool close = false;
    auto response = dispatcher.dispatch(req, close);
    check(close && response.starts_with("HTTP/1.1 500") && response.find("private detail") == std::string::npos);
    octane::HttpResponse res;
    res.text("abc");
    check(res.serialize(false, true).ends_with("\r\n\r\n"));
    check(res.status(204).serialize().find("Content-Length:") == std::string::npos);
    res.header("X-Test", "value\r\nInjected: yes");
    try { res.serialize(); std::abort(); } catch (const std::invalid_argument&) {}
}
