/**
 * @file Router.h
 * @brief Memory-safe, zero-allocation two-tier HTTP router for Octane.
 */

#pragma once
#include "Trie.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include <unordered_map>
#include <string_view>
#include <string>
#include <deque>
#include <array>

namespace octane
{
    class Router {
    public:
        void add(std::string_view method,
                 std::string_view path,
                 Handler handler) {
            HttpMethod m = stringToMethod(method);
            if (m == HttpMethod::UNKNOWN) return;

            size_t idx = static_cast<size_t>(m);

            if (isDynamic(path)) {
                dynamic_[idx].insert(std::string(path), std::move(handler));
            } else {
                // std::deque ensures stable references upon push_back (no reallocation pointer invalidation)
                path_storage_.emplace_back(path);
                std::string_view persistent_view = path_storage_.back();
                static_routes_[idx][persistent_view] = std::move(handler);
            }
        }

        void get    (std::string_view path, Handler h) { add("GET",     path, std::move(h)); }
        void post   (std::string_view path, Handler h) { add("POST",    path, std::move(h)); }
        void put    (std::string_view path, Handler h) { add("PUT",     path, std::move(h)); }
        void patch  (std::string_view path, Handler h) { add("PATCH",   path, std::move(h)); }
        void del    (std::string_view path, Handler h) { add("DELETE",  path, std::move(h)); }
        void options(std::string_view path, Handler h) { add("OPTIONS", path, std::move(h)); }
        void head   (std::string_view path, Handler h) { add("HEAD",    path, std::move(h)); }

        bool resolve(HttpRequest& req, HttpResponse& res) {
            size_t method_idx = static_cast<size_t>(req.method);
            if (method_idx >= 7) return false;

            // 1. Static O(1) direct match
            const auto& method_map = static_routes_[method_idx];
            auto it = method_map.find(req.path);
            if (it != method_map.end()) {
                it->second(req, res);
                return true;
            }

            // 2. Trie lookup for parameterized segments
            Handler matched = nullptr;
            if (dynamic_[method_idx].search(req.path, matched, req.params)) {
                matched(req, res);
                return true;
            }

            return false;
        }

    private:
        using MethodStaticMap = std::unordered_map<std::string_view, Handler, StringViewHash, std::equal_to<>>;
        
        std::array<MethodStaticMap, 8> static_routes_;
        std::array<Trie, 8>            dynamic_;
        std::deque<std::string>        path_storage_;

        static bool isDynamic(std::string_view path) noexcept {
            return path.find(':') != std::string_view::npos;
        }

        static HttpMethod stringToMethod(std::string_view m) noexcept {
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