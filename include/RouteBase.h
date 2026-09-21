/**
 * @file RouteBase.h
 * @brief Zero-overhead CRTP route dispatchers.
 */

#pragma once
#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

namespace octane::routes {

    template <typename Derived>
    class Base {
    public:
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

        static Handler handler() noexcept {
            return &invoke;
        }
    };

    template <typename Derived>
    class Get : public Base<Derived> {
    public:
        static constexpr std::string_view method() noexcept { return "GET"; }

    protected:
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        std::string_view query (const HttpRequest& r, std::string_view k) const noexcept { return r.q(k);      }
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
        std::string_view cookie(const HttpRequest& r, std::string_view k) const noexcept { return r.cookie(k); }
    };

    template <typename Derived>
    class Post : public Base<Derived> {
    public:
        static constexpr std::string_view method() noexcept { return "POST"; }

    protected:
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
        std::string_view body  (const HttpRequest& r)                     const noexcept { return r.body;      }
        bool             is_json(const HttpRequest& r)                    const noexcept { return r.is_json(); }
    };

    template <typename Derived>
    class Put : public Post<Derived> {
    public:
        static constexpr std::string_view method() noexcept { return "PUT"; }
    };

    template <typename Derived>
    class Patch : public Post<Derived> {
    public:
        static constexpr std::string_view method() noexcept { return "PATCH"; }
    };

    template <typename Derived>
    class Del : public Base<Derived> {
    public:
        static constexpr std::string_view method() noexcept { return "DELETE"; }

    protected:
        std::string_view param (const HttpRequest& r, std::string_view k) const noexcept { return r.param(k);  }
        std::string_view header(const HttpRequest& r, std::string_view k) const noexcept { return r.header(k); }
    };
}