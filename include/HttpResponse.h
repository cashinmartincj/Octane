/**
 * @file HttpResponse.h
 * @brief Zero-allocation structured HTTP response representation and serialization engine.
 *
 * @details
 * `HttpResponse` encapsulates an HTTP/1.1 response status code, content type, headers,
 * and body payload. It supports three distinct body storage models (`BodyType`):
 * - `Dynamic`: Body stored in an internal `std::string` buffer (`res.json(...)`, `res.text(...)`).
 * - `Owned`: Body moved into `owned_body` for async task transfers or offloaded execution queues.
 * - `Mapped`: Zero-copy non-owning `std::string_view` referencing memory-mapped files or static assets
 *   (`res.html_view(...)`, `res.image_view(...)`), avoiding heap allocation entirely.
 *
 * Header serialization produces standard RFC 9112 compliant HTTP/1.1 response framing,
 * automatically computing `Content-Length` (omitted for 204 No Content and 304 Not Modified),
 * setting `Connection: keep-alive` or `Connection: close`, and validating custom header names
 * and values against RFC specifications.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "HttpResponse.h"` or via `#include "Octane.h"`
 * - Handlers: Returned by value from all route handlers (`[](HttpRequest& req, HttpResponse& res) -> HttpResponse`)
 * - Transport: Read by `octane::transport::TcpConnection` to emit response headers and body fragments to `io_uring`
 * - Dispatcher: Used by `octane::core::RequestDispatcher` for synchronous and offloaded route fulfillment
 * - Static Files: Combined with `FileHandle` to stream memory-mapped files without user-space buffer copies
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include <utility>
#include "HttpTypes.h"

namespace octane
{
    /**
     * @brief Translates an internal ContentType enum value into an RFC-compliant MIME string view.
     * @param type ContentType enum variant.
     * @return Canonical MIME type string literal (e.g., "application/json", "text/html").
     *
     * @note Executed at compile-time / zero-cost (`noexcept constexpr`).
     */
    inline constexpr std::string_view get_content_type_sv(ContentType type) noexcept {
        switch (type) {
            case ContentType::TEXT_PLAIN:                  return "text/plain";
            case ContentType::TEXT_HTML:                   return "text/html";
            case ContentType::TEXT_CSS:                    return "text/css";
            case ContentType::TEXT_JAVASCRIPT:             return "text/javascript";
            case ContentType::APPLICATION_JSON:            return "application/json";
            case ContentType::APPLICATION_XML:             return "application/xml";
            case ContentType::APPLICATION_FORM_URLENCODED: return "application/x-www-form-urlencoded";
            case ContentType::MULTIPART_FORM_DATA:         return "multipart/form-data";
            case ContentType::IMAGE_JPEG:                  return "image/jpeg";
            case ContentType::IMAGE_PNG:                   return "image/png";
            case ContentType::IMAGE_GIF:                   return "image/gif";
            case ContentType::IMAGE_WEBP:                  return "image/webp";
            case ContentType::IMAGE_SVG:                   return "image/svg+xml";
            case ContentType::APPLICATION_PDF:             return "application/pdf";
            case ContentType::OCTET_STREAM:                return "application/octet-stream";
            default:                                       return "application/octet-stream";
        }
    }

    /**
     * @brief Translates standard HTTP status codes into canonical RFC reason phrases.
     * @param code Numeric HTTP status code (e.g., 200, 404, 500).
     * @return Canonical reason phrase string literal (e.g., "OK", "Not Found").
     */
    inline constexpr std::string_view get_status_text_sv(int code) noexcept {
        switch (code) {
            case 101: return "Switching Protocols";
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 413: return "Content Too Large";
            case 417: return "Expectation Failed";
            case 431: return "Request Header Fields Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 503: return "Service Unavailable";
            case 505: return "HTTP Version Not Supported";
            default:  return "Unknown";
        }
    }

    /**
     * @struct HttpResponse
     * @brief Fluent builder and container for outgoing HTTP/1.1 responses.
     *
     * @details
     * Provides chainable setters (`.status(200).json(...)`) for constructing responses.
     * Supports zero-copy memory-mapped file output via `.html_view(...)` or `.image_view(...)`,
     * dynamic heap-backed bodies for JSON / text endpoints, and custom header definitions.
     *
     * Example usage in a route handler:
     * @code
     * app.get("/api/v1/health", [](HttpRequest& req, HttpResponse& res) -> HttpResponse {
     *     return res.status(200).json(R"({"status":"ok"})");
     * });
     * @endcode
     */
    struct HttpResponse {
        /// HTTP status code (default: 200 OK)
        int         status_code  = 200;

        /// Content-Type MIME category (default: TEXT_PLAIN)
        ContentType content_type = ContentType::TEXT_PLAIN;
        
        /// Key-value map of custom response headers
        std::unordered_map<std::string, std::string> headers;

        /**
         * @enum BodyType
         * @brief Defines the internal memory ownership model of the response body.
         */
        enum class BodyType {
            Dynamic, ///< Body is stored in the internal `std::string body` buffer.
            Owned,   ///< Body is owned via `std::string owned_body` (transferred across threads).
            Mapped   ///< Body is a borrowed `std::string_view` referencing mmap/static memory.
        } body_type = BodyType::Dynamic;

        std::string      body;        ///< Standard heap-allocated dynamic response body
        std::string      owned_body;  ///< Transferred/moved response payload for worker queues
        std::string_view mapped_body; ///< Non-owning view pointing into mmap'd static files or literals
        bool             is_mapped = false; ///< True if response body is memory mapped

        /**
         * @brief Sets the numeric HTTP status code.
         * @param code Standard HTTP status code (200, 400, 404, 500, etc.).
         * @return Fluent reference to `*this`.
         */
        HttpResponse& status(int code) noexcept { status_code = code; return *this; }
        
        /**
         * @brief Sets response content type to application/json and copies data to body.
         * @param data JSON string or string_view.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& json(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = ContentType::APPLICATION_JSON; 
            return *this; 
        }

        /**
         * @brief Sets response content type to text/html and copies data to body.
         * @param data HTML markup string.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& html(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = ContentType::TEXT_HTML; 
            return *this; 
        }

        /**
         * @brief Sets response content type to text/plain and copies data to body.
         * @param data Plain text string.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& text(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = ContentType::TEXT_PLAIN; 
            return *this; 
        }

        /**
         * @brief Assigns raw data to response body without altering current content type.
         * @param data Binary or string payload.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& send(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            return *this; 
        }

        /**
         * @brief Assigns image bytes and sets the appropriate image MIME content type.
         * @param data Binary image buffer.
         * @param type ContentType enum indicating image format (e.g., IMAGE_PNG, IMAGE_JPEG).
         * @return Fluent reference to `*this`.
         */
        HttpResponse& image(std::string_view data, ContentType type) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = type; 
            return *this; 
        }

        /**
         * @brief Adds or replaces an HTTP response header.
         * @param key Header name (case-insensitive in HTTP/1.1; checked during serialization).
         * @param val Header value.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& header(const std::string& key, const std::string& val) { 
            headers[key] = val; 
            return *this; 
        }

        /**
         * @brief Emits an HTML response from a non-owning string_view (zero copy).
         * @details Useful for static pages or mmap'd assets. Caller must guarantee lifetime.
         * @param data Valid HTML string view.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& html_view(std::string_view data) noexcept {
            content_type = ContentType::TEXT_HTML; 
            mapped_body = data; 
            body_type = BodyType::Mapped; 
            return *this;
        }

        /**
         * @brief Emits an image response from a non-owning string_view (zero copy).
         * @param data Non-owning buffer pointing to memory-mapped image data.
         * @param ct Image MIME enum type.
         * @return Fluent reference to `*this`.
         */
        HttpResponse& image_view(std::string_view data, ContentType ct) noexcept {
            content_type = ct; 
            mapped_body = data; 
            body_type = BodyType::Mapped; 
            return *this;
        }

        /**
         * @brief Resolves the active body view based on current BodyType mode.
         * @return std::string_view representing the active body bytes.
         */
        [[nodiscard]] std::string_view active_body() const noexcept {
            switch (body_type) {
                case BodyType::Dynamic: return std::string_view(body);
                case BodyType::Owned:   return std::string_view(owned_body);
                case BodyType::Mapped:  return mapped_body;
                default:                std::unreachable();
            }
        }

        /**
         * @brief Serializes HTTP/1.1 response status line and headers into a formatted string.
         * @param close When true, sets `Connection: close`; otherwise `Connection: keep-alive`.
         * @return Formatted HTTP/1.1 headers string ending with `\r\n\r\n`.
         * @throws std::invalid_argument If status_code is outside [200, 599] or headers contain illegal chars.
         */
        [[nodiscard]] std::string serialize_headers(bool close = false) const {
            if (status_code != 101 && (status_code < 200 || status_code > 599)) throw std::invalid_argument("Unsupported response status");
            std::string_view stext = get_status_text_sv(status_code);
            auto body_ret = active_body();

            std::string response;
            response.reserve(256);
            
            response.append("HTTP/1.1 ").append(std::to_string(status_code)).append(" ").append(stext).append("\r\n");
            
            if (status_code == 101) {
                // 101 Switching Protocols: output custom headers directly (Upgrade, Connection, Sec-WebSocket-Accept)
                for (const auto& [key, val] : headers) {
                    if (!valid_header_name(key) || !valid_header_value(val)) {
                        throw std::invalid_argument("Invalid HTTP response header");
                    }
                    response.append(key).append(": ").append(val).append("\r\n");
                }
                response.append("\r\n");
                return response;
            }

            bool custom_content_type = false;
            for (const auto& [key, val] : headers) {
                if (!valid_header_name(key) || !valid_header_value(val)) {
                    throw std::invalid_argument("Invalid HTTP response header");
                }
                if (key.size() == 12 && 
                    (key[0] == 'c' || key[0] == 'C') && 
                    CaseInsensitiveEqual{}(key, "content-type")) {
                    custom_content_type = true;
                    break;
                }
            }
                
            if (!custom_content_type) {
                response.append("Content-Type: ").append(get_content_type_sv(content_type)).append("\r\n");
            }
                
            if (status_code != 204 && status_code != 304) {
                response.append("Content-Length: ").append(std::to_string(body_ret.size())).append("\r\n");
            }
                
            response.append(close ? "Connection: close\r\n" : "Connection: keep-alive\r\n");
            
            for (const auto& [key, val] : headers) {
                if (CaseInsensitiveEqual{}(key, "content-length") || 
                    CaseInsensitiveEqual{}(key, "transfer-encoding") ||
                    CaseInsensitiveEqual{}(key, "connection")) continue;
                response.append(key).append(": ").append(val).append("\r\n");
            }
            
            response.append("\r\n");
            return response;
        }

        /**
         * @brief Validates that an HTTP header name complies with RFC 9110 token requirements.
         * @param name Header field name.
         * @return True if valid token characters only; false otherwise.
         */
        [[nodiscard]] static bool valid_header_name(std::string_view name) noexcept {
            if (name.empty()) return false;
            for (unsigned char c : name) {
                const bool alpha_numeric =
                    (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9');
                if (!alpha_numeric &&
                    std::string_view("!#$%&'*+-.^_`|~").find(
                        static_cast<char>(c)) == std::string_view::npos) {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Validates that an HTTP header value contains only permitted visible ASCII or tab characters.
         * @param value Header value string.
         * @return True if valid; false if contains control characters (CR, LF, NUL, DEL, etc.).
         */
        [[nodiscard]] static bool valid_header_value(std::string_view value) noexcept {
            for (unsigned char c : value) {
                if ((c < 32 && c != '\t') || c == 127) return false;
            }
            return true;
        }

        /**
         * @brief Full serialization for testing, dispatchers, and monolithic socket writes.
         * @param close Whether to append `Connection: close`.
         * @param head Whether this is a HEAD request (omits body bytes, keeps headers).
         * @return Complete serialized HTTP/1.1 message string.
         */
        std::string serialize(bool close = false, bool head = false) const {
            std::string full = serialize_headers(close);
            if (!head && status_code != 204 && status_code != 304) {
                full.append(active_body());
            }
            return full;
        }

        /**
         * @brief Test utility checking if serialized response begins with a prefix.
         * @param prefix Expected prefix (e.g., "HTTP/1.1 200 OK").
         * @return True if prefix matches.
         */
        bool starts_with(std::string_view prefix) const {
            return serialize().starts_with(prefix);
        }

        /**
         * @brief Test utility searching for a substring within serialized response.
         * @param needle Substring to search for.
         * @return Position if found, std::string::npos otherwise.
         */
        std::size_t find(std::string_view needle) const {
            return serialize().find(needle);
        }

        /**
         * @brief Implicit string conversion operator for testing and debugging.
         */
        operator std::string() const {
            return serialize();
        }
    };
}
