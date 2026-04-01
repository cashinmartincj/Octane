#pragma once
#include <string>
#include <unordered_map>
class Response {
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    public:
        Response& status(int code);
        void send(const std::string& text);
        void html(const std::string& path);
        void json(const std::string& data);
        Response& set(const std::string& key, const std::string& value);
        std::string build();
};