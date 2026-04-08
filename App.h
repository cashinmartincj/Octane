#pragma once
#include "Router.h"
#include "HttpParser.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"        // ← Handler comes from here
#include <asio.hpp>
#include <iostream>
#include <thread>
using asio::ip::tcp;

#ifdef OCTANE_DEV_MODE
const std::string LIVE_RELOAD_SCRIPT =
"<script>"
"let last='';"
"setInterval(async()=>{"
"const r=await fetch('/__reload');"
"const t=await r.text();"
"if(last&&last!==t)location.reload();"
"last=t;"
"},500);"
"</script>";
#endif

class App {
    Router           router_;
    asio::io_context io_;
    tcp::acceptor acceptor_{io_};  // ← member, lives as long as App


public:
    // ── CRTP style ───────────────────────────────
    // app.get<GetImage>("/image")
    template<typename RouteClass> App& get    (const std::string& path) { router_.get    (path, RouteClass::handler()); return *this; }
    template<typename RouteClass> App& post   (const std::string& path) { router_.post   (path, RouteClass::handler()); return *this; }
    template<typename RouteClass> App& put    (const std::string& path) { router_.put    (path, RouteClass::handler()); return *this; }
    template<typename RouteClass> App& patch  (const std::string& path) { router_.patch  (path, RouteClass::handler()); return *this; }
    template<typename RouteClass> App& del    (const std::string& path) { router_.del    (path, RouteClass::handler()); return *this; }
    template<typename RouteClass> App& options(const std::string& path) { router_.options(path, RouteClass::handler()); return *this; }

    // ── Lambda style ─────────────────────────────
    // app.get("/health", [](HttpRequest& req, HttpResponse& res) { ... })
    App& get    (const std::string& path, Handler h) { router_.get    (path, h); return *this; }
    App& post   (const std::string& path, Handler h) { router_.post   (path, h); return *this; }
    App& put    (const std::string& path, Handler h) { router_.put    (path, h); return *this; }
    App& patch  (const std::string& path, Handler h) { router_.patch  (path, h); return *this; }
    App& del    (const std::string& path, Handler h) { router_.del    (path, h); return *this; }
    App& options(const std::string& path, Handler h) { router_.options(path, h); return *this; }

    // ── Start server ─────────────────────────────
    void listen(int port, int threads = std::thread::hardware_concurrency()) {
        auto work = asio::make_work_guard(io_);
        acceptor_.open(tcp::v4());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(tcp::endpoint(tcp::v4(), port));
        acceptor_.listen();

        accept_next();  // ← no argument needed now

        std::vector<std::thread> pool;
        for (int i = 0; i < threads - 1; ++i)
            pool.emplace_back([this]{ io_.run(); });
        io_.run();
        for (auto& t : pool) t.join();
    }
    
private:
    // ── Async accept loop ────────────────────────
    void accept_next() {
        auto socket = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*socket, [this, socket](asio::error_code ec) {
            if (ec == asio::error::operation_aborted) return;
            if (!ec) handle_connection(socket);
            accept_next();   // ← no argument
        });
    }

    // ── Async read → parse → route → write ───────
    void handle_connection(std::shared_ptr<tcp::socket> socket) {
        auto buf = std::make_shared<asio::streambuf>();
        asio::async_read_until(*socket, *buf, "\r\n\r\n",
            [this, socket, buf](asio::error_code ec, std::size_t) {
                if (ec) return;

                // peek at headers WITHOUT consuming buffer
                auto data = buf->data();
                std::string headers_raw(
                    asio::buffers_begin(data),
                    asio::buffers_end(data)   // ← doesn't consume
                );

                // check Content-Length
                HttpRequest req = HttpParser::parse(headers_raw);
                int content_length = 0;
                auto cl = req.header("Content-Length");
                if (!cl.empty()) {
                    try { content_length = std::stoi(cl); } catch (...) {}
                }

                int already_read = (int)buf->size();
                int remaining    = content_length - already_read;

                if (remaining > 0) {
                    asio::async_read(*socket, *buf,
                        asio::transfer_exactly(remaining),
                        [this, socket, buf](asio::error_code ec, std::size_t) {
                            if (ec) return;
                            dispatch(socket, buf);
                        });
                } else {
                    dispatch(socket, buf);
                }
            });
    }

    void dispatch(std::shared_ptr<tcp::socket> socket,
                std::shared_ptr<asio::streambuf> buf) {

        std::string raw {
            std::istreambuf_iterator<char>(buf.get()),
            std::istreambuf_iterator<char>()
        };

    #ifdef OCTANE_DEV_MODE
        if (raw.find("/__reload") != std::string::npos) {
            serve_reload(socket);
            return;
        }
    #endif

        // 1. parse — body now included
        HttpRequest  req = HttpParser::parse(raw);

        // 2. resolve
        HttpResponse res;
        if (!router_.resolve(req, res))
            res.status(404).json(R"({"error":"not found"})");

    #ifdef OCTANE_DEV_MODE
        if (res.content_type == ContentType::TEXT_HTML) {
            auto pos = res.body.find("</body>");
            if (pos != std::string::npos)
                res.body.insert(pos, LIVE_RELOAD_SCRIPT);
            res.headers["Content-Length"] = std::to_string(res.body.size());
        }
    #endif

        // 3. async write
        auto payload = std::make_shared<std::string>(res.serialize());
        asio::async_write(*socket, asio::buffer(*payload),
            [this, socket, payload](asio::error_code ec, std::size_t) {
                if (ec) return;
                handle_connection(socket);
            });
    }

#ifdef OCTANE_DEV_MODE
    void serve_reload(std::shared_ptr<tcp::socket> socket) {
        struct stat st;
        stat("index.html", &st);
        std::string mtime    = std::to_string(st.st_mtime);
        auto        response = std::make_shared<std::string>(
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: " + std::to_string(mtime.size()) + "\r\n"
            "\r\n" + mtime
        );
        asio::async_write(*socket, asio::buffer(*response),
            [socket, response](asio::error_code, std::size_t){});
    }
#endif
};