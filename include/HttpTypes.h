#pragma once

/**
 * @file HttpTypes.h
 * @brief Common HTTP enumerations, type aliases, hashing functors, and execution policies.
 *
 * @details
 * This header defines fundamental types shared across the Octane framework:
 * - Transparent FNV-1a hash functors for zero-allocation `std::string_view` lookups.
 * - Case-insensitive header maps according to RFC 9110.
 * - Route handler function pointers and execution mode descriptors.
 * - HTTP methods and common MIME Content-Type enumerations.
 *
 * @section usage Usage:
 * @code
 * #include "HttpTypes.h"
 *
 * octane::ContentType type = octane::parseContentType("application/json; charset=utf-8");
 * if (type == octane::ContentType::APPLICATION_JSON) { ... }
 * @endcode
 */

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace octane {

/**
 * @struct StringViewHash
 * @brief Fast 64-bit FNV-1a hash functor supporting transparent heterogeneous lookup.
 * @details Allows `std::unordered_map` with `std::string_view` keys to find elements using
 * `std::string` without constructing intermediate temporary string objects.
 */
struct StringViewHash {
    using is_transparent = void;

    /**
     * @brief Computes 64-bit FNV-1a hash over a string_view.
     * @param sv Input string view.
     * @return 64-bit hash value.
     */
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        std::size_t hash = 14695981039346656037ULL;
        for (char c : sv) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    /**
     * @brief Overload for std::string keys.
     * @param s Input string.
     * @return 64-bit hash value.
     */
    [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
        return (*this)(std::string_view(s));
    }
};

/**
 * @struct CaseInsensitiveStringViewHash
 * @brief Case-insensitive 64-bit FNV-1a hash functor for HTTP header lookups.
 * @details Normalizes ASCII uppercase characters (`A-Z`) to lowercase (`a-z`) during hashing,
 * ensuring case-insensitive header matching mandated by RFC 9110.
 */
struct CaseInsensitiveStringViewHash {
    using is_transparent = void;

    /**
     * @brief Computes case-folded FNV-1a hash over a string_view.
     * @param sv Header name string view.
     * @return Case-folded 64-bit hash value.
     */
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        std::size_t hash = 14695981039346656037ULL;
        for (char c : sv) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 'A' && uc <= 'Z') uc += 32;
            hash ^= uc;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    /**
     * @brief Overload for std::string headers.
     * @param s Header name string.
     * @return Case-folded 64-bit hash value.
     */
    [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
        return (*this)(std::string_view(s));
    }
};

/**
 * @struct CaseInsensitiveEqual
 * @brief Case-insensitive equality comparator for ASCII HTTP header keys.
 */
struct CaseInsensitiveEqual {
    using is_transparent = void;

    /**
     * @brief Case-insensitively compares two string views.
     * @param a First string view.
     * @param b Second string view.
     * @return true if strings match ignoring ASCII case, false otherwise.
     */
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            unsigned char ca = static_cast<unsigned char>(a[i]);
            unsigned char cb = static_cast<unsigned char>(b[i]);
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    }
};

/**
 * @typedef StringMap
 * @brief Zero-allocation hash map storing non-owning string view key-value pairs (e.g. query params, route params).
 */
using StringMap = std::unordered_map<std::string_view, std::string_view,
                                     StringViewHash, std::equal_to<>>;

/**
 * @typedef OwnedStringMap
 * @brief Hash map storing owned `std::string` key-value pairs for offloaded request contexts.
 */
using OwnedStringMap = std::unordered_map<std::string, std::string,
                                          StringViewHash, std::equal_to<>>;

/**
 * @typedef HeaderMap
 * @brief Fast case-insensitive hash map storing HTTP request and response headers.
 */
using HeaderMap = std::unordered_map<std::string_view, std::string_view,
                                     CaseInsensitiveStringViewHash, CaseInsensitiveEqual>;

struct HttpRequest;
struct HttpResponse;

/**
 * @typedef Handler
 * @brief Standard route handler function pointer signature.
 * @details Signature: `void(HttpRequest& req, HttpResponse& res)`.
 */
using Handler = void(*)(HttpRequest&, HttpResponse&);

/**
 * @enum HandlerExecutionMode
 * @brief Dictates where and how a route handler executes relative to the io_uring ring thread.
 */
enum class HandlerExecutionMode : uint8_t {
    Inline,         ///< Executes directly on the owning io_uring worker thread (fast path, non-blocking)
    SharedBlocking, ///< Offloads request to a shared bounded worker queue (for light blocking I/O)
    Named           ///< Offloads request to a dedicated named worker pool (e.g. "database", "export")
};

/**
 * @struct HandlerExecution
 * @brief Encapsulates the execution policy for an individual route handler.
 */
struct HandlerExecution {
    HandlerExecutionMode mode{HandlerExecutionMode::Inline}; ///< Selected execution mode
    std::string queue_name;                                   ///< Target worker queue name when mode is Named

    /**
     * @brief Creates an inline execution policy (runs on io_uring ring thread).
     */
    [[nodiscard]] static HandlerExecution inline_execution() {
        return {};
    }

    /**
     * @brief Creates an offloaded execution policy targeting the shard's shared blocking worker queue.
     */
    [[nodiscard]] static HandlerExecution shared_blocking() {
        return {HandlerExecutionMode::SharedBlocking, {}};
    }

    /**
     * @brief Creates an offloaded execution policy targeting a dedicated named worker pool.
     * @param name Name of the worker queue (e.g., "database").
     */
    [[nodiscard]] static HandlerExecution named(std::string name) {
        return {HandlerExecutionMode::Named, std::move(name)};
    }
};

/**
 * @struct HandlerRoute
 * @brief Binds a callable handler function pointer to its execution policy.
 */
struct HandlerRoute {
    Handler handler{nullptr};       ///< Target function pointer
    HandlerExecution execution{};   ///< Associated execution policy
};

/**
 * @enum ContentType
 * @brief Standard HTTP MIME media types recognized by Octane.
 */
enum class ContentType : uint8_t {
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
 * @enum HttpMethod
 * @brief Standard HTTP request methods.
 */
enum class HttpMethod : uint8_t { 
    GET = 0,
    POST = 1,
    PUT = 2,
    PATCH = 3,
    DEL = 4,
    OPTIONS = 5,
    HEAD = 6,
    UNKNOWN = 7 
};

/**
 * @brief Parses an HTTP Content-Type header string into a typed `ContentType` enum.
 * @details Strips optional MIME parameters such as `; charset=utf-8` before matching.
 * @param raw Raw Content-Type header value.
 * @return Recognized `ContentType` enum value, or `ContentType::UNKNOWN` if unrecognized.
 */
inline ContentType parseContentType(std::string_view raw) noexcept {
    auto semicolon = raw.find(';');
    std::string_view key = raw.substr(0, semicolon);
    while (!key.empty() && key.back() == ' ') key.remove_suffix(1);

    if (key == "text/plain")                        return ContentType::TEXT_PLAIN;
    if (key == "application/json")                  return ContentType::APPLICATION_JSON;
    if (key == "text/html")                         return ContentType::TEXT_HTML;
    if (key == "text/css")                          return ContentType::TEXT_CSS;
    if (key == "text/javascript")                   return ContentType::TEXT_JAVASCRIPT;
    if (key == "application/x-www-form-urlencoded") return ContentType::APPLICATION_FORM_URLENCODED;
    if (key == "multipart/form-data")               return ContentType::MULTIPART_FORM_DATA;
    if (key == "image/png")                         return ContentType::IMAGE_PNG;
    if (key == "image/jpeg")                        return ContentType::IMAGE_JPEG;
    if (key == "image/webp")                        return ContentType::IMAGE_WEBP;
    if (key == "image/gif")                         return ContentType::IMAGE_GIF;
    if (key == "image/svg+xml")                     return ContentType::IMAGE_SVG;
    if (key == "application/xml")                   return ContentType::APPLICATION_XML;
    if (key == "application/pdf")                   return ContentType::APPLICATION_PDF;
    if (key == "application/octet-stream")          return ContentType::OCTET_STREAM;

    return ContentType::UNKNOWN;
}

} // namespace octane
