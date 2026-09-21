# HTTP Transport Limits and Verification

Octane supports high-throughput, bounded Content-Length requests over HTTP/1.0 and HTTP/1.1.
It is an optimized, low-latency microframework designed around a modern thread-per-core asynchronous execution architecture.
The framing policy follows [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html): ambiguous framing is rejected and the connection is closed. This implementation conservatively rejects all duplicate Content-Length fields, even identical ones. Transfer-Encoding is currently rejected with 501 (400 when combined with Content-Length). Expect requests receive 417; chunked decoding and 100-continue are future work. Only origin-form targets and `OPTIONS *` are supported. Host presence, duplicates, and unsafe delimiters are checked; full URI authority validation and proxy absolute-form handling are not implemented.

Query names and values decode valid `%HH` octets into request-owned strings. Invalid escapes are rejected. URI query plus signs remain literal and are not treated as form-encoded spaces. Response header names and values are validated during serialization; control-character injection is rejected before any response bytes are queued.

## Architecture and Execution Model

- **Thread-per-Core Execution:** The server provisions an independent `io_uring`, listening socket, and completion loop for each worker thread.
- **Kernel Load Balancing (`SO_REUSEPORT`):** On Linux platforms supporting `SO_REUSEPORT`, each worker binds and accepts on its own socket descriptor, allowing the operating system kernel to distribute incoming connections across cores without cross-thread lock contention or strand scheduling overhead.
- **Rolling Input Window:** Each connection retains a read position inside contiguous input storage. Consumed prefixes are compacted only when the window is empty, large, or mostly consumed, amortizing data movement during pipelining.
- **Scatter-Gather Response I/O:** Response headers and bodies are submitted with `sendmsg` iovecs. Dynamic bodies move into connection-owned storage, while mapped bodies remain borrowed views and are not copied into a combined serialization buffer.
- **Batched Ring Traffic:** The completion loop consumes CQEs in batches and flushes SQEs produced by the batch together. Reads and writes use pooled operation contexts instead of allocating an operation and linked timeout for every submission.
- **Accept Strategy:** Multishot accept is enabled when the kernel supports it. An `EINVAL` completion transparently switches that worker to single-shot accept; resource errors use a bounded retry delay.
- **Deadline Strategy:** Each shard maintains an allocation-free indexed minimum heap with at most one timer entry per live connection. The ring loop cancels only expired operations instead of scanning every connection or doubling normal SQE/CQE traffic with linked timeouts. Expiry precision is bounded by the shorter of the next deadline and the 25 ms stop-poll interval.
- **Shard-local Ownership:** Connection objects live in one worker's map and completion contexts carry non-owning handles valid only on that worker. This removes atomic shared-reference traffic from normal reads and writes while retaining deterministic teardown.
- **CPU Affinity:** Workers pin themselves to CPUs in the process's allowed affinity mask by default. `TcpServerOptions::pin_workers` can disable this when affinity is controlled by an orchestrator, and `ring_queue_depth` configures the per-worker ring depth (default 256, minimum 8).
- **Handler Isolation:** Routes are inline by default. Shared-blocking and named queues are bounded and shard-local; only offloaded requests are copied into owned storage. Completed responses wake and return to the connection's owning ring through `eventfd`.
- **TCP_NODELAY:** Client sockets default to `tcp::no_delay(true)` to eliminate packet-coalescing latency on pipelined keep-alive connections.

## Configuration

```cpp
octane::HttpLimits limits;
limits.max_header_bytes = 16 * 1024;
limits.max_body_bytes = 1024 * 1024;
limits.max_connections = 4096;
limits.max_requests_per_connection = 1000;
limits.read_timeout = std::chrono::seconds(10);
limits.write_timeout = std::chrono::seconds(10);
limits.shutdown_timeout = std::chrono::seconds(5);

// Binds to port 8080 across 8 dedicated worker event loops
app.listen(8080, 8, limits);
```

These are the defaults. Header bytes include the request line and final CRLF. The read deadline covers an entire request, including its body, and also limits idle keep-alive time. Sending occasional bytes does not restart the deadline. Timeouts and incomplete socket reads close the connection. Header/body limit errors return 431/413 then close. Excess accepted connections are closed without starting a request. Limits are per connection, not a global memory budget: request storage, parsed maps, output strings, and application allocations add overhead. Application response size is not currently capped.

Each connection executes strictly within the single thread that accepted it, eliminating strand serialization bottlenecks. Input and output storage remains owned until completion; request accessors return borrowed views which handlers must not retain beyond request lifetime. Complete pipelined requests are processed in arrival order. The transport owns Content-Length and Connection response fields. HEAD responses and 204/304 statuses suppress the body. Handler or serialization exceptions produce a 500 status and close; exception details are not sent to clients. Partial writes retain the same header/body storage and advance an offset rather than rebuilding the response.

SIGINT/SIGTERM (or `TcpServer::stop()`) stop accepting, close idle connections, and let active requests, queued handlers, or responses finish. Cancellation is retried if SQ space is temporarily unavailable, and workers drain cancellation completions before exit. Worker exceptions are reported to the coordinating thread and rethrown from `listen()`. Signal ownership is intentionally process-wide: attempting to run a second signal-managed server concurrently throws instead of silently sharing shutdown state. Inline handlers cannot be preempted; register blocking work with `HandlerExecution::shared_blocking()` or a configured named queue.

## Remaining Operational Tradeoffs

- The exact process-wide connection limit performs a relaxed atomic increment/decrement on connection churn. This is a deliberate coordination cost; established-connection I/O stays worker-local.
- Inline route handlers run on ring threads. CPU-heavy or blocking handlers reduce the capacity of that worker and should use a bounded execution queue. Queue workers cannot forcibly cancel arbitrary C++ handler code, so shutdown still waits for a handler that never returns.
- Queue sizing is per shard. Total queue threads equal `ring workers * threads_per_shard` for every lazily activated queue. Saturation or an unknown named queue produces a closing `503` response instead of unbounded memory growth.
- Mapped response views must remain valid until asynchronous transmission completes. Prefer process-lifetime mappings or otherwise guarantee their ownership externally.
- Response size is application-controlled and is not capped by `HttpLimits`; deployments should enforce suitable application-level output bounds.
- The native backend requires Linux and `liburing`. Kernel, container, and seccomp policies must permit `io_uring_setup` and the operations used by the server.

## Build and Regression Tests

From the Octane directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Tests cover parser rejection, body/header boundaries, fragmented requests, pipelining, HTTP/1.0 and Connection: close, inline and offloaded exceptions, owned offloaded request views, bounded-queue saturation, missing named queues, disconnects, deadlines, slow readers, connection capacity, and graceful shutdown. Network tests need permission to bind a loopback socket. Python 3 is required when `BUILD_TESTING` is enabled. Set `BUILD_TESTING=OFF` for a library-only build.

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

## Repeatable Load Baseline

Build Release, then run the fixture with production defaults:

```bash
build/octane_server_fixture 8080
# In another terminal:
python3 benchmarks/load.py --connections 16 --requests 1000 --label 'revision/compiler/CPU'
python3 benchmarks/load.py --connections 16 --requests 1000 --path /echo --body-bytes 4096
```

The JSON output records settings, client platform, elapsed time, successes, errors, received bytes, throughput, and p50/p95/p99 successful-request latency. The script is closed-loop, includes connection establishment, and has no warmup. Run one warmup invocation and save several measured runs using identical builds, hardware, and concurrency. Python and a colocated client can bottleneck the result; this is a regression baseline, not the server's maximum capacity. Use a dedicated load generator on a separate machine for capacity claims and monitor server CPU and RSS separately. The `benchmark_smoke` CTest runs GET and 4 KiB POST workloads and validates its JSON results.

To measure raw server capacity with pipelining and persistent connections, use `wrk`:

```bash
wrk -t8 -c128 -d30s --latency http://127.0.0.1:8080/
```

## Production Deployment Checklist

- **Reverse Proxy Termination:** Front Octane with an edge reverse proxy (such as NGINX, Cloudflare, or Envoy) to terminate TLS, enforce coarse DDoS mitigation, and normalize chunked requests.
- **Origin Isolation:** Bind to loopback/private networking or use firewall allowlists or Cloudflare Tunnel. Never rely on `X-Forwarded-*` or `CF-Connecting-IP` unless untrusted clients cannot reach the origin directly.
- **Request Normalization:** Octane accepts bounded Content-Length bodies and rejects chunked request transfer coding. Keep [NGINX request buffering](https://nginx.org/en/docs/http/ngx_http_proxy_module.html#proxy_request_buffering) enabled, set an edge body limit no larger than `max_body_bytes`, and verify the deployed proxy sends a valid Content-Length upstream. For Cloudflare Tunnel, enable its [`disableChunkedEncoding`](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/configure-tunnels/origin-parameters/#disablechunkedencoding) origin option.
- **Proxy Timeouts:** Configure proxy connect/read/send timeouts consistently with Octane's read and write deadlines. The edge may be stricter, but it should not retain an origin connection substantially longer than the application intends.
- **Deadline Configuration:** Keep finite read, write, and shutdown deadlines. The ring loop enforces them with cancellation and a maximum scheduling granularity of approximately 25 ms.
- **Worker Allocation Sizing:** Start near the number of available physical CPU cores and measure. Excess workers add rings, listening sockets, memory, and scheduler contention.
- **Kernel and Limit Sizing:** Validate the deployed kernel's multishot-accept behavior, file-descriptor limits, memory budget, and `io_uring` policy. The implementation falls back to single-shot accept, but it cannot bypass a policy that disables `io_uring` entirely.
- **Blocking Work:** Keep filesystem, database, DNS, and CPU-heavy operations off ring threads unless their latency is strictly bounded.
