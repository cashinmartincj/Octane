/**
 * @file Router.h
 * @brief Two-tier HTTP request router for Octane
 *
 * Routes are registered at startup and resolved on every request.
 * Uses two lookup strategies depending on route type:
 *
 *  Tier 1 — static hash map  O(1)
 *    Exact paths: /users  /health  /api/status
 *    Covers ~95% of typical traffic
 *
 *  Tier 2 — trie             O(k) where k = number of path segments
 *    Parameterised paths: /users/:id  /posts/:id/comments/:commentId
 *    Captures param values into req.params
 *
 * Built once at startup on a single thread.
 * Fully thread safe at runtime — immutable after listen().
 * No locks needed.
 *
 * Usage:
 *   router.get("/users",     handler);   // static  → hash map
 *   router.get("/users/:id", handler);   // dynamic → trie
 */

#pragma once
#include "Trie.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include <unordered_map>
#include <string>

namespace octane
{
    class Router {
        public:

            // ── Route Registration ───────────────────────

            void add(const std::string& method,
                    const std::string& path,
                    Handler            handler) {
                std::string key = makeKey(method, path);
                if (isDynamic(path))
                    dynamic_.insert(key, std::move(handler));
                else
                    static_[key] = std::move(handler);
            }

            void get    (const std::string& path, Handler h) { add("GET",     path, std::move(h)); }
            void post   (const std::string& path, Handler h) { add("POST",    path, std::move(h)); }
            void put    (const std::string& path, Handler h) { add("PUT",     path, std::move(h)); }
            void patch  (const std::string& path, Handler h) { add("PATCH",   path, std::move(h)); }
            void del    (const std::string& path, Handler h) { add("DELETE",  path, std::move(h)); }
            void options(const std::string& path, Handler h) { add("OPTIONS", path, std::move(h)); }
            void head   (const std::string& path, Handler h) { add("HEAD",    path, std::move(h)); }

            // ── Route Resolution ─────────────────────────

            bool resolve(HttpRequest& req, HttpResponse& res) {
                std::string key = makeKey(methodToString(req.method), req.path);

                // 1. static lookup — O(1)
                auto it = static_.find(key);
                if (it != static_.end()) {
                    it->second(req, res);
                    return true;
                }

                // 2. dynamic lookup — O(k)
                Handler matched;
                if (dynamic_.search(key, matched, req.params)) {
                    matched(req, res);
                    return true;
                }
                return false;
            }

        private:
            std::unordered_map<std::string, Handler> static_;  // O(1)  exact paths
            Trie                                     dynamic_; // O(k)  parameterised paths
            static std::string makeKey(const std::string& method,
                                        const std::string& path) {
                return method + ":" + path;
            }

            static bool isDynamic(const std::string& path) {
                return path.find(':') != std::string::npos;
            }

            static std::string methodToString(HttpMethod m) {
                switch (m) {
                    case HttpMethod::GET:     return "GET";
                    case HttpMethod::POST:    return "POST";
                    case HttpMethod::PUT:     return "PUT";
                    case HttpMethod::PATCH:   return "PATCH";
                    case HttpMethod::DEL:     return "DELETE";
                    case HttpMethod::OPTIONS: return "OPTIONS";
                    case HttpMethod::HEAD:    return "HEAD";
                    default:                  return "UNKNOWN";
                }
            }
    };
}
