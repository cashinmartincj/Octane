// examples/static_files.cpp

#include "App.h"
#include "RouteBase.h"
#include "FileHandle.h"

class GetHtml : public routes::Get<GetHtml> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<MappedFile>(); p->open("index.html"); return p; }();
        res.status(200).html_view(f->view());
    }
};

class GetCss : public routes::Get<GetCss> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<MappedFile>(); p->open("style.css"); return p; }();
        res.status(200).header("Content-Type", "text/css").send(std::string(f->view()));
    }
};

class GetJs : public routes::Get<GetJs> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto f = []{ auto p = std::make_unique<MappedFile>(); p->open("app.js"); return p; }();
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