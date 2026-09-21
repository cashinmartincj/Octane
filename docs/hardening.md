# HTTP Transport Limits and Verification

Octane supports high-throughput, bounded Content-Length requests over HTTP/1.0 and HTTP/1.1.
It is an optimized, low-latency microframework designed around a modern thread-per-core asynchronous execution architecture.
The framing policy follows [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html): ambiguous framing is rejected and the connection is closed. This implementation conservatively rejects all duplicate Content-Length fields, even identical ones. Transfer-Encoding is currently rejected with 501 (400 when combined with Content-Length). Expect requests receive 417; chunked decoding and 100-continue are future work. Only origin-form targets and `OPTIONS *` are supported. Host presence, duplicates, and unsafe delimiters are checked; full URI authority validation and proxy absolute-form handling are not implemented.

## Architecture and Execution Model

- **Thread-per-Core Execution:** The server provisions an independent `asio::io_context` and dedicated event loop for each worker thread.
- **Kernel Load Balancing (`SO_REUSEPORT`):** On Linux platforms supporting `SO_REUSEPORT`, each worker binds and accepts on its own socket descriptor, allowing the operating system kernel to distribute incoming connections across cores without cross-thread lock contention or strand scheduling overhead.
- **Contiguous Rolling Ring Buffer:** Each connection manages reads inside an allocated contiguous buffer using active window indices (`read_pos_` and `write_pos_`), avoiding repeated `memmove` operations and string reallocation during pipelined request streams.
- **Zero-Copy Scatter-Gather I/O:** Serialized response headers and payload buffers are passed to `asio::async_write` as vectorized buffer lists (`std::array<asio::const_buffer, 2>`). The kernel flushes headers and file/memory payloads via `writev` without copying them into an intermediate buffer.
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

Each connection executes strictly within the single thread that accepted it, eliminating strand serialization bottlenecks. Input and output storage remains owned until completion; request accessors return borrowed views which handlers must not retain beyond request lifetime. Complete pipelined requests are processed in arrival order. The transport owns Content-Length and Connection response fields. HEAD responses and 204/304 statuses suppress the body. User-provided response headers cannot inject CR/LF or override framing. Handler or serialization exceptions produce a 500 status and close; exception details are not sent to clients.

SIGINT/SIGTERM (or `TcpServer::stop()`) stop accepting, close idle/header-reading connections, and let requests already reading bodies or writing responses finish. Remaining connections are cancelled after the shutdown deadline. Worker threads join after cancellation callbacks drain. Application handlers still execute synchronously: a blocking handler cannot be preempted and can delay request completion. Offload blocking work to dedicated background queues.

## Build and Regression Tests

From the Octane directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Tests cover parser rejection, body/header boundaries, fragmented requests, pipelining, HTTP/1.0 and Connection: close, exceptions, disconnects, idle/body and trickle deadlines, slow readers, connection capacity, and graceful shutdown. Network tests need permission to bind a loopback socket. Python 3 is required when `BUILD_TESTING` is enabled. Set `BUILD_TESTING=OFF` for a library-only build.

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
- **Idle Connection Sweeper:** When benchmarking timers are bypassed for maximum throughput, maintain an active periodic idle sweep to prune slow-loris connections that never complete headers.
- **Worker Allocation Sizing:** Match the worker count in `app.listen()` to the available physical CPU cores (`std::thread::hardware_concurrency()`) to ensure zero context-switching overhead across event loops.
