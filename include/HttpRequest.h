/**
 * @file HttpRequest.h
 * @brief Represents a parsed HTTP/1.1 request using zero-allocation views.
 */

#pragma once
#include "HttpTypes.h"
#include <string>
#include <string_view>

namespace octane 
{
    struct HttpRequest {
        std::size_t      content_length = 0;
        bool             keep_alive     = true;

        HttpMethod       method         = HttpMethod::UNKNOWN;
        std::string_view path;
        std::string_view http_version   = "HTTP/1.1";
        std::string      body;
        ContentType      content_type   = ContentType::UNKNOWN;

        StringMap params;
        HeaderMap headers;
        OwnedStringMap query;
        StringMap cookies;

        [[nodiscard]] std::string_view param(std::string_view k, std::string_view fb = {}) const noexcept {
            auto it = params.find(k);
            return it != params.end() ? it->second : fb;
        }

        [[nodiscard]] std::string_view q(std::string_view k, std::string_view fb = {}) const noexcept {
            auto it = query.find(k);
            return it != query.end() ? it->second : fb;
        }

        [[nodiscard]] std::string_view header(std::string_view k, std::string_view fb = {}) const noexcept {
            auto it = headers.find(k);
            return it != headers.end() ? it->second : fb;
        }

        [[nodiscard]] std::string_view cookie(std::string_view k, std::string_view fb = {}) const noexcept {
            auto it = cookies.find(k);
            return it != cookies.end() ? it->second : fb;
        }

        [[nodiscard]] bool is_json()      const noexcept { return content_type == ContentType::APPLICATION_JSON; }
        [[nodiscard]] bool is_form()      const noexcept { return content_type == ContentType::APPLICATION_FORM_URLENCODED; }
        [[nodiscard]] bool is_multipart() const noexcept { return content_type == ContentType::MULTIPART_FORM_DATA; }
        [[nodiscard]] bool is_html()      const noexcept { return content_type == ContentType::TEXT_HTML; }
        [[nodiscard]] bool is_text()      const noexcept { return content_type == ContentType::TEXT_PLAIN; }
        [[nodiscard]] bool has_body()     const noexcept { return !body.empty(); }
    };
}
