/**
 * @file HttpParser.h
 * @brief Zero-copy, bounded HTTP/1.1 request parser.
 *
 * @details
 * `HttpParser` provides high-throughput, allocation-minimized parsing for HTTP/1.1 requests.
 * It parses the request line (method, target URI, HTTP version), headers (case-insensitive keys),
 * URL-encoded query parameters, cookies, and body framing (Content-Length).
 *
 * Key Design Principles:
 * - **Zero Allocations in Hot Path:** All string slices (path, header keys, header values, cookies)
 *   are stored as `std::string_view` referencing the underlying connection read buffer directly.
 * - **RFC 9112 Strict Compliance:**
 *   - Verifies RFC 9110 valid token characters for HTTP methods and header names.
 *   - Enforces valid ASCII/tab values without control characters.
 *   - Validates RFC 9112 URI framing (rejects whitespace, `#` fragments, invalid `%` encodings).
 *   - Enforces required `Host` header for HTTP/1.1 requests.
 *   - Rejects conflicting or duplicate `Content-Length`, `Host`, or `Transfer-Encoding` headers (RFC 9112 §6.1).
 *   - Rejects `Transfer-Encoding: chunked` with 501 Not Implemented (HTTP/1.1 pipeline optimization).
 *   - Enforces configurable boundaries via `HttpLimits` (`max_header_bytes`, `max_body_bytes`).
 *
 * Where this is imported / used:
 * - Direct Include: `#include "HttpParser.h"`
 * - Transport Engine: Called in `octane::transport::TcpConnection::handle_read()` to parse inbound
 *   socket buffers received from the Linux `io_uring` completion ring.
 * - Test Suites: Used extensively in test files (`tests/test_parser.cpp`, `tests/http_limits.cpp`)
 *   to verify protocol conformance and edge-case handling.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
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
    /**
     * @struct HttpParseError
     * @brief Exception thrown when an inbound HTTP request violates protocol syntax or size limits.
     */
    struct HttpParseError : std::runtime_error {
        int status; ///< Suggested HTTP response status code (e.g., 400 Bad Request, 413, 431, 501, 505)

        /**
         * @brief Constructs an HttpParseError with a specific HTTP status code.
         * @param code Numeric HTTP error status code.
         */
        explicit HttpParseError(int code) : std::runtime_error("Invalid HTTP request"), status(code) {}
    };

    /**
     * @class HttpParser
     * @brief Static utility class providing zero-copy HTTP/1.1 parsing methods.
     */
    class HttpParser {
    public:
        /**
         * @brief Assigns the request payload body view to the parsed HttpRequest object.
         * @param req Target HttpRequest to receive the body.
         * @param body Non-owning string_view referencing the payload slice in the connection buffer.
         */
        static void set_body(HttpRequest& req, std::string_view body) {
            req.body.assign(body);
        }

        /**
         * @brief Parses the HTTP request line and headers from a raw buffer slice up to `\r\n\r\n`.
         * @param raw Sliced buffer containing headers ending with `\r\n\r\n`.
         * @param limits Boundary configuration constraints (header size, body size).
         * @return Populated HttpRequest struct with non-owning views pointing into `raw`.
         * @throws HttpParseError If the request violates RFC syntax, size boundaries, or required headers.
         */
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

            const auto content_length_it =
                req.headers.find(std::string_view{"content-length"});
            const bool has_content_length =
                content_length_it != req.headers.end();

            if (!req.header("transfer-encoding").empty()) {
                if (has_content_length) throw HttpParseError(400);
                throw HttpParseError(501); 
            }

            req.content_length = 0;
            if (has_content_length) {
                const auto cl_val = content_length_it->second;
                if (cl_val.empty()) throw HttpParseError(400);
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

        /**
         * @brief Parses a complete raw HTTP request (headers + body) in one pass.
         * @param raw Full raw request string view including body bytes.
         * @return Fully parsed HttpRequest instance.
         * @throws HttpParseError If headers or body lengths fail validation.
         */
        static HttpRequest parse(std::string_view raw) {
            auto header_end = raw.find("\r\n\r\n");
            if (header_end == std::string_view::npos) throw HttpParseError(400);

            HttpRequest req = parse_headers(raw.substr(0, header_end + 4));

            if (raw.size() - header_end - 4 != req.content_length) throw HttpParseError(400);
            set_body(req, raw.substr(header_end + 4));

            return req;
        }

        /**
         * @brief Performs case-insensitive ASCII string comparison.
         * @param a First string view.
         * @param b Second string view.
         * @return True if strings match case-insensitively.
         */
        static bool iequals(std::string_view a, std::string_view b) noexcept {
            return CaseInsensitiveEqual{}(a, b);
        }

        /**
         * @brief Checks if a string view conforms to RFC 9110 token character requirements.
         * @param text String view to inspect.
         * @return True if non-empty and containing only valid token characters.
         */
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

        /**
         * @brief Searches a comma-delimited header value for a case-insensitive token.
         * @param values Comma-separated header string (e.g. "keep-alive, upgrade").
         * @param expected Target token to locate (e.g. "keep-alive").
         * @return True if token exists.
         */
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

        /**
         * @brief Maps standard HTTP method string representations to HttpMethod enum.
         * @param m Method token string view (e.g., "GET", "POST").
         * @return Corresponding HttpMethod variant, or HttpMethod::UNKNOWN.
         */
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

    private:
        /**
         * @brief Converts a hexadecimal ASCII character to its 4-bit integer equivalent.
         * @param c ASCII character.
         * @return Integer [0..15] or -1 if invalid.
         */
        static int hex(unsigned char c) noexcept {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        /**
         * @brief Parses and validates the HTTP request line (`METHOD /path HTTP/1.1`).
         * @param line Single request line string view without trailing CRLF.
         * @param req Output HttpRequest struct.
         * @throws HttpParseError If method, path, or HTTP version is malformed.
         */
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
                parseQueryString(full_path.substr(qpos + 1), req);
            } else {
                req.path = full_path;
            }
        }

        /**
         * @brief Parses a single HTTP header line (`Key: Value`) into the request header table.
         * @param line Single header line without trailing CRLF.
         * @param req Output HttpRequest struct.
         * @throws HttpParseError If key or value contains forbidden characters or illegal duplicates.
         */
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

        /**
         * @brief Decodes a percent-encoded query parameter component (e.g., `%20` -> ` `).
         * @param encoded Raw percent-encoded string view.
         * @return Decoded std::string.
         * @throws HttpParseError If an invalid `%` sequence is encountered.
         */
        static std::string decodeQueryComponent(std::string_view encoded) {
            std::string decoded;
            decoded.reserve(encoded.size());
            for (std::size_t i = 0; i < encoded.size(); ++i) {
                if (encoded[i] != '%') {
                    decoded.push_back(encoded[i]);
                    continue;
                }
                if (i + 2 >= encoded.size()) throw HttpParseError(400);
                const int high = hex(static_cast<unsigned char>(encoded[i + 1]));
                const int low = hex(static_cast<unsigned char>(encoded[i + 2]));
                if (high < 0 || low < 0) throw HttpParseError(400);
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            }
            return decoded;
        }

        /**
         * @brief Parses and decodes query parameters from a raw query string (`k1=v1&k2=v2`).
         * @param qs Query string slice following `?`.
         * @param query Destination OwnedStringMap.
         */
        static void parseQueryString(std::string_view qs, HttpRequest& request) {
            while (!qs.empty()) {
                auto amp = qs.find('&');
                std::string_view pair = (amp == std::string_view::npos) ? qs : qs.substr(0, amp);
                auto eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    auto key=decodeQueryComponent(pair.substr(0,eq));
                    auto value=decodeQueryComponent(pair.substr(eq+1));
                    request.query_lists[key].push_back(value);
                    request.query[std::move(key)]=std::move(value);
                }
                if (amp == std::string_view::npos) break;
                qs.remove_prefix(amp + 1);
            }
        }

        /**
         * @brief Parses semicolon-separated cookie values into the cookies StringMap.
         * @param raw Raw Cookie header value.
         * @param cookies Destination StringMap.
         */
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

        /**
         * @brief Trims leading and trailing spaces and horizontal tabs from a string view.
         * @param s Input string view.
         * @return Trimmed string view without memory allocation.
         */
        static std::string_view trim(std::string_view s) noexcept {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) 
                s.remove_prefix(1);
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) 
                s.remove_suffix(1);
            return s;
        }
    };
}
