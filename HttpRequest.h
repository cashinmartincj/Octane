#pragma once
#include "HttpResponse.h"
#include <string>
#include <unordered_map>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    ContentType content_type;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> headers;
};