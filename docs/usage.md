# Using Octane

This guide covers building Octane, creating an application, registering routes,
reading requests, writing responses, selecting an execution policy, serving
mapped files, and running the supplied examples. See
[`deployment.md`](deployment.md) for production and Kubernetes operation and
[`hardening.md`](hardening.md) for transport internals and security boundaries.

## 1. Requirements

Octane's supported runtime is Linux with:

- a C++20 compiler;
- CMake 3.20 or newer;
- `pkg-config` and the `liburing` development package;
- a kernel, container runtime, and security policy that permit `io_uring`;
- Python 3 when building the test suite.

For Debian or Ubuntu, the development dependencies are typically installed
with:

```bash
sudo apt-get update
sudo apt-get install build-essential cmake pkg-config liburing-dev python3
```

Package names differ on other distributions. The native backend is not
supported on macOS or Windows.

## 2. Build and run the repository examples

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Run the HTML example:

```bash
./build/examples/01_hello_world/hello_world
```

Open <http://127.0.0.1:8080/> or test it from another terminal:

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/api/hello
```

Run the REST example instead:

```bash
./build/examples/02_rest_api/rest_api
curl -i http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/users
```

Only one example can use port 8080 at a time. Stop a running application with
`Ctrl+C`. The examples demonstrate the framework API; their in-memory data,
minimal JSON extraction, and static-asset choices are not production
application components.

To build just one example:

```bash
cmake --build build --target hello_world -j"$(nproc)"
```

## 3. Use Octane from another CMake project

Pin a released tag rather than tracking a moving branch:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(OCTANE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    octane
    GIT_REPOSITORY https://github.com/cashinmartincj/Octane.git
    GIT_TAG        v0.2.0
)
FetchContent_MakeAvailable(octane)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE octane_lib)
```

Omit the `BUILD_TESTING` assignment when the parent project intentionally
enables CTest targets. Because this is a standard CMake-wide option, forcing it
off also disables tests in other subprojects that follow it.

Configure and run that application normally:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/my_service
```

## 4. Minimal application

```cpp
#include "App.h"
#include "RouteBase.h"

class Hello : public octane::routes::Get<Hello> {
public:
    void handle(const octane::HttpRequest&, octane::HttpResponse& res) {
        res.status(200).text("Hello, World!");
    }
};

int main() {
    octane::App app;
    app.get<Hello>("/");
    app.listen(8080);
}
```

`App::listen` blocks the calling thread until `SIGINT`, `SIGTERM`, a call to
the underlying server's stop mechanism, or a worker failure ends the server.
Only one signal-managed server can listen in a process.

## 5. Define routes

### CRTP route classes

Octane's route classes dispatch through a static function pointer without a
virtual call:

```cpp
class GetUser : public octane::routes::Get<GetUser> {
public:
    void before(octane::HttpRequest&, octane::HttpResponse&) {
        // Optional hook. It does not skip handle().
    }

    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        const auto id = req.param("id");
        res.status(200).json(
            "{\"id\":\"" + std::string(id) + "\"}");
    }

    void after(octane::HttpRequest&, octane::HttpResponse&) {
        // Optional hook, called after handle() returns.
    }
};

app.get<GetUser>("/users/:id");
```

A route object is constructed for every invocation. Do not put persistent
application state in instance members; store shared state in an object with a
lifetime longer than the server and synchronize it where necessary. `before`,
`handle`, and `after` run in that order. A `before` hook is not middleware and
cannot short-circuit `handle`.

The supplied CRTP bases are:

| HTTP method | Route base | Registration |
| --- | --- | --- |
| GET | `octane::routes::Get<T>` | `app.get<T>(path)` |
| POST | `octane::routes::Post<T>` | `app.post<T>(path)` |
| PUT | `octane::routes::Put<T>` | `app.put<T>(path)` |
| PATCH | `octane::routes::Patch<T>` | `app.patch<T>(path)` |
| DELETE | `octane::routes::Del<T>` | `app.del<T>(path)` |

`App` and `Router` also expose `head` and `options` registration. Use a plain
handler for those methods because dedicated `routes::Head<T>` and
`routes::Options<T>` convenience bases are not currently provided.

### Plain handlers

`octane::Handler` is a function pointer. Free functions, static member
functions, and non-capturing lambdas are accepted; capturing lambdas are not.

```cpp
void health(octane::HttpRequest&, octane::HttpResponse& res) {
    res.status(200).json(R"({"status":"ok"})");
}

app.get("/health", health);
app.head("/health", +[](octane::HttpRequest&,
                         octane::HttpResponse& res) {
    res.status(200).text("body suppressed for HEAD");
});
```

### Static and dynamic paths

Literal paths use the static route table. A segment beginning with `:` creates
a path parameter:

```cpp
app.get<GetUser>("/users/:id");
// GET /users/42 -> req.param("id") == "42"
```

Register all routes before calling `listen`. Route mutation while requests are
being served is not supported.

## 6. Read a request

`octane::HttpRequest` provides:

| Member or accessor | Meaning |
| --- | --- |
| `method` | Parsed `HttpMethod` enum |
| `path` | Request path without the query string |
| `http_version` | `HTTP/1.0` or `HTTP/1.1` view |
| `body` | Owned request body string |
| `content_length` | Parsed Content-Length |
| `content_type` | Parsed `ContentType` enum |
| `keep_alive` | Connection persistence decision |
| `param(name, fallback)` | Dynamic route parameter |
| `q(name, fallback)` | Percent-decoded query value |
| `header(name, fallback)` | Case-insensitive header lookup |
| `cookie(name, fallback)` | Cookie lookup |
| `has_body()` | Whether the body is non-empty |
| `is_json()`, `is_form()`, ... | Content-Type helpers |

Example:

```cpp
void inspect(octane::HttpRequest& req, octane::HttpResponse& res) {
    const auto request_id = req.header("x-request-id", "missing");
    const auto page = req.q("page", "1");
    const auto session = req.cookie("session");

    if (!req.is_json() || !req.has_body()) {
        res.status(400).json(R"({"error":"JSON body required"})");
        return;
    }

    // Parse req.body with the application's JSON library.
    res.header("X-Request-Id", std::string(request_id))
       .status(200)
       .json("{\"page\":\"" + std::string(page) +
             "\",\"hasSession\":" +
             (session.empty() ? "false" : "true") + "}");
}
```

Most request maps contain views into connection-owned input. Do not retain
`path`, parameters, headers, cookies, or their returned `string_view` values
after the handler finishes. Query strings and `body` are owned by the request,
but should still be copied before use by work that outlives the request.

Valid `%HH` query escapes are decoded. A plus sign remains `+`; Octane does not
apply HTML-form plus-to-space conversion to URI queries.

## 7. Write a response

Response builders are chainable:

```cpp
res.status(201)
   .header("Location", "/users/42")
   .json(R"({"id":"42"})");
```

| Method | Behavior |
| --- | --- |
| `status(code)` | Selects the response status |
| `header(name, value)` | Adds or replaces a response header |
| `text(data)` | Copies data and selects `text/plain` |
| `html(data)` | Copies data and selects `text/html` |
| `json(data)` | Copies data and selects `application/json` |
| `send(data)` | Copies data without changing the current content type |
| `image(data, type)` | Copies data and selects the supplied image type |
| `html_view(data)` | Borrows mapped/static data as `text/html` |
| `image_view(data, type)` | Borrows mapped/static image data |

`Content-Length`, `Transfer-Encoding`, and `Connection` are transport-owned;
attempts to set them in a handler are ignored during serialization. Header
names and values are validated to prevent control-character injection.
Status 204 and 304 responses suppress bodies, as do responses to HEAD.
Exceptions escaping handlers or serialization generate a closing 500 response.

`html_view` and `image_view` do not copy the bytes. Their storage must remain
valid until the asynchronous response has finished, not merely until the
handler returns. Process-lifetime mappings are the simplest safe pattern.

## 8. Serve mapped files

```cpp
#include "FileHandle.h"
#include <stdexcept>

class Index : public octane::routes::Get<Index> {
public:
    void handle(const octane::HttpRequest&, octane::HttpResponse& res) {
        static octane::utils::MappedFile file = [] {
            octane::utils::MappedFile mapped;
            if (!mapped.open("/srv/my-service/index.html")) {
                throw std::runtime_error("cannot map index.html");
            }
            return mapped;
        }();
        res.status(200).html_view(file.view());
    }
};
```

`MappedFile::open` uses the path supplied by the application. Relative paths
are relative to the process working directory. The HTML example calculates
the executable directory itself before opening its bundled files.

Do not use request-controlled paths without canonicalization, allowlisting,
and traversal protection. Octane does not provide a static directory server.

## 9. Choose an execution policy

Inline execution is the default and is the fastest choice for bounded work
that never blocks:

```cpp
app.get("/health", health);
```

Move filesystem, database, DNS, long CPU work, and other potentially blocking
operations to a bounded queue:

```cpp
app.post("/reports", create_report,
         octane::HandlerExecution::shared_blocking());

app.get<GetUser>("/users/:id",
                 octane::HandlerExecution::named("database"));
```

Configure queues before listening:

```cpp
octane::transport::TcpServerOptions transport;
transport.execution_queues.shared_threads_per_shard = 1;
transport.execution_queues.shared_capacity = 256;
transport.execution_queues.named.push_back({"database", 2, 128});

app.listen(8080, 4, {}, transport);
```

Queue configuration is per ring shard. Four ring workers and two named queue
threads per shard can create eight queue threads for that named queue. Named
queues are created lazily. A full queue or an unconfigured named queue returns
503 and closes the connection.

Offloading copies and reparses the request, synchronizes through a queue, and
wakes the owning ring to deliver the response. Do not offload trivial handlers.
Queue workers cannot forcibly stop arbitrary C++ code, so a handler that never
returns can delay shutdown indefinitely.

## 10. Configure the server
├── Inline handler → executes directly on io_uring worker

```cpp
octane::HttpLimits limits;
limits.max_header_bytes = 16 * 1024;
limits.max_body_bytes = 1024 * 1024;
limits.max_connections = 4096;
limits.max_requests_per_connection = 1000;
limits.read_timeout = std::chrono::seconds(10);
limits.write_timeout = std::chrono::seconds(10);
limits.shutdown_timeout = std::chrono::seconds(5);

octane::transport::TcpServerOptions transport;
transport.bind_address = "0.0.0.0";
transport.pin_workers = true;
transport.ring_queue_depth = 256;

app.listen(8080, 4, limits, transport);
```

`listen(port, workers, limits, transport)` validates its arguments and blocks.
Port `0` asks the kernel for an ephemeral port and is mainly useful for tests.
`bind_address` accepts an IPv4 literal, not a hostname or an IPv6 address.

The default worker count is `hardware_concurrency`. Set it explicitly inside
containers because visible CPUs and CPU quotas do not always describe the same
capacity. CPU pinning uses the process's allowed affinity mask; disable it when
an orchestrator controls placement.

Request limits are per connection, not a process-wide memory limit. Application
allocations and response bodies are not capped by `HttpLimits`.

## 11. Protocol boundaries

Octane supports bounded HTTP/1.0 and HTTP/1.1 requests framed with
Content-Length. In particular:

- request `Transfer-Encoding`, including chunked bodies, is rejected;
- `Expect: 100-continue` is rejected with 417;
- duplicate Content-Length fields are rejected, even when identical;
- only origin-form targets and `OPTIONS *` are supported;
- HTTP/2, HTTP/3, WebSocket upgrades, TLS termination, compression, multipart
  decoding, and a JSON parser are not framework features;
- the framework does not cap application response size;
- authentication, authorization, CORS, persistence, logging, metrics, tracing,
  and application validation belong to the application or deployment edge.

Put Octane behind a trusted production edge and confirm that request bodies
reach it with Content-Length. See [`deployment.md`](deployment.md).

## 12. Test and benchmark

Run the full regression suite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Socket tests require permission to bind loopback ports. For a repeatable local
regression benchmark:

```bash
./build/octane_server_fixture 8080
```

In a second terminal:

```bash
python3 benchmarks/load.py --connections 16 --requests 1000 \
  --label 'revision/compiler/CPU'
python3 benchmarks/load.py --connections 16 --requests 1000 \
  --path /echo --body-bytes 4096 --label 'post-4k'
```

For a higher-throughput external tool:

```bash
wrk -t8 -c128 -d30s --latency http://127.0.0.1:8080/
```

Warm up first, preserve multiple measured runs, and compare only equivalent
hardware, builds, payloads, concurrency, and client placement. A loopback
benchmark is useful for regression detection but is not a production capacity
claim.

## 13. Common mistakes

- Calling `res.body(...)`: `body` is storage, not a builder; use `text`,
  `html`, `json`, or `send`.
- Registering a capturing lambda: handlers are function pointers.
- Performing blocking work inline: it stalls that ring worker.
- Retaining request views after the handler returns.
- Returning a mapped view backed by a temporary or unmapped object.
- Assuming a relative asset path is relative to the executable.
- Binding to `127.0.0.1` when the proxy is in a different Kubernetes Pod.
- Binding to `0.0.0.0` on a host where the origin port is publicly reachable.
- Trusting forwarding headers when clients can bypass the trusted proxy.
- Expecting the framework to decode chunked request bodies.
