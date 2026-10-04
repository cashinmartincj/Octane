/**
 * @file main.cpp
 * @brief 01_hello_world — Static file serving and JSON API example for Octane.
 *
 * @details
 * Demonstrates:
 * - CRTP route handlers (`octane::routes::Get<T>`)
 * - Virtual memory zero-copy static asset serving using `octane::utils::MappedFile`
 * - Static HTML, CSS, JavaScript, and JSON endpoint registration
 *
 * Routes registered:
 * - GET `/`          -> serves `index.html` via `res.html_view(...)` (zero copy)
 * - GET `/style.css` -> serves `style.css`
 * - GET `/app.js`    -> serves `app.js`
 * - GET `/api/hello` -> returns JSON payload
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#include "Octane.h"
#include "RouteBase.h"
#include "FileHandle.h"
#include <filesystem>
#include <iostream>

/**
 * @brief Resolves the directory path containing the current executing binary.
 * @return Absolute filesystem path to the executable's directory.
 */
static std::string exe_dir() {
#ifdef _WIN32
    char result[1024];
    GetModuleFileNameA(nullptr, result, sizeof(result));
    return std::filesystem::path(result).parent_path().string();

#elif __APPLE__
    char result[1024];
    uint32_t size = sizeof(result);
    _NSGetExecutablePath(result, &size);
    return std::filesystem::path(result).parent_path().string();

#else
    char result[1024];
    ssize_t count = readlink("/proc/self/exe", result, sizeof(result));
    return std::filesystem::path(std::string(result, count))
               .parent_path().string();
#endif
}

/**
 * @class GetHtml
 * @brief Serves index.html via memory-mapped zero-copy view.
 */
class GetHtml : public octane::routes::Get<GetHtml> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        static auto f = []() {
            auto p = std::make_unique<octane::utils::MappedFile>();
            if (!p->open(exe_dir() + "/index.html"))
                std::cerr << "ERROR: failed to open index.html\n";
            else
                std::cerr << "OK: index.html mapped, size=" << p->view().size() << "\n";
            return p;
        }();

        if (!f->view().data() || f->view().empty()) {
            res.status(500).text("index.html not found");
            return;
        }

        res.status(200).html_view(f->view());
    }
};

/**
 * @class GetCss
 * @brief Serves style.css stylesheet.
 */
class GetCss : public octane::routes::Get<GetCss> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<octane::utils::MappedFile>(); p->open(exe_dir() + "/style.css"); return p; }();
        res.status(200).header("Content-Type", "text/css").send(std::string(f->view()));
    }
};

/**
 * @class GetJs
 * @brief Serves client application JavaScript file.
 */
class GetJs : public octane::routes::Get<GetJs> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<octane::utils::MappedFile>(); p->open(exe_dir() + "/app.js"); return p; }();
        res.status(200).header("Content-Type", "text/javascript").send(std::string(f->view()));
    }
};

/**
 * @class GetHello
 * @brief Simple JSON greeting endpoint demonstrating microsecond latency.
 */
class GetHello : public octane::routes::Get<GetHello> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        res.status(200).json(R"({"message":"hello from octane","framework":"c++","speed":"fast"})");
    }
};

/**
 * @brief Server main entry point for the 01_hello_world example.
 */
int main() {
    octane::init app;

    app.get<GetHtml> ("/");
    app.get<GetCss>  ("/style.css");
    app.get<GetJs>   ("/app.js");
    app.get<GetHello>("/api/hello");

    app.listen(8080);
}