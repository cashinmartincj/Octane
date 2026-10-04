/**
 * @file RouteBase.h
 * @brief Zero-overhead CRTP (Curiously Recurring Template Pattern) route base classes.
 *
 * @details
 * `RouteBase.h` provides compile-time polymorphic base classes for structuring HTTP endpoints
 * as dedicated C++ classes rather than raw lambda callbacks.
 *
 * Architectural Features:
 * - **Zero VTable Overhead:** Eliminates virtual function tables and dynamic dispatch. Method resolution
 *   and lifecycle hook evaluation occur entirely at compile time via static polymorphism (`Derived`).
 * - **Compile-Time Contract Validation:** Uses C++20 `requires` clauses and `static_assert` to verify
 *   that the derived class implements `handle(HttpRequest&, HttpResponse&)`.
 * - **Optional Lifecycle Hooks:** Detects presence of `before(HttpRequest&, HttpResponse&)` and
 *   `after(HttpRequest&, HttpResponse&)` via `if constexpr` reflection, invoking them only when defined.
 * - **Convenience Accessors:** Provides protected shorthand helpers (`param`, `query`, `header`, `cookie`, `body`).
 *
 * Where this is imported / used:
 * - Direct Include: `#include "RouteBase.h"` or via `#include "Octane.h"`
 * - Route Registration: Used with `app.route<T>(path)` in `octane::init` (`include/Octane.h`).
 * - Application Handlers: Base classes subclassed across the application backend for modular, testable controllers.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once
#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

namespace octane::routes {

    /**
     * @class Base
     * @brief CRTP root base class providing lifecycle orchestration and function pointer generation.
     * @tparam Derived Subclass implementing `handle(HttpRequest&, HttpResponse&)`.
     */
    template <typename Derived>
    class Base {
    public:
        /**
         * @brief Static entry point executing the route lifecycle (before -> handle -> after).
         * @param req Inbound HttpRequest reference.
         * @param res Outbound HttpResponse reference.
         */
        static void invoke(HttpRequest& req, HttpResponse& res) {
            Derived instance;
            static_assert(requires { instance.handle(req, res); },
                          "Routes must implement handle(HttpRequest&, HttpResponse&)");
            if constexpr (requires { instance.before(req, res); })
                instance.before(req, res);
            instance.handle(req, res);
            if constexpr (requires { instance.after(req, res); })
                instance.after(req, res);
        }

        /**
         * @brief Converts the static `invoke` method into a std::function-compatible Handler.
         * @return Function pointer to `invoke`.
         */
        static Handler handler() noexcept {
            return &invoke;
        }
    };

    /**
     * @class Get
     * @brief CRTP base class for HTTP GET endpoints.
     * @tparam Derived Derived controller class implementing `handle(HttpRequest&, HttpResponse&)`.
     *
     * Example:
     * @code
     * class GetUser : public octane::routes::Get<GetUser> {
     * public:
     *     void handle(HttpRequest& req, HttpResponse& res) {
     *         auto id = param(req, "id");
     *         res.json(R"({"user_id": ")" + std::string(id) + R"("})");
     *     }
     * };
     * app.route<GetUser>("/users/:id");
     * @endcode
     */
    template <typename Derived>
    class Get : public Base<Derived> {
    public:
        /// Static HTTP method identifier
        static constexpr std::string_view method() noexcept { return "GET"; }

    protected:
        /// Helper to extract a dynamic route parameter (e.g. `:id`)
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        /// Helper to extract a query parameter (e.g. `?limit=10`)
        std::string_view query (const HttpRequest& r, std::string_view k) const noexcept { return r.q(k);      }
        /// Helper to extract an HTTP request header (case-insensitive)
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
        /// Helper to extract a cookie value
        std::string_view cookie(const HttpRequest& r, std::string_view k) const noexcept { return r.cookie(k); }
    };

    /**
     * @class Post
     * @brief CRTP base class for HTTP POST endpoints.
     * @tparam Derived Derived controller class implementing `handle(HttpRequest&, HttpResponse&)`.
     */
    template <typename Derived>
    class Post : public Base<Derived> {
    public:
        /// Static HTTP method identifier
        static constexpr std::string_view method() noexcept { return "POST"; }

    protected:
        /// Helper to extract a dynamic route parameter (e.g. `:id`)
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        /// Helper to extract an HTTP request header (case-insensitive)
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
        /// Helper to access raw request payload body
        std::string_view body  (const HttpRequest& r)                     const noexcept { return r.body;      }
        /// Helper to verify if Content-Type is application/json
        bool             is_json(const HttpRequest& r)                    const noexcept { return r.is_json(); }
    };

    /**
     * @class Put
     * @brief CRTP base class for HTTP PUT endpoints.
     * @tparam Derived Derived controller class implementing `handle(HttpRequest&, HttpResponse&)`.
     */
    template <typename Derived>
    class Put : public Post<Derived> {
    public:
        /// Static HTTP method identifier
        static constexpr std::string_view method() noexcept { return "PUT"; }
    };

    /**
     * @class Patch
     * @brief CRTP base class for HTTP PATCH endpoints.
     * @tparam Derived Derived controller class implementing `handle(HttpRequest&, HttpResponse&)`.
     */
    template <typename Derived>
    class Patch : public Post<Derived> {
    public:
        /// Static HTTP method identifier
        static constexpr std::string_view method() noexcept { return "PATCH"; }
    };

    /**
     * @class Del
     * @brief CRTP base class for HTTP DELETE endpoints.
     * @tparam Derived Derived controller class implementing `handle(HttpRequest&, HttpResponse&)`.
     */
    template <typename Derived>
    class Del : public Base<Derived> {
    public:
        /// Static HTTP method identifier
        static constexpr std::string_view method() noexcept { return "DELETE"; }

    protected:
        /// Helper to extract a dynamic route parameter (e.g. `:id`)
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        /// Helper to extract an HTTP request header (case-insensitive)
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
    };

    // ── C++23 Non-Templated Route Bases ──────────────────────────────────

    /**
     * @class Endpoint
     * @brief Modern C++23 non-templated base class eliminating CRTP template boilerplate.
     *
     * Example:
     * @code
     * class UserProfile : public octane::routes::GetRoute {
     * public:
     *     void handle(HttpRequest& req, HttpResponse& res) {
     *         auto id = param(req, "id");
     *         res.json(R"({"user_id": ")" + std::string(id) + R"("})");
     *     }
     * };
     * app.route<UserProfile>("/users/:id");
     * @endcode
     */
    class Endpoint {
    protected:
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        std::string_view query (const HttpRequest& r, std::string_view k) const noexcept { return r.q(k);      }
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
        std::string_view cookie(const HttpRequest& r, std::string_view k) const noexcept { return r.cookie(k); }
        std::string_view body  (const HttpRequest& r)                     const noexcept { return r.body;      }
        bool             is_json(const HttpRequest& r)                    const noexcept { return r.is_json(); }
    };

    struct GetRoute : Endpoint { static constexpr std::string_view method() noexcept { return "GET"; } };
    struct PostRoute : Endpoint { static constexpr std::string_view method() noexcept { return "POST"; } };
    struct PutRoute : Endpoint { static constexpr std::string_view method() noexcept { return "PUT"; } };
    struct PatchRoute : Endpoint { static constexpr std::string_view method() noexcept { return "PATCH"; } };
    struct DelRoute : Endpoint { static constexpr std::string_view method() noexcept { return "DELETE"; } };
    struct OptionsRoute : Endpoint { static constexpr std::string_view method() noexcept { return "OPTIONS"; } };
    struct HeadRoute : Endpoint { static constexpr std::string_view method() noexcept { return "HEAD"; } };

} // namespace octane::routes