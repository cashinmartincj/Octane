/**
 * @file HttpParser.h
 * @brief Parses bounded HTTP requests with zero heap allocations in the hot path.
 */

#pragma once
#include "HttpRequest.h"
#include "HttpTypes.h"
#include "HttpLimits.h"
#include <string_view>
#include <charconv>
#include <stdexcept>

namespace octane
{
    struct HttpParseError : std::runtime_error {
        int status;
        explicit HttpParseError(int code) : std::runtime_error("Invalid HTTP request"), status(code) {}
    };

    class HttpParser {
    public:
        static void set_body(HttpRequest& req, std::string_view body) {
            req.body.assign(body);
        }

        static HttpRequest parse_headers(std::string_view raw, const HttpLimits& limits = {}) {
            HttpRequest req;

            if (raw.size() > limits.max_header_bytes) throw HttpParseError(431);
            if (!raw.ends_with("\r\n\r\n")) throw HttpParseError(400);
            
            auto line_end = raw.find("\r\n");
            if (line_end == std::string_view::npos) throw HttpParseError(400);

            parseRequestLine(raw.substr(0, line_end), req);
            
            auto head = raw.substr(line_end + 2);
            while (head != "\r\n") {
                const auto end = head.find("\r\n");
                if (end == std::string_view::npos) throw HttpParseError(400);
                parseHeaderLine(head.substr(0, end), req);
                head.remove_prefix(end + 2);
            }

            if (req.http_version == "HTTP/1.1" && req.header("host").empty()) {
                throw HttpParseError(400);
            }
                
            const auto host = req.header("host");
            if (!host.empty()) {
                for (unsigned char c : host) {
                    if (c <= 32 || c >= 127 || std::string_view("/\\@,?#").find(c) != std::string_view::npos) {
                        throw HttpParseError(400);
                    }
                }
            }

            if (!req.header("transfer-encoding").empty()) {
                if (!req.header("content-length").empty()) throw HttpParseError(400);
                throw HttpParseError(501); 
            }

            req.content_length = 0;
            const auto cl_val = req.header("content-length");
            if (!cl_val.empty()) {
                for (char c : cl_val) {
                    if (c < '0' || c > '9') throw HttpParseError(400);
                }
                const auto [end, ec] = std::from_chars(cl_val.data(), cl_val.data() + cl_val.size(), req.content_length);
                if (ec != std::errc{} || end != cl_val.data() + cl_val.size()) throw HttpParseError(400);
                if (req.content_length > limits.max_body_bytes) throw HttpParseError(413);
            }

            if (!req.header("expect").empty()) throw HttpParseError(417);
            
            req.keep_alive = (req.http_version == "HTTP/1.1");
            const auto connection = req.header("connection");
            if (contains_token(connection, "keep-alive")) req.keep_alive = true;
            if (contains_token(connection, "close")) req.keep_alive = false;

            auto ct = req.header("content-type");
            if (!ct.empty()) req.content_type = parseContentType(ct);

            auto ck = req.header("cookie");
            if (!ck.empty()) parseCookies(ck, req.cookies);

            return req;
        }

        static HttpRequest parse(std::string_view raw) {
            auto header_end = raw.find("\r\n\r\n");
            if (header_end == std::string_view::npos) throw HttpParseError(400);

            HttpRequest req = parse_headers(raw.substr(0, header_end + 4));

            if (raw.size() - header_end - 4 != req.content_length) throw HttpParseError(400);
            set_body(req, raw.substr(header_end + 4));

            return req;
        }

        static bool iequals(std::string_view a, std::string_view b) noexcept {
            return CaseInsensitiveEqual{}(a, b);
        }

        static bool token(std::string_view text) noexcept {
            if (text.empty()) return false;
            for (unsigned char c : text) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) {
                    return false;
                }
            }
            return true;
        }

        static bool contains_token(std::string_view values, std::string_view expected) noexcept {
            while (!values.empty()) {
                const auto comma = values.find(',');
                auto part = trim(values.substr(0, comma));
                if (iequals(part, expected)) return true;
                if (comma == std::string_view::npos) break;
                values.remove_prefix(comma + 1);
            }
            return false;
        }

    private:
        static int hex(unsigned char c) noexcept {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        static void parseRequestLine(std::string_view line, HttpRequest& req) {
            const auto s1 = line.find(' ');
            if (s1 == std::string_view::npos) throw HttpParseError(400);
            
            auto method_part = line.substr(0, s1);
            if (!token(method_part)) throw HttpParseError(400);
            req.method = toMethod(method_part);
            line.remove_prefix(s1 + 1);
            
            const auto s2 = line.find(' ');
            if (s2 == std::string_view::npos) throw HttpParseError(400);
            const auto full_path = line.substr(0, s2);
            req.http_version = line.substr(s2 + 1);
            
            if (req.http_version != "HTTP/1.1" && req.http_version != "HTTP/1.0") throw HttpParseError(505);
            if (full_path.empty() || (full_path.front() != '/' && !(full_path == "*" && req.method == HttpMethod::OPTIONS))) {
                throw HttpParseError(400);
            }
            
            for (std::size_t i = 0; i < full_path.size(); ++i) {
                unsigned char c = full_path[i];
                if (c <= 32 || c >= 127 || c == '#') throw HttpParseError(400);
                if (c == '%' && (i + 2 >= full_path.size() || hex(full_path[i+1]) < 0 || hex(full_path[i+2]) < 0)) {
                    throw HttpParseError(400);
                }
            }
            
            auto qpos = full_path.find('?');
            if (qpos != std::string_view::npos) {
                req.path = full_path.substr(0, qpos);
                parseQueryString(full_path.substr(qpos + 1), req.query);
            } else {
                req.path = full_path;
            }
        }

        static void parseHeaderLine(std::string_view line, HttpRequest& req) {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) throw HttpParseError(400);
            
            std::string_view key = line.substr(0, colon);
            if (!token(key)) throw HttpParseError(400);

            std::string_view value = trim(line.substr(colon + 1));
            for (unsigned char c : value) {
                if ((c < 32 && c != '\t') || c == 127) throw HttpParseError(400);
            }
            
            auto [it, inserted] = req.headers.try_emplace(key, value);
            if (!inserted) {
                if (iequals(key, "content-length") || iequals(key, "host") || iequals(key, "transfer-encoding")) {
                    throw HttpParseError(400);
                }
            }
        }

        static void parseQueryString(std::string_view qs, StringMap& query) {
            while (!qs.empty()) {
                auto amp = qs.find('&');
                std::string_view pair = (amp == std::string_view::npos) ? qs : qs.substr(0, amp);
                auto eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    query[pair.substr(0, eq)] = pair.substr(eq + 1);
                }
                if (amp == std::string_view::npos) break;
                qs.remove_prefix(amp + 1);
            }
        }

        static void parseCookies(std::string_view raw, StringMap& cookies) {
            while (!raw.empty()) {
                auto semi = raw.find(';');
                std::string_view pair = (semi == std::string_view::npos) ? raw : raw.substr(0, semi);
                auto eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    cookies[trim(pair.substr(0, eq))] = trim(pair.substr(eq + 1));
                }
                if (semi == std::string_view::npos) break;
                raw.remove_prefix(semi + 1);
            }
        }

        static std::string_view trim(std::string_view s) noexcept {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) 
                s.remove_prefix(1);
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) 
                s.remove_suffix(1);
            return s;
        }
        
        static HttpMethod toMethod(std::string_view m) noexcept {
            if (m == "GET")     return HttpMethod::GET;
            if (m == "POST")    return HttpMethod::POST;
            if (m == "PUT")     return HttpMethod::PUT;
            if (m == "PATCH")   return HttpMethod::PATCH;
            if (m == "DELETE")  return HttpMethod::DEL;
            if (m == "OPTIONS") return HttpMethod::OPTIONS;
            if (m == "HEAD")    return HttpMethod::HEAD;
            return HttpMethod::UNKNOWN;
        }
    };
}