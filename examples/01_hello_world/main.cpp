// examples/static_files.cpp

#include "App.h"
#include "RouteBase.h"
#include "FileHandle.h"
#include <filesystem>

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

class GetHtml : public routes::Get<GetHtml> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto f = []() {
            auto p = std::make_unique<MappedFile>();
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

class GetCss : public routes::Get<GetCss> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<MappedFile>(); p->open(exe_dir() + "/style.css"); return p; }();
        res.status(200).header("Content-Type", "text/css").send(std::string(f->view()));
    }
};

class GetJs : public routes::Get<GetJs> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<MappedFile>(); p->open(exe_dir() + "/app.js"); return p; }();
        res.status(200).header("Content-Type", "text/javascript").send(std::string(f->view()));
    }
};

class GetHello : public routes::Get<GetHello> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        res.status(200).json(R"({"message":"hello from octane","framework":"c++","speed":"fast"})");
    }
};

int main() {
    App app;

    app.get<GetHtml> ("/");
    app.get<GetCss>  ("/style.css");
    app.get<GetJs>   ("/app.js");
    app.get<GetHello>("/api/hello");

    app.listen(8080);
}