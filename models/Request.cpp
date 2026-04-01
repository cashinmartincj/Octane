#pragma once
#include <string_view>
#include <unordered_map>
#include <string>
struct Request {
    std::string_view method;
    std::string_view path;
    std::string_view body; 
    std::unordered_map<std::string, std::string> query;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string_view> headers;
    std::string_view header(const std::string& key) const;
};