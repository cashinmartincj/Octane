/**
 * @file HttpRequest.h
 * @brief Represents a parsed HTTP/1.1 request
 *
 * HttpParser fills this struct from raw TCP bytes.
 * Route handlers receive it as a const reference — read only.
 *
 * Example:
 *   void handle(const HttpRequest& req, HttpResponse& res) {
 *       auto id    = req.param("id");        // route param  /users/:id
 *       auto page  = req.q("page");          // query string ?page=2
 *       auto token = req.header("Authorization");
 *       auto body  = req.body;               // raw request body
 *   }
 */

#pragma once
#include "HttpTypes.h"
#include <string>
#include <unordered_map>

struct HttpRequest {

    // ── Core fields ───────────────────────────────
    HttpMethod  method       = HttpMethod::UNKNOWN;   // GET, POST, PUT etc
    std::string path;                                  // /users/42
    std::string http_version = "HTTP/1.1";            // HTTP version string
    std::string body;                                  // raw request body
    ContentType content_type = ContentType::UNKNOWN;  // parsed Content-Type

    // ── Parsed collections ────────────────────────
    std::unordered_map<std::string, std::string> params;   // route params  :id → "42"
    std::unordered_map<std::string, std::string> headers;  // request headers
    std::unordered_map<std::string, std::string> query;    // query string  ?page=2
    std::unordered_map<std::string, std::string> cookies;  // parsed cookies

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

    // ── Content-Type helpers ──────────────────────
    bool is_json()      const { return content_type == ContentType::APPLICATION_JSON; }
    bool is_form()      const { return content_type == ContentType::APPLICATION_FORM_URLENCODED; }
    bool is_multipart() const { return content_type == ContentType::MULTIPART_FORM_DATA; }
    bool is_html()      const { return content_type == ContentType::TEXT_HTML; }
    bool is_text()      const { return content_type == ContentType::TEXT_PLAIN; }
    bool has_body()     const { return !body.empty(); }
};