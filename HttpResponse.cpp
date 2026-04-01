#include "HttpResponse.h"
#include <string>
#include <unordered_map>

std::string HttpResponse::serialize() const 
{
    std::string response;
    response += "HTTP/1.1 " + std::to_string(status) + "\r\n";
    response += "Content-Type: " + content_type_str[content_type] + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    
    for (auto& [key, val] : headers) {
        response += key + ": " + val + "\r\n";
    }
    
    response += "\r\n";
    response += body;
    return response;
}