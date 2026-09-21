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

    public:
        explicit RequestDispatcher(Router& router) noexcept : router_(router) {}

        // In-place handler dispatch
        void dispatch(HttpRequest& req, HttpResponse& res) {
            try {
                if (!router_.resolve(req, res)) {
                    res.status(404).text("Not Found");
                }
            } catch (const std::exception&) {
                res.status(500).text("Internal Server Error");
            } catch (...) {
                res.status(500).text("Internal Server Error");
            }
        }

        // Returns structured HttpResponse object (used by TcpConnection)
        [[nodiscard]] HttpResponse dispatch_response(HttpRequest& req, bool close_connection = false) {
            HttpResponse res;
            dispatch(req, res);
            if (close_connection) {
                res.header("Connection", "close");
            }
            return res;
        }

        // Default dispatch() overload returning std::string (satisfies tests like request_dispatch.cpp)
        [[nodiscard]] std::string dispatch(HttpRequest& req, bool close_connection = false) {
            HttpResponse res = dispatch_response(req, close_connection);
            return res.serialize(!req.keep_alive || close_connection, req.method == HttpMethod::HEAD);
        }

        [[nodiscard]] std::string dispatch_to_string(HttpRequest& req) {
            return dispatch(req, !req.keep_alive);
        }
    };
}