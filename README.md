# Octane

A high-performance async HTTP/1.1 web framework written in C++20.

Built on Linux `io_uring` with an isolated thread-per-core architecture (`SO_REUSEPORT`), a hybrid zero-allocation router (O(1) hash map for static routes, trie for dynamic), zero-copy mapped-body serving, scatter-gather I/O, and CRTP-based route handlers with no virtual dispatch overhead.

Performance needs to be measured for your workload. See the transport limits, supported protocol behavior, and reproducible checks below.

---

## Features

- **Thread-per-core architecture** — an independent `io_uring` and listening socket per worker.
- **Kernel-level connection balancing** — Linux `SO_REUSEPORT` socket distribution.
- **Scatter-gather response I/O** — headers and payloads are submitted together with `sendmsg`; mapped bodies are not copied into an intermediate response string.
- **Rolling input window** — amortizes compaction and reallocations on pipelined keep-alive streams.
- **Batched ring operation** — completion queues are drained in batches and generated submissions are flushed together.
- **Multishot accept** — used when supported by the running kernel, with automatic single-shot fallback.
- **Per-route execution policy** — keep small handlers inline or isolate blocking work in bounded shared/named queues.
- **TCP_NODELAY enabled** — sub-millisecond tail latency by disabling Nagle's algorithm.
- **Persistent keep-alive connections** with pipelining support.
- **Hybrid router** — O(1) static route lookup, trie for dynamic routes with path parameters.
- **CRTP route handlers** — compile-time lifecycle (`before`, `handle`, `after`) with zero vtable overhead.
- **Zero-heap HTTP/1.1 parser** — request-line, headers, query string, and cookies as non-owning string views.
- **Zero-copy static file serving** via virtual memory `mmap`.
- **POST/PUT/PATCH body parsing** via two-phase Content-Length reads.
- **Single include, two-line route registration**.

---

## Requirements

- C++20
- CMake 3.20+
- Linux with `io_uring` support
- `liburing` and `pkg-config`
- Platform threads (`pthreads`)

Install the development package for `liburing` before configuring the default backend.

---

## Platform Support

| Platform | Status |
| --- | --- |
| Linux | ✅ Native `io_uring` backend with `SO_REUSEPORT` |
| macOS | ❌ Native backend unavailable |
| Windows | ❌ Native backend unavailable |

---

## HTTP Transport Limits and Protocol Behavior

Octane supports bounded Content-Length requests over HTTP/1.0 and HTTP/1.1. It is not a complete HTTP implementation or a claim of production readiness. The framing policy follows [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html): ambiguous framing is rejected and the connection is closed.

- This implementation conservatively rejects all duplicate Content-Length fields, even identical ones.
- Transfer-Encoding is currently rejected with 501 (400 when combined with Content-Length).
- Expect requests receive 417; chunked decoding and 100-continue are future work.
- Only origin-form targets and `OPTIONS *` are supported.
- Host presence, duplicates, and unsafe delimiters are checked; full URI authority validation and proxy absolute-form handling are not implemented.
- Query names and values percent-decode valid `%HH` octets into owned strings. A plus sign remains a literal `+`; form-style plus-to-space conversion is not applied to the URI query.

### Configuration

```cpp
octane::HttpLimits limits;
limits.max_header_bytes = 16 * 1024;
limits.max_body_bytes = 1024 * 1024;
limits.max_connections = 4096;
limits.max_requests_per_connection = 1000;
limits.read_timeout = std::chrono::seconds(10);
limits.write_timeout = std::chrono::seconds(10);
limits.shutdown_timeout = std::chrono::seconds(5);

// Binds to port 8080 with 8 worker threads matching core count
app.listen(8080, 8, limits);
```

These are the defaults. Header bytes include the request line and final CRLF. The read deadline covers an entire request, including its body, and also limits idle keep-alive time. Sending occasional bytes does not restart the deadline. Timeouts and incomplete socket reads close the connection. Header/body limit errors return 431/413 then close. Excess accepted connections are closed without starting a request.

Limits are per connection, not a global memory budget: request storage, parsed maps, output strings and application allocations add overhead. Application response size is not currently capped.

Each connection executes strictly within its assigned ring thread. Input and output storage stays owned until completion; request accessors return borrowed views which handlers must not retain beyond request lifetime. Complete pipelined requests are processed in arrival order. The transport owns Content-Length and Connection response fields. HEAD responses and 204/304 statuses suppress the body. Handler or serialization exceptions produce a 500 and close; exception details are not sent to clients.

SIGINT/SIGTERM (or `TcpServer::stop()`) stop accepting, close idle connections, and let active requests, queued handlers, or responses drain. Ring cancellation completions are drained before worker exit. Only one signal-managed `TcpServer` may listen in a process at a time. Inline handlers execute synchronously and therefore must not block; blocking handlers should opt into one of the bounded execution queues.

The transport pools operation contexts, tracks partial writes without rebuilding response buffers, and keeps one allocation-free indexed deadline-heap entry per active connection instead of scanning every connection or attaching a timeout SQE to every operation. Connections and completion handles are owned by one ring shard without hot-path shared-reference counting. Worker failures propagate back through `listen()`. The exact global connection cap still requires one relaxed atomic update on accept and close.

Workers are pinned to CPUs from the process's allowed affinity mask by default, and each ring defaults to 256 entries. Both choices are configurable:

```cpp
octane::transport::TcpServerOptions transport;
transport.pin_workers = true;
transport.ring_queue_depth = 256;
transport.execution_queues.shared_threads_per_shard = 1;
transport.execution_queues.shared_capacity = 1024;
transport.execution_queues.named.push_back({"database", 2, 256});
app.listen(8080, 8, limits, transport);
```

Disable worker pinning when an external runtime manages affinity. Queue depth should be measured under the intended concurrency; making it very large consumes additional locked kernel memory and is not automatically faster.

Handlers are inline by default. Offloading is explicit:

```cpp
app.get("/health", health); // Inline fast path: no request copy or queue hop.
app.post("/reports", reports,
         octane::HandlerExecution::shared_blocking());
app.get("/users", users,
        octane::HandlerExecution::named("database"));
```

Execution queues are shard-local. With eight ring workers, a named configuration using two `threads_per_shard` creates at most sixteen threads once that queue is first used. `capacity` limits waiting jobs per shard; it does not include currently executing jobs. A saturated shared queue, saturated named queue, or unconfigured named queue returns `503 Service Unavailable` and closes that connection.

Only offloaded requests are materialized into owned raw request storage and reparsed on the queue worker, so their header, path, parameter, cookie, query, and body views remain valid. Responses are posted through an `eventfd` wakeup to the connection's owning ring. This copy, queue synchronization, and thread handoff make offloading inappropriate for trivial handlers; leave those inline.

## Origin Deployment Behind NGINX or Cloudflare

Octane is intended to run as an HTTP/1.1 origin behind a production edge rather than terminate public TLS directly. Keep the origin bound to a private interface, loopback address, firewall-restricted address, or Cloudflare Tunnel. Do not trust forwarding headers from arbitrary clients; accept them only when direct access to the origin is blocked.

For NGINX, enable [request buffering](https://nginx.org/en/docs/http/ngx_http_proxy_module.html#proxy_request_buffering) and apply an edge body limit no larger than the application's configured limit. Preserve upstream keep-alive and forwarding metadata only from the trusted proxy:

```nginx
location / {
    client_max_body_size 1m;
    proxy_request_buffering on;
    proxy_buffering on;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_pass http://127.0.0.1:8080;
}
```

Octane deliberately rejects chunked request bodies. Confirm that the selected proxy configuration buffers and forwards bodies with `Content-Length`; do not expose the origin until that behavior has been tested with the deployed proxy version.

For [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/), point the local service at Octane's private HTTP listener and enable the [`disableChunkedEncoding`](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/configure-tunnels/origin-parameters/#disablechunkedencoding) origin option. Tunnel uses outbound-only connections, allowing inbound access to the origin to remain blocked. Use more than one tunnel connector where origin availability requires it.

## Build

```bash
git clone https://github.com/cashinmartincj/octane
cd octane
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

To skip examples:

```bash
cmake .. -DOCTANE_BUILD_EXAMPLES=OFF
```

To build a single example:

```bash
cmake --build . --target hello_world
```

## Build and Regression Tests

From the Octane directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Tests cover parser rejection, body/header boundaries, fragmented requests, pipelining, HTTP/1.0 and Connection: close, exceptions, disconnects, idle/body and trickle deadlines, slow readers, connection capacity and graceful shutdown. Network tests need permission to bind a loopback socket. Python 3 is required when `BUILD_TESTING` is enabled. Set `BUILD_TESTING=OFF` for a library-only build.

### Sanitizers

```bash
cmake -S . -B build-sanitize -DCMAKE_CXX_COMPILER=clang++ \
  -DOCTANE_SANITIZERS=ON -DOCTANE_BUILD_EXAMPLES=OFF
cmake --build build-sanitize -j$(nproc)
ctest --test-dir build-sanitize --output-on-failure
```

`OCTANE_SANITIZERS` enables ASan/UBSan with GCC or Clang; it is not ThreadSanitizer.

## Parser Fuzzing

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
  -DOCTANE_FUZZING=ON -DOCTANE_BUILD_EXAMPLES=OFF
cmake --build build-fuzz --target octane_fuzz_parser -j$(nproc)
mkdir -p /tmp/octane-corpus
cp tests/corpus/* /tmp/octane-corpus/
build-fuzz/octane_fuzz_parser /tmp/octane-corpus -max_total_time=60 -max_len=20000
```

Use a writable corpus copy; libFuzzer adds minimized inputs. The target checks the complete parser with ASan/UBSan and accepts only `HttpParseError` as an expected rejection. A short fuzz run is a smoke check, not proof of protocol correctness.

## Repeatable Load Baseline & Benchmarks

Build Release, then run the fixture with production defaults:

```bash
build/octane_server_fixture 8080
# In another terminal:
python3 benchmarks/load.py --connections 16 --requests 1000 --label 'revision/compiler/CPU'
python3 benchmarks/load.py --connections 16 --requests 1000 --path /echo --body-bytes 4096
```

The JSON output records settings, client platform, elapsed time, successes, errors, received bytes, throughput, and p50/p95/p99 successful-request latency. The script is closed-loop, includes connection establishment, and has no warmup. Run one warmup invocation and save several measured runs using identical builds, hardware and concurrency. Python and a colocated client can bottleneck the result; this is a regression baseline, not the server's maximum capacity. Use a dedicated load generator on a separate machine for capacity claims and monitor server CPU and RSS separately. The `benchmark_smoke` CTest runs GET and 4 KiB POST workloads and validates its JSON results.

### High-Throughput Baseline (8-Core Linux, Loopback)

```bash
wrk -t8 -c128 -d30s --latency http://127.0.0.1:8080/
```

- **Requests/sec:** ~696,222
- **Transfer/sec:** ~4.03 GB
- **Median Latency:** ~102 µs
- **99% Latency:** ~1.26 ms

## Validation Performed for This Transport Revision

- The project compiled successfully with the configured GCC C++20 build.
- `request_dispatch` and the real-socket `tcp_transport` regression tests passed.
- The GCC ThreadSanitizer transport run passed after replacing asynchronous signal-handler state with a blocked-signal waiter and atomic stop flag.
- Clang ASan/UBSan passed the request, transport, and benchmark tests with leak detection enabled.
- All five CTest tests passed, including parser hardening, request dispatch, bounded execution queues, real TCP behavior, and the benchmark smoke test.
- `benchmark_smoke` completed 2,000 GET and 2,000 4 KiB POST requests with zero reported errors.
- A ten-second local Release `wrk` regression run (`-t8 -c256`) with execution queues compiled in measured about 615k inline GET requests/s, with approximately 429 us median and 0.74 ms p99 latency. The immediately preceding build without execution queues measured about 618k requests/s; that difference is within normal local-run variation. Loopback results are regression checks rather than capacity claims.
- A longer Release soak completed approximately 17.55 million GETs over 30 seconds at 256 connections, followed by 128,000 persistent 4 KiB POSTs, with zero reported errors.

No multi-hour soak test or full HTTP conformance audit was performed for this revision. A slow handler explicitly left inline remains capable of stalling its assigned ring thread.

## Quick Start Guide

```cpp
#include "App.h"

class HelloWorld : public octane::routes::Get<HelloWorld> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        res.status(200).body("Hello, World!");
    }
};

int main() {
    octane::App app;
    app.get<HelloWorld>("/");
    app.listen(8080);
}
```

Or with `using namespace octane` to avoid prefixing:

```cpp
using namespace octane;

class HelloWorld : public routes::Get<HelloWorld> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        res.status(200).body("Hello, World!");
    }
};

int main() {
    App app;
    app.get<HelloWorld>("/");
    app.listen(8080);
}
```

### Route Parameters

```cpp
class GetUser : public octane::routes::Get<GetUser> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        auto id = req.param("id");
        res.status(200).json("{\"id\":\"" + std::string(id) + "\"}");
    }
};

app.get<GetUser>("/users/:id");
```

### POST Body

```cpp
class CreateUser : public octane::routes::Post<CreateUser> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        auto body = req.body; // raw body string
        res.status(201).json(body);
    }
};

app.post<CreateUser>("/users");
```

### Static File Serving

```cpp
#include "FileHandle.h"

class ServeFile : public octane::routes::Get<ServeFile> {
    octane::utils::MappedFile file_;

public:
    ServeFile() {
        file_.open("index.html");
    }

    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        res.status(200).html_view(file_.view()); // zero-copy via mmap
    }
};
```

Files are resolved relative to the executable directory.

### Auth, Middleware & CORS

Octane is an infrastructure layer — routing, parsing, and I/O. Auth, CORS, and logging are application concerns handled inside your route handlers:

```cpp
class ProtectedRoute : public octane::routes::Get<ProtectedRoute> {
public:
    void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
        auto token = req.header("Authorization");
        if (token.empty()) {
            res.status(401).json("{\"error\":\"unauthorized\"}");
            return;
        }
        // proceed
    }
};
```

### Supported HTTP Methods

| Method | CRTP Base |
| --- | --- |
| GET | `octane::routes::Get<T>` |
| POST | `octane::routes::Post<T>` |
| PUT | `octane::routes::Put<T>` |
| PATCH | `octane::routes::Patch<T>` |
| DELETE | `octane::routes::Del<T>` |
| OPTIONS | `octane::routes::Options<T>` |

## Project Structure

```text
octane/
├── include/
│   ├── App.h           # Thread pool, accept loop, entry point
│   ├── Router.h        # Hybrid static/dynamic router
│   ├── Trie.h          # Trie for dynamic route matching
│   ├── HttpParser.h    # Raw bytes → HttpRequest
│   ├── HttpRequest.h   # Request struct + helpers
│   ├── HttpResponse.h  # Response struct + serialization
│   ├── HttpTypes.h     # ContentType, HttpMethod, Handler
│   ├── HttpLimits.h    # Connection bounds and timeout policies
│   ├── RouteBase.h     # CRTP base classes
│   ├── core/           # Request dispatch pipeline
│   ├── transport/      # SO_REUSEPORT multi-acceptor & rolling buffers
│   └── FileHandle.h    # mmap file serving (octane::utils)
├── src/
│   └── FileHandle.cpp
├── examples/
│   ├── 01_hello_world/
│   └── 02_rest_api/
├── CMakeLists.txt
├── LICENSE
└── README.md
```

## Using as a Library (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
    octane
    GIT_REPOSITORY https://github.com/cashinmartincj/octane
    GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(octane)

target_link_libraries(your_app PRIVATE octane_lib)
```

## License

MIT — see [LICENSE](LICENSE).
