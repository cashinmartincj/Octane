/**
 * @file RequestDispatcher.h
 * @brief Central dispatch pipeline executing routes and handling fallback/error responses.
 */

#pragma once
#include "../Router.h"
#include "../HttpRequest.h"
#include "../HttpResponse.h"
#include <string>

namespace octane::core
{
    class RequestDispatcher {
        Router& router_;

        bool dispatch_safely(HttpRequest& req, HttpResponse& res) noexcept {
            try {
                if (!router_.resolve(req, res)) {
                    res.status(404).text("Not Found");
                }
                return true;
            } catch (...) {
                res = HttpResponse{};
                res.status(500).text("Internal Server Error");
                return false;
            }
        }

    public:
        explicit RequestDispatcher(Router& router) noexcept : router_(router) {}

        // In-place handler dispatch
        void dispatch(HttpRequest& req, HttpResponse& res) {
            (void)dispatch_safely(req, res);
        }

        // Returns structured HttpResponse object (used by TcpConnection)
        [[nodiscard]] HttpResponse dispatch_response(HttpRequest& req,
                                                     bool& close_connection) {
            HttpResponse res;
            if (!dispatch_safely(req, res)) close_connection = true;
            if (close_connection) {
                res.header("Connection", "close");
            }
            return res;
        }

        [[nodiscard]] HttpResponse dispatch_response(HttpRequest& req) {
            bool close_connection = false;
            return dispatch_response(req, close_connection);
        }

        // Default dispatch() overload returning std::string (satisfies tests like request_dispatch.cpp)
        [[nodiscard]] std::string dispatch(HttpRequest& req,
                                           bool& close_connection) {
            HttpResponse res = dispatch_response(req, close_connection);
            return res.serialize(!req.keep_alive || close_connection, req.method == HttpMethod::HEAD);
        }

        [[nodiscard]] std::string dispatch(HttpRequest& req) {
            bool close_connection = false;
            return dispatch(req, close_connection);
        }

        [[nodiscard]] std::string dispatch_to_string(HttpRequest& req) {
            bool close_connection = !req.keep_alive;
            return dispatch(req, close_connection);
        }
    };
}
