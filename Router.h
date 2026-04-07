#pragma once
#include "Trie.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include <unordered_map>
#include <string>

// ─────────────────────────────────────────────
//  Router
//
//  Two-tier lookup strategy:
//  ┌─────────────────────────────────────────┐
//  │  Tier 1 — static_  (hash map)           │
//  │  GET:/users  POST:/users  GET:/health   │
//  │  O(1) lookup — covers ~95% of traffic   │
//  ├─────────────────────────────────────────┤
//  │  Tier 2 — dynamic_ (trie)               │
//  │  GET:/users/:id  GET:/posts/:id/comments│
//  │  O(segments) lookup — params captured   │
//  └─────────────────────────────────────────┘
//
//  Built once at startup — fully thread-safe at runtime
//  No locks needed — immutable after listen()
// ─────────────────────────────────────────────
class Router {
public:

    // ── Route Registration ───────────────────────
    // Called at startup — not thread-safe (single thread builds routes)
    void add(const std::string& method,
             const std::string& path,
             Handler            handler) {

        std::string key = makeKey(method, path);

        if (isDynamic(path))
            dynamic_.insert(key, std::move(handler));   // → trie
        else
            static_[key] = std::move(handler);          // → hash map
    }

    // Convenience methods — clean user-facing API with fluent chaining
    void get   (const std::string& path, Handler h) { add("GET",     path, std::move(h)); }
    void post  (const std::string& path, Handler h) { add("POST",    path, std::move(h)); }
    void put   (const std::string& path, Handler h) { add("PUT",     path, std::move(h)); }
    void patch (const std::string& path, Handler h) { add("PATCH",   path, std::move(h)); }
    void del   (const std::string& path, Handler h) { add("DELETE",  path, std::move(h)); }
    void options(const std::string& path, Handler h){ add("OPTIONS", path, std::move(h)); }
    void head  (const std::string& path, Handler h) { add("HEAD",    path, std::move(h)); }

    // ── Route Resolution ─────────────────────────
    // Called on every request — must be fast
    // Returns false if no route matched (caller sends 404)
    bool resolve(HttpRequest& req, HttpResponse& res) {
        std::string key = makeKey(methodToString(req.method), req.path);

        // 1. Static lookup — O(1)
        auto it = static_.find(key);
        if (it != static_.end()) {
            it->second(req, res);
            return true;
        }

        // 2. Dynamic lookup — O(segments)
        Handler matched;
        if (dynamic_.search(key, matched, req.params)) {
            matched(req, res);
            return true;
        }

        // 3. No match — caller handles 404
        return false;
    }

private:
    std::unordered_map<std::string, Handler> static_;   // O(1)  for static routes
    Trie                                     dynamic_;  // O(n)  for :param routes

    // "GET" + ":" + "/users" → "GET:/users"
    static std::string makeKey(const std::string& method,
                                const std::string& path) {
        return method + ":" + path;
    }

    // A route is dynamic if it contains ':' anywhere in the path
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