#include "HttpParser.h"
#include "RouteBase.h"
#include "core/RequestDispatcher.h"
#include <cstdlib>
#include <string_view>

void check(bool condition) { if (!condition) std::abort(); }

struct Echo : octane::routes::Post<Echo> {
    void before(octane::HttpRequest& req, octane::HttpResponse&) { req.body += "!"; }
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        res.text(body(req));
    }
    void after(octane::HttpRequest&, octane::HttpResponse& res) {
        res.header("X-Hook", "yes");
    }
};

int main() {
    auto req = octane::HttpParser::parse(
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\nContent-Type: application/json\r\n\r\nabc");
    check(req.header("content-length") == "3");
    check(req.header("CONTENT-LENGTH") == "3");
    check(req.is_json());
    const auto missing = req.param("missing");
    check(missing.empty());
    check(req.q("missing", "fallback") == "fallback");
    req.params["id"] = "123";
    check(req.param(std::string_view("id")) == "123");
    check(req.param("id").data() == req.params.at("id").data());
    octane::Router router;
    router.post("/echo", Echo::handler());
    octane::core::RequestDispatcher dispatcher(router);
    const auto response = dispatcher.dispatch(req);
    check(response.ends_with("abc!"));
    check(response.find("X-Hook: yes\r\n") != std::string::npos);
    octane::HttpResponse res;
    res.html_view("mapped").text(std::string_view("owned"));
    check(res.active_body() == "owned");
}
