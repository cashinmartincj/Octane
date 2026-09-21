#pragma once
#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <cstdint>

namespace octane
{
    // Fast FNV-1a transparent hash for string_view and std::string
    struct StringViewHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
            std::size_t hash = 14695981039346656037ULL;
            for (char c : sv) {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
            return (*this)(std::string_view(s));
        }
    };

    // Case-insensitive hash for HTTP header normalization lookups
    struct CaseInsensitiveStringViewHash {
        using is_transparent = void;

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

        [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
            return (*this)(std::string_view(s));
        }
    };

    struct CaseInsensitiveEqual {
        using is_transparent = void;

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

    using StringMap = std::unordered_map<std::string_view, std::string_view,
                                         StringViewHash, std::equal_to<>>;

    using HeaderMap = std::unordered_map<std::string_view, std::string_view,
                                         CaseInsensitiveStringViewHash, CaseInsensitiveEqual>;

    struct HttpRequest;
    struct HttpResponse;

    using Handler = void(*)(HttpRequest&, HttpResponse&);

    enum class ContentType : uint8_t {
        TEXT_PLAIN, TEXT_HTML, TEXT_CSS, TEXT_JAVASCRIPT, APPLICATION_JSON,
        APPLICATION_XML, APPLICATION_PDF, APPLICATION_FORM_URLENCODED,
        MULTIPART_FORM_DATA, IMAGE_JPEG, IMAGE_PNG, IMAGE_GIF, IMAGE_WEBP,
        IMAGE_SVG, OCTET_STREAM, UNKNOWN
    };

    enum class HttpMethod : uint8_t { 
        GET = 0, POST = 1, PUT = 2, PATCH = 3, DEL = 4, OPTIONS = 5, HEAD = 6, UNKNOWN = 7 
    };

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
}