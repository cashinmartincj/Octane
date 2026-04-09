/**
 * @file HttpResponse.h
 * @brief Represents an HTTP/1.1 response built by route handlers
 *
 * All setters are chainable:
 *   res.status(200).json("{\"ok\":true}");
 *   res.status(404).text("not found");
 *   res.status(200).html_view(file.view());  // zero copy mmap
 */

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

/**
 * @brief Maps HTTP status code to reason phrase
 * e.g. 404 → "Not Found"
 */
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
    std::unordered_map<std::string, std::string> headers;

    // ── Three body storage strategies ────────────
    // Dynamic: generated at runtime — JSON, error messages
    // Owned:   loaded into memory   — read_file()
    // Mapped:  zero copy mmap       — MappedFile::view()
    enum class BodyType { Dynamic, Owned, Mapped } body_type = BodyType::Dynamic;

    std::string      body;         // dynamic body
    std::string      owned_body;   // owned copy (read_file)
    std::string_view mapped_body;  // zero copy view into mmap memory
    bool             is_mapped = false;

    // ── Chainable setters — dynamic body ─────────

    HttpResponse& status(int code) { status_code = code; return *this; }
    HttpResponse& json  (const std::string& data) { body = data; content_type = ContentType::APPLICATION_JSON; return *this; }
    HttpResponse& html  (const std::string& data) { body = data; content_type = ContentType::TEXT_HTML;        return *this; }
    HttpResponse& text  (const std::string& data) { body = data; content_type = ContentType::TEXT_PLAIN;       return *this; }
    HttpResponse& send  (const std::string& data) { body = data; return *this; }
    HttpResponse& image (const std::string& data, ContentType type) { body = data; content_type = type; return *this; }
    HttpResponse& header(const std::string& key, const std::string& val) { headers[key] = val; return *this; }

    // ── Chainable setters — zero copy mmap body ──

    /**
     * @brief Respond with HTML from mmap memory — zero copy
     * @param data  string_view into MappedFile memory
     */
    HttpResponse& html_view(std::string_view data) {
        content_type = ContentType::TEXT_HTML;
        mapped_body  = data;
        body_type    = BodyType::Mapped;
        return *this;
    }

    /**
     * @brief Respond with image from mmap memory — zero copy
     * @param data  string_view into MappedFile memory
     * @param ct    Image ContentType e.g. ContentType::IMAGE_PNG
     */
    HttpResponse& image_view(std::string_view data, ContentType ct) {
        content_type = ct;
        mapped_body  = data;
        body_type    = BodyType::Mapped;
        return *this;
    }

    /**
     * @brief Returns the active body based on BodyType
     *
     * Dynamic → body
     * Owned   → owned_body
     * Mapped  → mapped_body (zero copy mmap view)
     */
    std::string_view active_body() const {
        switch (body_type) {
            case BodyType::Dynamic: return std::string_view(body);
            case BodyType::Owned:   return std::string_view(owned_body);
            case BodyType::Mapped:  return mapped_body;
            default:                return std::string_view(body);
        }
    }

    /**
     * @brief Serialize response to raw HTTP/1.1 bytes
     *
     * Builds the full response string:
     *   status line + headers + blank line + body
     *
     * Always includes Connection: keep-alive for persistent connections.
     *
     * @return Complete HTTP/1.1 response as std::string
     */
    std::string serialize() const {
        auto sit   = status_text_str.find(status_code);
        auto stext = sit != status_text_str.end() ? sit->second : "Unknown";
        auto body_ret = active_body();

        std::string response;
        response += "HTTP/1.1 " + std::to_string(status_code) + " " + stext + "\r\n";
        response += "Content-Type: " + content_type_str.at(content_type) + "\r\n";
        response += "Content-Length: " + std::to_string(body_ret.size()) + "\r\n";
        response += "Connection: keep-alive\r\n";
        response += "Keep-Alive: timeout=5\r\n";
        for (auto& [key, val] : headers)
            response += key + ": " + val + "\r\n";
        response += "\r\n";
        response += body_ret;
        return response;
    }
};