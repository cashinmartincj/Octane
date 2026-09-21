/**
 * @file HttpResponse.h
 * @brief Zero-allocation structured response with optional full-serialization support.
 */

#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include "HttpTypes.h"

namespace octane
{
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

    inline constexpr std::string_view get_status_text_sv(int code) noexcept {
        switch (code) {
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
            case 505: return "HTTP Version Not Supported";
            default:  return "Unknown";
        }
    }

    struct HttpResponse {
        int         status_code  = 200;
        ContentType content_type = ContentType::TEXT_PLAIN;
        
        std::unordered_map<std::string, std::string> headers;

        enum class BodyType { Dynamic, Owned, Mapped } body_type = BodyType::Dynamic;

        std::string      body;
        std::string      owned_body;
        std::string_view mapped_body;
        bool             is_mapped = false;

        HttpResponse& status(int code) noexcept { status_code = code; return *this; }
        
        HttpResponse& json(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = ContentType::APPLICATION_JSON; 
            return *this; 
        }

        HttpResponse& html(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = ContentType::TEXT_HTML; 
            return *this; 
        }

        HttpResponse& text(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = ContentType::TEXT_PLAIN; 
            return *this; 
        }

        HttpResponse& send(std::string_view data) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            return *this; 
        }

        HttpResponse& image(std::string_view data, ContentType type) { 
            body.assign(data); 
            body_type = BodyType::Dynamic; 
            content_type = type; 
            return *this; 
        }

        HttpResponse& header(const std::string& key, const std::string& val) { 
            headers[key] = val; 
            return *this; 
        }

        HttpResponse& html_view(std::string_view data) noexcept {
            content_type = ContentType::TEXT_HTML; 
            mapped_body = data; 
            body_type = BodyType::Mapped; 
            return *this;
        }

        HttpResponse& image_view(std::string_view data, ContentType ct) noexcept {
            content_type = ct; 
            mapped_body = data; 
            body_type = BodyType::Mapped; 
            return *this;
        }

        [[nodiscard]] std::string_view active_body() const noexcept {
            switch (body_type) {
                case BodyType::Dynamic: return std::string_view(body);
                case BodyType::Owned:   return std::string_view(owned_body);
                case BodyType::Mapped:  return mapped_body;
                default:                return std::string_view(body);
            }
        }

        [[nodiscard]] std::string serialize_headers(bool close = false) const {
            if (status_code < 200 || status_code > 599) throw std::invalid_argument("Unsupported response status");
            std::string_view stext = get_status_text_sv(status_code);
            auto body_ret = active_body();

            std::string response;
            response.reserve(256);
            
            response.append("HTTP/1.1 ").append(std::to_string(status_code)).append(" ").append(stext).append("\r\n");
            
            bool custom_content_type = false;
            for (const auto& [key, val] : headers) {
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

        // Full serialization for testing, dispatchers, and monolithic writes
        std::string serialize(bool close = false, bool head = false) const {
            std::string full = serialize_headers(close);
            if (!head && status_code != 204 && status_code != 304) {
                full.append(active_body());
            }
            return full;
        }

        // Test utility conveniences matching serialized representations
        bool starts_with(std::string_view prefix) const {
            return serialize().starts_with(prefix);
        }

        std::size_t find(std::string_view needle) const {
            return serialize().find(needle);
        }

        operator std::string() const {
            return serialize();
        }
    };
}