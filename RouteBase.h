// routes/RouteBase.h
#pragma once
#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

namespace routes {

    template <typename Derived>
    class Base {
    public:
        static void invoke(HttpRequest& req, HttpResponse& res) {
            Derived instance;
            // ← CRTP static_cast dispatch
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