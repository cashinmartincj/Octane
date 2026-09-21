#pragma once
#include "Router.h"
#include "HttpParser.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include "HttpLimits.h"
#include <asio.hpp>
#include <thread>
#include "transport/TcpServer.h"

namespace octane 
{
    class App {
        Router router_;

    public:
        // CRTP Route Registration
        template<typename RouteClass> App& get    (std::string_view path) { router_.get    (path, RouteClass::handler()); return *this; }
        template<typename RouteClass> App& post   (std::string_view path) { router_.post   (path, RouteClass::handler()); return *this; }
        template<typename RouteClass> App& put    (std::string_view path) { router_.put    (path, RouteClass::handler()); return *this; }
        template<typename RouteClass> App& patch  (std::string_view path) { router_.patch  (path, RouteClass::handler()); return *this; }
        template<typename RouteClass> App& del    (std::string_view path) { router_.del    (path, RouteClass::handler()); return *this; }
        template<typename RouteClass> App& options(std::string_view path) { router_.options(path, RouteClass::handler()); return *this; }
        template<typename RouteClass> App& head   (std::string_view path) { router_.head   (path, RouteClass::handler()); return *this; }

        template<typename RouteClass> App& get    (std::string_view path, HandlerExecution e) { router_.get    (path, RouteClass::handler(), std::move(e)); return *this; }
        template<typename RouteClass> App& post   (std::string_view path, HandlerExecution e) { router_.post   (path, RouteClass::handler(), std::move(e)); return *this; }
        template<typename RouteClass> App& put    (std::string_view path, HandlerExecution e) { router_.put    (path, RouteClass::handler(), std::move(e)); return *this; }
        template<typename RouteClass> App& patch  (std::string_view path, HandlerExecution e) { router_.patch  (path, RouteClass::handler(), std::move(e)); return *this; }
        template<typename RouteClass> App& del    (std::string_view path, HandlerExecution e) { router_.del    (path, RouteClass::handler(), std::move(e)); return *this; }
        template<typename RouteClass> App& options(std::string_view path, HandlerExecution e) { router_.options(path, RouteClass::handler(), std::move(e)); return *this; }
        template<typename RouteClass> App& head   (std::string_view path, HandlerExecution e) { router_.head   (path, RouteClass::handler(), std::move(e)); return *this; }

        // Function / Lambda Route Registration
        App& get    (std::string_view path, Handler h) { router_.get    (path, std::move(h)); return *this; }
        App& post   (std::string_view path, Handler h) { router_.post   (path, std::move(h)); return *this; }
        App& put    (std::string_view path, Handler h) { router_.put    (path, std::move(h)); return *this; }
        App& patch  (std::string_view path, Handler h) { router_.patch  (path, std::move(h)); return *this; }
        App& del    (std::string_view path, Handler h) { router_.del    (path, std::move(h)); return *this; }
        App& options(std::string_view path, Handler h) { router_.options(path, std::move(h)); return *this; }
        App& head   (std::string_view path, Handler h) { router_.head   (path, std::move(h)); return *this; }

        App& get    (std::string_view path, Handler h, HandlerExecution e) { router_.get    (path, h, std::move(e)); return *this; }
        App& post   (std::string_view path, Handler h, HandlerExecution e) { router_.post   (path, h, std::move(e)); return *this; }
        App& put    (std::string_view path, Handler h, HandlerExecution e) { router_.put    (path, h, std::move(e)); return *this; }
        App& patch  (std::string_view path, Handler h, HandlerExecution e) { router_.patch  (path, h, std::move(e)); return *this; }
        App& del    (std::string_view path, Handler h, HandlerExecution e) { router_.del    (path, h, std::move(e)); return *this; }
        App& options(std::string_view path, Handler h, HandlerExecution e) { router_.options(path, h, std::move(e)); return *this; }
        App& head   (std::string_view path, Handler h, HandlerExecution e) { router_.head   (path, h, std::move(e)); return *this; }

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
}
