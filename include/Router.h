/**
 * @file Router.h
 * @brief Memory-safe, zero-allocation two-tier HTTP router for Octane.
 *
 * @details
 * `Router` provides high-speed route registration and matching for HTTP/1.1 requests.
 * It employs a hybrid two-tier routing architecture:
 *
 * 1. **Tier 1 (Static Hash Map - O(1)):**
 *    Routes without URL wildcards (e.g. `/api/v1/health`) are stored in per-method
 *    `std::unordered_map` tables utilizing transparent string view hashing (`StringViewHash`).
 *    Path strings are preserved in an internal `std::deque<std::string>` to ensure stable pointer
 *    references across additions without relocation. Matching executes with zero dynamic heap allocation.
 *
 * 2. **Tier 2 (Dynamic Radix Trie - O(k)):**
 *    Routes with parameter wildcards (e.g. `/api/v1/users/:id`) are registered in per-method
 *    `Trie` instances (`dynamic_`). Wildcard parameter bindings are placed into `req.params` as
 *    non-owning `std::string_view` slices directly pointing into `req.path`.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "Router.h"`
 * - Application Core: Instantiated as `Router router_` in `octane::init` (`include/Octane.h`).
 * - Dispatch Engine: Referenced by `octane::core::RequestDispatcher` (`include/core/RequestDispatcher.h`)
 *   to match incoming HTTP requests against registered endpoints.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once
#include "Trie.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include "websocket/WebSocket.h"
#include <unordered_map>
#include <string_view>
#include <string>
#include <deque>
#include <array>
#include <utility>

namespace octane
{
    /**
     * @class Router
     * @brief High-performance two-tier HTTP router supporting static and parameterized routes.
     */
    class Router {
    public:
        /**
         * @brief Registers a route with an HTTP method, path, callback handler, and execution policy.
         * @param method HTTP method string (e.g. "GET", "POST").
         * @param path URL path pattern (e.g. "/users" or "/users/:id").
         * @param handler Callable callback `std::function<void(HttpRequest&, HttpResponse&)>`.
         * @param execution Execution scheduling policy (`inline_direct`, `shared_blocking`, or `named`).
         */
        void add(std::string_view method,
                 std::string_view path,
                 Handler handler,
                 HandlerExecution execution = {}) {
            HttpMethod m = stringToMethod(method);
            if (m == HttpMethod::UNKNOWN) return;

            size_t idx = std::to_underlying(m);

            if (isDynamic(path)) {
                dynamic_[idx].insert(
                    std::string(path), {handler, std::move(execution)});
            } else {
                // std::deque ensures stable references upon push_back (no reallocation pointer invalidation)
                path_storage_.emplace_back(path);
                std::string_view persistent_view = path_storage_.back();
                static_routes_[idx][persistent_view] =
                    {handler, std::move(execution)};
            }
        }

        /** @name Direct Handler Registrations (Default Execution Policy) */
        ///@{
        void get    (std::string_view path, Handler h) { add("GET",     path, std::move(h)); }
        void post   (std::string_view path, Handler h) { add("POST",    path, std::move(h)); }
        void put    (std::string_view path, Handler h) { add("PUT",     path, std::move(h)); }
        void patch  (std::string_view path, Handler h) { add("PATCH",   path, std::move(h)); }
        void del    (std::string_view path, Handler h) { add("DELETE",  path, std::move(h)); }
        void options(std::string_view path, Handler h) { add("OPTIONS", path, std::move(h)); }
        void head   (std::string_view path, Handler h) { add("HEAD",    path, std::move(h)); }
        ///@}

        /** @name Custom Execution Policy Handler Registrations (Worker Offloading) */
        ///@{
        void get    (std::string_view path, Handler h, HandlerExecution e) { add("GET",     path, h, std::move(e)); }
        void post   (std::string_view path, Handler h, HandlerExecution e) { add("POST",    path, h, std::move(e)); }
        void put    (std::string_view path, Handler h, HandlerExecution e) { add("PUT",     path, h, std::move(e)); }
        void patch  (std::string_view path, Handler h, HandlerExecution e) { add("PATCH",   path, h, std::move(e)); }
        void del    (std::string_view path, Handler h, HandlerExecution e) { add("DELETE",  path, h, std::move(e)); }
        void options(std::string_view path, Handler h, HandlerExecution e) { add("OPTIONS", path, h, std::move(e)); }
        void head   (std::string_view path, Handler h, HandlerExecution e) { add("HEAD",    path, h, std::move(e)); }
        ///@}

        /** @name WebSocket Route Registrations */
        ///@{
        void ws(std::string_view path, WebSocketConfig config) {
            path_storage_.emplace_back(path);
            std::string_view persistent_view = path_storage_.back();
            ws_routes_[persistent_view] = std::move(config);
        }
        ///@}

        /**
         * @brief Checks if a given path is registered as a WebSocket route.
         */
        [[nodiscard]] const WebSocketConfig* find_ws(std::string_view path) const noexcept {
            auto it = ws_routes_.find(path);
            if (it != ws_routes_.end()) return &it->second;

            // Trailing slash tolerance
            if (path.size() > 1) {
                if (path.ends_with('/')) {
                    std::string_view trimmed(path.data(), path.size() - 1);
                    auto it_trimmed = ws_routes_.find(trimmed);
                    if (it_trimmed != ws_routes_.end()) return &it_trimmed->second;
                } else {
                    std::string slashed = std::string(path) + "/";
                    auto it_slashed = ws_routes_.find(slashed);
                    if (it_slashed != ws_routes_.end()) return &it_slashed->second;
                }
            }
            return nullptr;
        }

        /**
         * @brief Matches an incoming HttpRequest against registered static and dynamic routes.
         * @param[in,out] req Inbound HttpRequest (path read, params populated if dynamic).
         * @param[out] route Pointer to the matching HandlerRoute entry.
         * @return True if a route was successfully matched; false otherwise (404 Not Found candidate).
         */
        [[nodiscard]] bool match(HttpRequest& req,
                                 const HandlerRoute*& route) const noexcept {
            size_t method_idx = std::to_underlying(req.method);
            if (method_idx >= 7) return false;

            const auto& method_map = static_routes_[method_idx];
            auto it = method_map.find(req.path);
            if (it != method_map.end()) {
                route = &it->second;
                return true;
            }

            // Trailing slash tolerance: if "/path/", try "/path"; if "/path", try "/path/"
            if (req.path.size() > 1) {
                if (req.path.ends_with('/')) {
                    std::string_view trimmed(req.path.data(), req.path.size() - 1);
                    auto it_trimmed = method_map.find(trimmed);
                    if (it_trimmed != method_map.end()) {
                        route = &it_trimmed->second;
                        return true;
                    }
                } else {
                    std::string slashed = std::string(req.path) + "/";
                    auto it_slashed = method_map.find(slashed);
                    if (it_slashed != method_map.end()) {
                        route = &it_slashed->second;
                        return true;
                    }
                }
            }

            return dynamic_[method_idx].search(req.path, route, req.params);
        }

        /**
         * @brief Synchronously resolves and executes a route for an inbound request.
         * @param req Inbound HttpRequest.
         * @param res Outbound HttpResponse to populate.
         * @return True if a route matched and ran; false if no route matched.
         */
        bool resolve(HttpRequest& req, HttpResponse& res) {
            const HandlerRoute* route = nullptr;
            if (!match(req, route)) return false;
            route->handler(req, res);
            return true;
        }

    private:
        /// Static route map indexed by path string_view using transparent hashing
        using MethodStaticMap = std::unordered_map<std::string_view, HandlerRoute, StringViewHash, std::equal_to<>>;
        
        /// Array of static route maps indexed by numeric HttpMethod enum value
        std::array<MethodStaticMap, 8> static_routes_;

        /// Array of dynamic Radix Tries indexed by numeric HttpMethod enum value
        std::array<Trie, 8>            dynamic_;

        /// Stable string backing storage for static route path views
        std::deque<std::string>        path_storage_;

        /// WebSocket routes mapped by path string_view
        std::unordered_map<std::string_view, WebSocketConfig, StringViewHash, std::equal_to<>> ws_routes_;

        /**
         * @brief Inspects a path string to detect parameter wildcards (`:`).
         * @param path URL path view.
         * @return True if path contains dynamic wildcards; false if static.
         */
        static bool isDynamic(std::string_view path) noexcept {
            return path.find(':') != std::string_view::npos;
        }

        /**
         * @brief Maps method string view to HttpMethod enum variant.
         * @param m Method string view.
         * @return Corresponding HttpMethod variant.
         */
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
