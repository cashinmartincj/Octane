#pragma once

/**
 * @file HttpRequest.h
 * @brief Represents a parsed HTTP request using zero-allocation non-owning string views.
 *
 * @details
 * `HttpRequest` contains the parsed representation of an incoming HTTP/1.0 or HTTP/1.1
 * request. For maximum performance and zero memory copies on the hot path, path segments,
 * headers, and route parameters are exposed as non-owning `std::string_view` slices over
 * the socket read buffer.
 *
 * @section usage Usage Example in Route Handlers:
 * @code
 * #include "HttpRequest.h"
 * #include "HttpResponse.h"
 *
 * void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
 *     // Route parameter from dynamic path (/users/:id):
 *     std::string_view id = req.param("id");
 *
 *     // URL query parameter (?sort=desc):
 *     std::string_view sort = req.q("sort", "asc"); // with default fallback
 *
 *     // HTTP Request header (case-insensitive):
 *     std::string_view auth = req.header("Authorization");
 *
 *     // HTTP Cookie:
 *     std::string_view session = req.cookie("sessionid");
 *
 *     // Body payload:
 *     if (req.is_json()) {
 *         // process req.body ...
 *     }
 * }
 * @endcode
 *
 * @warning String View Lifetime Invariant:
 * Return values from `req.param()`, `req.header()`, `req.cookie()`, and `req.path` are non-owning
 * views borrowed directly from the connection's read buffer. They remain valid ONLY for the
 * duration of the handler invocation. If you offload tasks to background threads or retain values
 * in long-lived data structures, you MUST copy them into owned `std::string` objects!
 */

#include "HttpTypes.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace octane {

/**
 * @struct HttpRequest
 * @brief Represents a fully received and parsed incoming HTTP request.
 */
struct HttpRequest {
    /// Declared Content-Length from request headers, or 0 if omitted.
    std::size_t content_length = 0;

    /// Whether the client requested a persistent connection (`Connection: keep-alive`).
    bool keep_alive = true;

    /// Parsed HTTP verb (GET, POST, PUT, PATCH, DEL, OPTIONS, HEAD).
    HttpMethod method = HttpMethod::UNKNOWN;

    /// Target request path as a non-owning string view (e.g. `"/api/users"`).
    std::string_view path;

    /// HTTP protocol version string (e.g. `"HTTP/1.1"` or `"HTTP/1.0"`).
    std::string_view http_version = "HTTP/1.1";

    /// Raw request body string.
    std::string body;

    /// Structured MIME type derived from the Content-Type header.
    ContentType content_type = ContentType::UNKNOWN;

    /// Dynamic path parameters extracted by the router (e.g. `:id` -> `"42"`).
    StringMap params;

    /// Case-insensitive map of HTTP request headers.
    HeaderMap headers;

    /// URL-decoded query parameters (e.g. `?search=term` -> `"search": "term"`).
    OwnedStringMap query;

    /// All values of repeated query keys, in arrival order (Django getlist parity).
    std::unordered_map<std::string,std::vector<std::string>> query_lists;

    /// Parsed cookies extracted from the `Cookie:` header.
    StringMap cookies;

    /**
     * @brief Retrieves a dynamic route path parameter.
     * @param k The parameter name without leading colon (e.g. `"id"` for route `/users/:id`).
     * @param fb Fallback string view returned if the parameter is absent.
     * @return Parameter value view, or fallback if absent.
     */
    [[nodiscard]] std::string_view param(std::string_view k, std::string_view fb = {}) const noexcept {
        auto it = params.find(k);
        return it != params.end() ? it->second : fb;
    }

    /**
     * @brief Retrieves a URL query string parameter.
     * @param k Query parameter name.
     * @param fb Fallback string view returned if the query parameter is absent.
     * @return Decoded query parameter value, or fallback if absent.
     */
    [[nodiscard]] std::string_view q(std::string_view k, std::string_view fb = {}) const noexcept {
        auto it = query.find(std::string(k));
        return it != query.end() ? it->second : fb;
    }

    [[nodiscard]] std::vector<std::string> qlist(std::string_view key) const {
        auto all=query_lists.find(std::string(key));
        if(all!=query_lists.end())return all->second;
        auto single=query.find(std::string(key));
        return single==query.end()?std::vector<std::string>{}:std::vector<std::string>{single->second};
    }

    /**
     * @brief Retrieves an HTTP header value by name (case-insensitive).
     * @param k Header name (e.g., `"Content-Type"` or `"authorization"`).
     * @param fb Fallback string view returned if the header was not sent.
     * @return Header value string view, or fallback if absent.
     */
    [[nodiscard]] std::string_view header(std::string_view k, std::string_view fb = {}) const noexcept {
        auto it = headers.find(k);
        return it != headers.end() ? it->second : fb;
    }

    /**
     * @brief Retrieves an HTTP cookie value by name.
     * @param k Cookie name (e.g., `"sessionid"` or `"csrftoken"`).
     * @param fb Fallback string view returned if the cookie is absent.
     * @return Cookie value string view, or fallback if absent.
     */
    [[nodiscard]] std::string_view cookie(std::string_view k, std::string_view fb = {}) const noexcept {
        auto it = cookies.find(k);
        return it != cookies.end() ? it->second : fb;
    }

    /// Checks if Content-Type is application/json.
    [[nodiscard]] bool is_json() const noexcept { return content_type == ContentType::APPLICATION_JSON; }

    /// Checks if Content-Type is application/x-www-form-urlencoded.
    [[nodiscard]] bool is_form() const noexcept { return content_type == ContentType::APPLICATION_FORM_URLENCODED; }

    /// Checks if Content-Type is multipart/form-data.
    [[nodiscard]] bool is_multipart() const noexcept { return content_type == ContentType::MULTIPART_FORM_DATA; }

    /// Checks if Content-Type is text/html.
    [[nodiscard]] bool is_html() const noexcept { return content_type == ContentType::TEXT_HTML; }

    /// Checks if Content-Type is text/plain.
    [[nodiscard]] bool is_text() const noexcept { return content_type == ContentType::TEXT_PLAIN; }

    /// Checks if the request body is non-empty.
    [[nodiscard]] bool has_body() const noexcept { return !body.empty(); }
};

} // namespace octane
