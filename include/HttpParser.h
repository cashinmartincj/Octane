/**
 * @file HttpParser.h
 * @brief Parses raw HTTP/1.1 request bytes into an HttpRequest struct
 *
 * Designed for zero allocation where possible — uses string_view to
 * view into the original buffer rather than copying substrings.
 *
 * Handles:
 *   Request line  — method, path, query string, HTTP version
 *   Headers       — key/value pairs, case preserved
 *   Body          — raw bytes after \r\n\r\n
 *   Cookies       — parsed from Cookie header into req.cookies
 *   Content-Type  — parsed into req.content_type enum
 *   Query string  — parsed into req.query map, URL decoded
 */

#pragma once
#include "HttpRequest.h"
#include "HttpTypes.h"
#include <string>
#include <unordered_map>

class HttpParser {
public:

    /**
     * @brief Parse raw HTTP/1.1 request bytes into HttpRequest
     *
     * Splits on \r\n\r\n to separate headers from body.
     * Uses string_view throughout to avoid copies where possible.
     * Body is the only field that requires an owning copy.
     *
     * @param raw  Complete raw HTTP request bytes
     * @return     Populated HttpRequest, default-constructed on parse failure
     */
    static HttpRequest parse(const std::string& raw) {
        HttpRequest req;
        std::string_view view(raw);

        // 1. split head and body
        auto header_end = view.find("\r\n\r\n");
        if (header_end == std::string_view::npos) return req;

        std::string_view head = view.substr(0, header_end);
        req.body = std::string(view.substr(header_end + 4)); // body needs owning copy

        // 2. parse request line
        auto line_end = head.find("\r\n");
        parseRequestLine(head.substr(0, line_end), req);
        head = head.substr(line_end + 2);  // advance past \r\n

        // 3. parse headers
        while (!head.empty()) {
            auto end = head.find("\r\n");
            std::string_view line = end == std::string_view::npos
                                  ? head
                                  : head.substr(0, end);
            parseHeaderLine(line, req);
            if (end == std::string_view::npos) break;
            head = head.substr(end + 2);   // advance past \r\n
        }

        // 4. content type + cookies
        auto ct = req.header("content-type");
        if (!ct.empty()) req.content_type = parseContentType(ct);

        auto ck = req.header("cookie");
        if (!ck.empty()) parseCookies(ck, req.cookies);

        return req;
    }

private:
    static void parseRequestLine(std::string_view line, HttpRequest& req) {
        // method
        auto s1 = line.find(' ');
        req.method = toMethod(std::string(line.substr(0, s1)));
        line = line.substr(s1 + 1);

        // path + query
        auto s2 = line.find(' ');
        std::string_view full_path = line.substr(0, s2);
        req.http_version = std::string(trim(line.substr(s2 + 1)));

        auto qpos = full_path.find('?');
        if (qpos != std::string_view::npos) {
            req.path = std::string(full_path.substr(0, qpos));
            parseQueryString(full_path.substr(qpos + 1), req.query);
        } else {
            req.path = std::string(full_path);
        }
    }

    static void parseHeaderLine(std::string_view line, HttpRequest& req) {
        auto colon = line.find(':');
        if (colon == std::string_view::npos) return;
        std::string key   = std::string(trim(line.substr(0, colon)));
        std::string value = std::string(trim(line.substr(colon + 1)));
        req.headers[key]  = std::move(value);
    }

    /**
     * @brief Parse URL query string into key/value map
     *
     * Format: "key1=val1&key2=val2"
     * Keys and values are URL decoded — %20 → ' ', + → ' '
     *
     * @param qs     Query string without leading '?'
     * @param query  Map to populate
     */
    static void parseQueryString(std::string_view qs,
                                  std::unordered_map<std::string, std::string>& query) {
        while (!qs.empty()) {
            auto amp = qs.find('&');
            std::string_view pair = amp == std::string_view::npos
                                  ? qs : qs.substr(0, amp);

            auto eq = pair.find('=');
            if (eq != std::string_view::npos)
                query[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));

            if (amp == std::string_view::npos) break;
            qs = qs.substr(amp + 1);
        }
    }

    static void parseCookies(const std::string& raw,
                              std::unordered_map<std::string, std::string>& cookies) {
        std::string_view view(raw);
        while (!view.empty()) {
            auto semi = view.find(';');
            std::string_view pair = semi == std::string_view::npos
                                  ? view : view.substr(0, semi);

            auto eq = pair.find('=');
            if (eq != std::string_view::npos)
                cookies[std::string(trim(pair.substr(0, eq)))] =
                        std::string(trim(pair.substr(eq + 1)));

            if (semi == std::string_view::npos) break;
            view = view.substr(semi + 1);
        }
    }

    static std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r'))
            s.remove_suffix(1);
        return s;
    }

    static std::string toLower(std::string s) {
        for (char& c : s) c = std::tolower(c);
        return s;
    }

    static std::string urlDecode(std::string_view s) {
        std::string result;
        result.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                int hex = std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16);
                result += static_cast<char>(hex);
                i += 2;
            } else if (s[i] == '+') {
                result += ' ';
            } else {
                result += s[i];
            }
        }
        return result;
    }

    static HttpMethod toMethod(const std::string& m) {
        static const std::unordered_map<std::string, HttpMethod> map = {
            { "GET",     HttpMethod::GET },
            { "POST",    HttpMethod::POST },
            { "PUT",     HttpMethod::PUT },
            { "PATCH",   HttpMethod::PATCH },
            { "DELETE",  HttpMethod::DEL },
            { "OPTIONS", HttpMethod::OPTIONS },
            { "HEAD",    HttpMethod::HEAD },
        };
        auto it = map.find(m);
        return it != map.end() ? it->second : HttpMethod::UNKNOWN;
    }
};