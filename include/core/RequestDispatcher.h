/**
 * @file RequestDispatcher.h
 * @brief Central dispatch pipeline executing routes and handling fallback/error responses.
 *
 * @details
 * `RequestDispatcher` bridges the network transport layer (`TcpConnection`) and the routing layer (`Router`).
 * It receives parsed `HttpRequest` objects, routes them to registered callbacks, handles missing routes
 * (yielding standard 404 Not Found responses), and catches unhandled C++ exceptions from user code
 * (yielding standard 500 Internal Server Error responses while preventing worker crashes).
 *
 * Features:
 * - **Exception Boundary:** Encapsulates user handler execution within a `noexcept` `try...catch(...)`
 *   boundary, setting `500 Internal Server Error` and signaling connection close on panic.
 * - **Fallback 404:** Populates a clean 404 response when no static or dynamic route matches.
 * - **Flexible Output Modes:** Provides both in-place structured `HttpResponse` population and
 *   fully serialized `std::string` emission (for testing and single-shot buffer responses).
 *
 * Where this is imported / used:
 * - Direct Include: `#include "core/RequestDispatcher.h"`
 * - Network Transport: Instantiated in `octane::transport::TcpConnection` to evaluate incoming HTTP requests.
 * - Test Suites: Used in `tests/request_dispatch.cpp` to verify dispatch correctness and error isolation.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
 */

#pragma once
#include "../Router.h"
#include "../HttpRequest.h"
#include "../HttpResponse.h"
#include <string>

namespace octane::core
{
    /**
     * @class RequestDispatcher
     * @brief Dispatches parsed HTTP requests to the application Router with error handling.
     */
    class RequestDispatcher {
        Router& router_; ///< Reference to application route table

        /**
         * @brief Internal dispatch helper wrapping route execution in an exception safety boundary.
         * @param req Inbound HttpRequest.
         * @param res Outbound HttpResponse.
         * @return True if execution succeeded (including 404s); false if an unhandled exception occurred.
         */
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
        /**
         * @brief Constructs a RequestDispatcher bound to an active Router.
         * @param router Application Router instance.
         */
        explicit RequestDispatcher(Router& router) noexcept : router_(router) {}

        /**
         * @brief Dispatches the request in-place into the provided HttpResponse.
         * @param req Inbound HttpRequest.
         * @param res Outbound HttpResponse populated by handler or 404/500 fallback.
         */
        void dispatch(HttpRequest& req, HttpResponse& res) {
            (void)dispatch_safely(req, res);
        }

        /**
         * @brief Dispatches a request and returns a structured HttpResponse object.
         * @param req Inbound HttpRequest.
         * @param[out] close_connection Set to true if an error occurred requiring socket termination.
         * @return Populated HttpResponse object.
         */
        [[nodiscard]] HttpResponse dispatch_response(HttpRequest& req,
                                                     bool& close_connection) {
            HttpResponse res;
            if (!dispatch_safely(req, res)) close_connection = true;
            if (close_connection) {
                res.header("Connection", "close");
            }
            return res;
        }

        /**
         * @brief Dispatches a request and returns a structured HttpResponse object.
         * @param req Inbound HttpRequest.
         * @return Populated HttpResponse object.
         */
        [[nodiscard]] HttpResponse dispatch_response(HttpRequest& req) {
            bool close_connection = false;
            return dispatch_response(req, close_connection);
        }

        /**
         * @brief Dispatches a request and serializes the response to an HTTP/1.1 wire string.
         * @param req Inbound HttpRequest.
         * @param[out] close_connection Set to true if connection should close after transmission.
         * @return Wire-serialized HTTP response string.
         */
        [[nodiscard]] std::string dispatch(HttpRequest& req,
                                           bool& close_connection) {
            HttpResponse res = dispatch_response(req, close_connection);
            return res.serialize(!req.keep_alive || close_connection, req.method == HttpMethod::HEAD);
        }

        /**
         * @brief Dispatches a request and serializes the response string.
         * @param req Inbound HttpRequest.
         * @return Wire-serialized HTTP response string.
         */
        [[nodiscard]] std::string dispatch(HttpRequest& req) {
            bool close_connection = false;
            return dispatch(req, close_connection);
        }

        /**
         * @brief Dispatches a request and returns the serialized wire response string.
         * @param req Inbound HttpRequest.
         * @return Wire-serialized HTTP response string respecting keep-alive headers.
         */
        [[nodiscard]] std::string dispatch_to_string(HttpRequest& req) {
            bool close_connection = !req.keep_alive;
            return dispatch(req, close_connection);
        }
    };
}
