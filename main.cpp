#include "FileHandle.h"
#include <iostream>
#include <asio.hpp>

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

void handle(tcp::socket socket, std::string_view content) 
{
    char buf[4096];
    asio::error_code ec;
    std::size_t len = socket.read_some(asio::buffer(buf), ec);
    if (ec) return;

    std::string request(buf, len);

    #ifdef OCTANE_DEV_MODE
        if (request.find("/__reload") != std::string::npos) {
            struct stat st;
            stat("index.html", &st);
            std::string mtime = std::to_string(st.st_mtime);
            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: " + std::to_string(mtime.size()) + "\r\n"
                "\r\n" + mtime;
            asio::write(socket, asio::buffer(response), ec);
            return;
        }

        std::string html(content);
        auto pos = html.find("</body>");
        if (pos != std::string::npos)
            html.insert(pos, LIVE_RELOAD_SCRIPT);

        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " + std::to_string(html.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n";

        std::array<asio::const_buffer, 2> buffers = {
            asio::buffer(header),
            asio::buffer(html.data(), html.size())
        };
        asio::write(socket, buffers, ec);
    #else
        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " + std::to_string(content.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n";

        std::array<asio::const_buffer, 2> buffers = {
            asio::buffer(header),
            asio::buffer(content.data(), content.size())
        };
        asio::write(socket, buffers, ec);
    #endif
}

int main()
{
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 8080));
    std::cout << "OCTANE RUNNING ON PORT..." << std::endl;

    #ifndef OCTANE_DEV_MODE
        MappedFile html;
        if (!html.open("index.html")) 
        {
            std::cerr << "Failed to open index.html\n";
            return 1;
        }
    #endif

        while (true)
        {
            tcp::socket socket(io);
            acceptor.accept(socket);
            #ifdef OCTANE_DEV_MODE
                std::string html = read_file("index.html");
                handle(std::move(socket), html);
            #else
                handle(std::move(socket), html.view());
            #endif
        }
    return 0;
}