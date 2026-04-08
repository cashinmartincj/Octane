#pragma once
#include <string>
#include <unordered_map>
#include "HttpTypes.h"

inline const std::unordered_map<ContentType, std::string> content_type_str = {
     { ContentType::TEXT_PLAIN,                  "text/plain" },
     { ContentType::TEXT_HTML,                   "text/html" },
     { ContentType::TEXT_CSS,                    "text/css" },
     { ContentType::TEXT_JAVASCRIPT,             "text/javascript" },
     { ContentType::APPLICATION_JSON,            "application/json" },
     { ContentType::APPLICATION_XML,             "application/xml" },
     { ContentType::APPLICATION_FORM_URLENCODED, "application/x-www-form-urlencoded" },
     { ContentType::MULTIPART_FORM_DATA,         "multipart/form-data" },
     { ContentType::IMAGE_JPEG,                  "image/jpeg" },
     { ContentType::IMAGE_PNG,                   "image/png" },
    { ContentType::IMAGE_WEBP,                  "image/webp" },
    { ContentType::APPLICATION_PDF,             "application/pdf" },
    { ContentType::OCTET_STREAM,                "application/octet-stream" },
};

inline const std::unordered_map<int, std::string> status_text_str = {
     { 200, "OK" },
     { 201, "Created" },
     { 204, "No Content" },
     { 301, "Moved Permanently" },
     { 302, "Found" },
     { 400, "Bad Request" },
     { 401, "Unauthorized" },
     { 403, "Forbidden" },
     { 404, "Not Found" },
     { 405, "Method Not Allowed" },
    { 500, "Internal Server Error" },
};

struct HttpResponse {
    int         status_code  = 200;                      
    ContentType content_type = ContentType::TEXT_PLAIN;
    std::string      owned_body;   // for dynamic responses (JSON etc)
    std::string      body;
    std::string_view mapped_body;  // for mmap responses (files)
    bool             is_mapped = false;
    std::unordered_map<std::string, std::string> headers;

    // ── Setters — all chainable ──────────────────
    HttpResponse& status(int code)                { status_code = code;                    return *this; }
    HttpResponse& json  (const std::string& data) { body = data; content_type = ContentType::APPLICATION_JSON; return *this; }
    HttpResponse& html  (const std::string& data) { body = data; content_type = ContentType::TEXT_HTML;        return *this; }
    HttpResponse& text  (const std::string& data) { body = data; content_type = ContentType::TEXT_PLAIN;       return *this; }
    HttpResponse& send  (const std::string& data) { body = data;                           return *this; }
    HttpResponse& image (const std::string& data, ContentType type) { body = data; content_type = type; return *this; }
    HttpResponse& header(const std::string& key, const std::string& val) { headers[key] = val; return *this; }

    HttpResponse& html_view(std::string_view data) {
        content_type = ContentType::TEXT_HTML;
        mapped_body  = data;
        body_type    = BodyType::Mapped;
        return *this;
    }

    HttpResponse& image_view(std::string_view data, ContentType ct) {
        content_type = ct;
        mapped_body  = data;
        body_type    = BodyType::Mapped;
        return *this;
    }

    enum class BodyType { Dynamic, Owned, Mapped } body_type = BodyType::Dynamic;

    std::string_view active_body() const {
        switch (body_type) {
            case BodyType::Dynamic: return std::string_view(body);
            case BodyType::Owned:   return std::string_view(owned_body);
            case BodyType::Mapped:  return mapped_body;
            default:                return std::string_view(body);
        }
    }

    // ── Serialize ────────────────────────────────
    std::string serialize() const {
        // status text — fallback to "Unknown" if not in map
        auto sit = status_text_str.find(status_code);
        std::string stext = sit != status_text_str.end() ? sit->second : "Unknown";
        auto body_ret = active_body();   // ← picks right one automatically

        std::string response;
        response += "HTTP/1.1 " + std::to_string(status_code) + " " + stext + "\r\n";  // ✅ status text
        response += "Content-Type: " + content_type_str.at(content_type) + "\r\n"; // ✅ map lookup
        response += "Content-Length: " + std::to_string(body_ret.size()) + "\r\n";
        response += "Connection: keep-alive\r\n";   // ← add here
        response += "Keep-Alive: timeout=5\r\n";    // ← and this
        for (auto& [key, val] : headers)
            response += key + ": " + val + "\r\n";
        response += "\r\n";
        response += body_ret;
        return response;
    }
};


