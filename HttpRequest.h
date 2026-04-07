#pragma once
#include "HttpTypes.h"
#include <string>
#include <unordered_map>

struct HttpRequest {
    HttpMethod  method       = HttpMethod::UNKNOWN;
    std::string path;
    std::string http_version = "HTTP/1.1";
    std::string body;
    ContentType content_type = ContentType::UNKNOWN;  // ← add default

    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> query;
    std::unordered_map<std::string, std::string> cookies;

    // ── Helpers ───────────────────────────────────
    const std::string& param(const std::string& k, const std::string& fb = "") const {
        auto it = params.find(k); return it != params.end() ? it->second : fb;
    }
    const std::string& q(const std::string& k, const std::string& fb = "") const {
        auto it = query.find(k); return it != query.end() ? it->second : fb;
    }
    const std::string& header(const std::string& k, const std::string& fb = "") const {
        auto it = headers.find(k); return it != headers.end() ? it->second : fb;
    }
    const std::string& cookie(const std::string& k, const std::string& fb = "") const {
        auto it = cookies.find(k); return it != cookies.end() ? it->second : fb;
    }
    bool is_json()      const { return content_type == ContentType::APPLICATION_JSON; }
    bool is_form()      const { return content_type == ContentType::APPLICATION_FORM_URLENCODED; }
    bool is_multipart() const { return content_type == ContentType::MULTIPART_FORM_DATA; }
    bool is_html()      const { return content_type == ContentType::TEXT_HTML; }
    bool is_text()      const { return content_type == ContentType::TEXT_PLAIN; }
    bool has_body()     const { return !body.empty(); }
};