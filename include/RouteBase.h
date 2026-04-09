/**
 * @file RouteBase.h
 * @brief CRTP base classes for defining type-safe route handlers
 *
 * Provides strongly typed base classes for each HTTP method.
 * Developers inherit from the appropriate class and implement handle():
 *
 *   class GetUser : public routes::Get<GetUser> {
 *       void handle(const HttpRequest& req, HttpResponse& res) {
 *           auto id = param(req, "id");
 *           res.status(200).json("...");
 *       }
 *   };
 *
 * CRTP (Curiously Recurring Template Pattern) is used instead of
 * virtual functions — zero vtable overhead, all dispatch resolved
 * at compile time.
 *
 * Each route class exposes only the helpers relevant to its method:
 *   Get         — param, query, header, cookie
 *   Post/Put    — param, header, body, is_json
 *   Del         — param, header
 */

#pragma once
#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

namespace routes {

    /**
    * @brief CRTP base for all route handlers
    *
    * Provides the invoke() entry point called by the router.
    * Runs the before → handle → after lifecycle.
    *
    * Derived classes must implement:
    *   void handle(HttpRequest& req, HttpResponse& res)
    *
    * Derived classes may optionally override:
    *   void before(HttpRequest& req, HttpResponse& res)
    *   void after (HttpRequest& req, HttpResponse& res)
    */
    template <typename Derived>
    class Base {
    public:
        static void invoke(HttpRequest& req, HttpResponse& res) {
            Derived instance;
            static_cast<Derived*>(&instance)->before(req, res);
            static_cast<Derived*>(&instance)->handle(req, res);
            static_cast<Derived*>(&instance)->after (req, res);
        }

        static Handler handler() {
            return &invoke;
        }

        void before(HttpRequest& req, HttpResponse& res) {}

        void after (HttpRequest& req, HttpResponse& res) {}
    };

    template <typename Derived>
    class Get : public Base<Derived> {
    public:
        static constexpr std::string_view method() { return "GET"; }

    protected:
        const std::string& param (const HttpRequest& r, const std::string& k) const { return r.param(k);  }
        const std::string& query (const HttpRequest& r, const std::string& k) const { return r.q(k);      }
        const std::string& header(const HttpRequest& r, const std::string& k) const { return r.header(k); }
        const std::string& cookie(const HttpRequest& r, const std::string& k) const { return r.cookie(k); }
    };

    template <typename Derived>
    class Post : public Base<Derived> {
    public:
        static constexpr std::string_view method() { return "POST"; }

    protected:
        const std::string& param  (const HttpRequest& r, const std::string& k) const { return r.param(k);  }
        const std::string& header (const HttpRequest& r, const std::string& k) const { return r.header(k); }
        const std::string& body   (const HttpRequest& r)                        const { return r.body;      }
        bool               is_json(const HttpRequest& r)                        const { return r.is_json(); }
    };

    template <typename Derived>
    class Put : public Post<Derived> {
    public:
        static constexpr std::string_view method() { return "PUT"; }
    };

    template <typename Derived>
    class Patch : public Post<Derived> {
    public:
        static constexpr std::string_view method() { return "PATCH"; }
    };

    template <typename Derived>
    class Del : public Base<Derived> {
    public:
        static constexpr std::string_view method() { return "DELETE"; }

    protected:
        const std::string& param (const HttpRequest& r, const std::string& k) const { return r.param(k);  }
        const std::string& header(const HttpRequest& r, const std::string& k) const { return r.header(k); }
    };
}