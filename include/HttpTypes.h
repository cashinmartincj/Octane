/**
 * @file HttpTypes.h
 * @brief Core types shared across the Octane framework
 *
 * Contains:
 *   Handler     — function pointer type for route callbacks
 *   ContentType — enum of supported MIME types
 *   HttpMethod  — enum of supported HTTP methods
 *   parseContentType() — parses raw Content-Type header into enum
 *
 * Forward declares HttpRequest and HttpResponse to avoid
 * circular includes across the framework headers.
 */

#pragma once
#include <string>
#include <unordered_map>

// forward declarations — avoid circular includes
struct HttpRequest;
struct HttpResponse;

/**
 * @brief Route handler function pointer type
 *
 * Every route handler in Octane has this signature:
 *   void handle(HttpRequest& req, HttpResponse& res)
 *
 * req  — incoming request, read only
 * res  — outgoing response, write to this
 */
using Handler = void(*)(HttpRequest&, HttpResponse&);

/**
 * @brief Supported MIME / Content-Type values
 *
 * Used in both HttpRequest (incoming Content-Type)
 * and HttpResponse (outgoing Content-Type).
 */
enum class ContentType {
    TEXT_PLAIN,
    TEXT_HTML,
    TEXT_CSS,
    TEXT_JAVASCRIPT,
    APPLICATION_JSON,
    APPLICATION_XML,
    APPLICATION_PDF,
    APPLICATION_FORM_URLENCODED,
    MULTIPART_FORM_DATA,
    IMAGE_JPEG,
    IMAGE_PNG,
    IMAGE_GIF,
    IMAGE_WEBP,
    IMAGE_SVG,
    OCTET_STREAM,
    UNKNOWN
};

/**
 * @brief Supported HTTP methods
 *
 * DEL is used instead of DELETE — DELETE is a reserved
 * macro on some platforms (Windows).
 */
enum class HttpMethod {
    GET,
    POST,
    PUT,
    PATCH,
    DEL,
    OPTIONS,
    HEAD,
    UNKNOWN
};

inline ContentType parseContentType(const std::string& raw) {
    static const std::unordered_map<std::string, ContentType> map = {
        { "text/plain",                        ContentType::TEXT_PLAIN },
        { "text/html",                         ContentType::TEXT_HTML },
        { "text/css",                          ContentType::TEXT_CSS },
        { "text/javascript",                   ContentType::TEXT_JAVASCRIPT },
        { "application/json",                  ContentType::APPLICATION_JSON },
        { "application/xml",                   ContentType::APPLICATION_XML },
        { "application/pdf",                   ContentType::APPLICATION_PDF },
        { "application/x-www-form-urlencoded", ContentType::APPLICATION_FORM_URLENCODED },
        { "multipart/form-data",               ContentType::MULTIPART_FORM_DATA },
        { "image/jpeg",                        ContentType::IMAGE_JPEG },
        { "image/png",                         ContentType::IMAGE_PNG },
        { "image/gif",                         ContentType::IMAGE_GIF },
        { "image/webp",                        ContentType::IMAGE_WEBP },
        { "image/svg+xml",                     ContentType::IMAGE_SVG },
        { "application/octet-stream",          ContentType::OCTET_STREAM },
    };

    auto semicolon = raw.find(';');
    std::string key = semicolon != std::string::npos ? raw.substr(0, semicolon) : raw;
    while (!key.empty() && key.back() == ' ') key.pop_back();

    auto it = map.find(key);
    return it != map.end() ? it->second : ContentType::UNKNOWN;
}