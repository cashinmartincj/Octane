// main.cpp
#include "App.h"
#include "RouteBase.h"
#include <fstream>
#include <string>
#include <unordered_map>
#include "FileHandle.h"

class GetImage : public routes::Get<GetImage> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        std::ifstream file("image.png", std::ios::binary);
        if (!file.is_open())
        {
            res.status(404).text("image not found");
            return ;
        }
        std::string data((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());

        res.status(200).image(data, ContentType::IMAGE_PNG);
    }
};

class GetHtml : public routes::Get<GetHtml> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static auto file = [](){
            auto f = std::make_unique<MappedFile>();
            f->open("index.html");
            return f;   // ← unique_ptr is moveable, not copyable — works fine
        }();

        res.status(200).html_view(file->view());  // ← zero copy
    }
};

class PostEcho : public routes::Post<PostEcho> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        res.status(200).json(req.body);
    }
};

class GetValue : public routes::Get<GetValue> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        static const std::unordered_map<int, int> memo{{1, 10}, {2, 20}, {3, 30}};
        for (const auto& [name, value] : req.headers) {
            std::cout << "Header: '" << name << "' = '" << value << "'\n";
        }
        res.status(200).json("{\"debug\":\"check console\"}");
        std::string keyHeader = req.header("Key");

        // Validate the header is present
        if (keyHeader.empty()) {
            res.status(400).json("{\"error\":\"Missing Key header\"}");
            return;
        }

        int key = std::stoi(keyHeader); // consider wrapping in try/catch
        auto it = memo.find(key);

        if (it == memo.end()) {
            res.status(404).json("{\"error\":\"Key not found\"}");
            return;
        }

        std::string result = "{\"value\":" + std::to_string(it->second) + "}";
        res.status(200).json(std::move(result));
    }
};

int main() {
    App app;
    app.get<GetImage>("/image");
    app.get<GetHtml>("/");
    app.get<GetValue>("/value");
    app.post<PostEcho>("/echo");   // ← same pattern
    app.listen(8080);
}