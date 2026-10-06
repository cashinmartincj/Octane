#pragma once

/**
 * @file Octane.h
 * @brief Primary application initialization and routing facade for the Octane Web Framework.
 *
 * @details
 * This is the main header to include when building a web service with Octane. It exposes
 * the `octane::init` application class, which coordinates route registration (both compile-time
 * CRTP handlers and functional callbacks) and launches the underlying multi-threaded `io_uring`
 * TCP server.
 *
 * @section usage Usage Example:
 * @code
 * #include "Octane.h"
 * #include "RouteBase.h"
 *
 * // Define a compile-time CRTP handler:
 * class GetUser : public octane::routes::Get<GetUser> {
 * public:
 *     void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
 *         res.status(200).json(R"({"user":"Alice"})");
 *     }
 * };
 *
 * int main() {
 *     octane::init app;
 *
 *     // Register CRTP route:
 *     app.get<GetUser>("/api/user");
 *
 *     // Register functional route with offload to database queue:
 *     app.get("/api/health", [](const octane::HttpRequest&, octane::HttpResponse& res) {
 *         res.status(200).text("OK");
 *     });
 *
 *     // Start server on port 8080:
 *     app.listen(8080);
 * }
 * @endcode
 */

#include "Router.h"
#include "HttpParser.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include "HttpLimits.h"
#include "transport/TcpServer.h"

#include <algorithm>
#include <string_view>
#include <thread>
#include <utility>

namespace octane {

/**
 * @class init
 * @brief Application builder and server launcher for an Octane service.
 *
 * @details
 * An `init` instance owns the top-level `Router` where routes are configured. It provides
 * a fluent builder interface supporting all standard HTTP methods (GET, POST, PUT, PATCH,
 * DELETE, OPTIONS, HEAD) with either compile-time zero-overhead CRTP classes or callable
 * function/lambda handlers.
 */
class init {
    Router router_;

    template<typename RouteClass>
    static Handler resolve_route_handler() {
        if constexpr (requires { RouteClass::handler(); }) {
            return RouteClass::handler();
        } else {
            return [](HttpRequest& req, HttpResponse& res) {
                RouteClass instance;
                if constexpr (requires { instance.before(req, res); }) instance.before(req, res);
                instance.handle(req, res);
                if constexpr (requires { instance.after(req, res); }) instance.after(req, res);
            };
        }
    }

public:
    // ── Unified / C++23 Route Registration ───────────────────────────────

    /**
     * @brief Registers any route class automatically detecting its HTTP method.
     * @tparam RouteClass Route class defining static method() and handle(req, res).
     * @param path URL path pattern.
     * @return Reference to this init instance for method chaining.
     */
    template<typename RouteClass>
    init& route(std::string_view path) {
        router_.add(RouteClass::method(), path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers any route class with an explicit execution policy.
     * @tparam RouteClass Route class defining static method() and handle(req, res).
     * @param path URL path pattern.
     * @param e Execution policy.
     * @return Reference to this init instance for method chaining.
     */
    template<typename RouteClass>
    init& route(std::string_view path, HandlerExecution e) {
        router_.add(RouteClass::method(), path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    // ── Method-Specific Route Class Registrations ────────────────────────

    /**
     * @brief Registers an HTTP GET route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Get<RouteClass>` or C++23 `GetRoute`.
     * @param path The URL path pattern (e.g., `"/users"` or `"/users/:id"`).
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& get(std::string_view path) {
        router_.get(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers an HTTP POST route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Post<RouteClass>` or C++23 `PostRoute`.
     * @param path The URL path pattern.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& post(std::string_view path) {
        router_.post(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers an HTTP PUT route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Put<RouteClass>` or C++23 `PutRoute`.
     * @param path The URL path pattern.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& put(std::string_view path) {
        router_.put(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers an HTTP PATCH route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Patch<RouteClass>` or C++23 `PatchRoute`.
     * @param path The URL path pattern.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& patch(std::string_view path) {
        router_.patch(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers an HTTP DELETE route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Del<RouteClass>` or C++23 `DelRoute`.
     * @param path The URL path pattern.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& del(std::string_view path) {
        router_.del(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers an HTTP OPTIONS route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Options<RouteClass>` or C++23 `OptionsRoute`.
     * @param path The URL path pattern.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& options(std::string_view path) {
        router_.options(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    /**
     * @brief Registers an HTTP HEAD route using a route handler class (inline execution).
     * @tparam RouteClass Route class inheriting from `Head<RouteClass>` or C++23 `HeadRoute`.
     * @param path The URL path pattern.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& head(std::string_view path) {
        router_.head(path, resolve_route_handler<RouteClass>());
        return *this;
    }

    // ── CRTP Route Registration with Custom Execution Policy ─────────────

    /**
     * @brief Registers an HTTP GET route with an explicit execution policy (e.g., worker queue offload).
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy (`HandlerExecution::inline_execution()`, `shared_blocking()`, or `named("queue")`).
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& get(std::string_view path, HandlerExecution e) {
        router_.get(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP POST route with an explicit execution policy.
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& post(std::string_view path, HandlerExecution e) {
        router_.post(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP PUT route with an explicit execution policy.
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& put(std::string_view path, HandlerExecution e) {
        router_.put(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP PATCH route with an explicit execution policy.
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& patch(std::string_view path, HandlerExecution e) {
        router_.patch(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP DELETE route with an explicit execution policy.
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& del(std::string_view path, HandlerExecution e) {
        router_.del(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP OPTIONS route with an explicit execution policy.
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& options(std::string_view path, HandlerExecution e) {
        router_.options(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP HEAD route with an explicit execution policy.
     * @tparam RouteClass Derived route handler class.
     * @param path The URL path pattern.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename RouteClass>
    init& head(std::string_view path, HandlerExecution e) {
        router_.head(path, resolve_route_handler<RouteClass>(), std::move(e));
        return *this;
    }

    // ── Function / Lambda Route Registration ─────────────────────────────

    /**
     * @brief Registers an HTTP GET route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature: `void(const HttpRequest&, HttpResponse&)`.
     * @return Reference to this `init` instance for method chaining.
     */
    init& get(std::string_view path, Handler h) {
        router_.get(path, std::move(h));
        return *this;
    }

    /**
     * @brief Registers an HTTP POST route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature.
     * @return Reference to this `init` instance for method chaining.
     */
    init& post(std::string_view path, Handler h) {
        router_.post(path, std::move(h));
        return *this;
    }

    /**
     * @brief Registers an HTTP PUT route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature.
     * @return Reference to this `init` instance for method chaining.
     */
    init& put(std::string_view path, Handler h) {
        router_.put(path, std::move(h));
        return *this;
    }

    /**
     * @brief Registers an HTTP PATCH route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature.
     * @return Reference to this `init` instance for method chaining.
     */
    init& patch(std::string_view path, Handler h) {
        router_.patch(path, std::move(h));
        return *this;
    }

    /**
     * @brief Registers an HTTP DELETE route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature.
     * @return Reference to this `init` instance for method chaining.
     */
    init& del(std::string_view path, Handler h) {
        router_.del(path, std::move(h));
        return *this;
    }

    /**
     * @brief Registers an HTTP OPTIONS route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature.
     * @return Reference to this `init` instance for method chaining.
     */
    init& options(std::string_view path, Handler h) {
        router_.options(path, std::move(h));
        return *this;
    }

    /**
     * @brief Registers an HTTP HEAD route using a functional handler callback (inline execution).
     * @param path URL pattern.
     * @param h Callable handler signature.
     * @return Reference to this `init` instance for method chaining.
     */
    init& head(std::string_view path, Handler h) {
        router_.head(path, std::move(h));
        return *this;
    }

    // ── Function / Lambda Route Registration with Execution Policy ───────

    /**
     * @brief Registers an HTTP GET functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& get(std::string_view path, Handler h, HandlerExecution e) {
        router_.get(path, std::move(h), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP POST functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& post(std::string_view path, Handler h, HandlerExecution e) {
        router_.post(path, std::move(h), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP PUT functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& put(std::string_view path, Handler h, HandlerExecution e) {
        router_.put(path, std::move(h), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP PATCH functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& patch(std::string_view path, Handler h, HandlerExecution e) {
        router_.patch(path, std::move(h), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP DELETE functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& del(std::string_view path, Handler h, HandlerExecution e) {
        router_.del(path, std::move(h), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP OPTIONS functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& options(std::string_view path, Handler h, HandlerExecution e) {
        router_.options(path, std::move(h), std::move(e));
        return *this;
    }

    /**
     * @brief Registers an HTTP HEAD functional route with an explicit execution policy.
     * @param path URL pattern.
     * @param h Callable handler.
     * @param e Execution policy.
     * @return Reference to this `init` instance for method chaining.
     */
    init& head(std::string_view path, Handler h, HandlerExecution e) {
        router_.head(path, std::move(h), std::move(e));
        return *this;
    }

    // ── WebSocket Route Registrations ────────────────────────────────────

    /**
     * @brief Registers a WebSocket route with complete callback configuration.
     * @param path URL pattern.
     * @param config WebSocket configuration and event callbacks.
     * @return Reference to this `init` instance for method chaining.
     */
    init& ws(std::string_view path, WebSocketConfig config) {
        router_.ws(path, std::move(config));
        return *this;
    }

    /**
     * @brief Registers a WebSocket route using a fluent configuration builder lambda.
     * @param path URL pattern.
     * @param setup Lambda accepting `WebSocketConfig&`.
     * @return Reference to this `init` instance for method chaining.
     */
    template<typename SetupFn>
    requires std::invocable<SetupFn, WebSocketConfig&>
    init& ws(std::string_view path, SetupFn setup) {
        WebSocketConfig config;
        setup(config);
        router_.ws(path, std::move(config));
        return *this;
    }

    // ── Server Execution ─────────────────────────────────────────────────

    /**
     * @brief Starts the Octane multi-worker io_uring server on the specified TCP port.
     * @details
     * Initializes one independent `io_uring` ring and listening socket per worker thread
     * using Linux `SO_REUSEPORT`. Blocks until a shutdown signal (SIGINT, SIGTERM) is received.
     *
     * @param port The TCP port to listen on (e.g., 8080).
     * @param threads Number of worker threads (defaults to hardware concurrency).
     * @param limits HTTP connection limits and timeouts (max header bytes, body bytes, timeouts).
     * @param options Low-level TCP transport options (queue depth, worker pinning, execution queues).
     */
    void listen(
        int port,
        int threads = std::max(1u, std::thread::hardware_concurrency()),
        const HttpLimits& limits = {},
        const transport::TcpServerOptions& options = {})
    {
        limits.validate();
        transport::TcpServer server(router_, limits, options);
        server.listen(port, threads);
    }
};

} // namespace octane
