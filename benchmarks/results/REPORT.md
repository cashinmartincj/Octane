# Local wrk profiling results

Machine: Intel Core i7-12700H, 20 logical CPUs. GCC 16.2.1, Release
(-O3 -DNDEBUG). Client and server ran on the same machine, without CPU pinning.
Each uninstrumented endpoint test used a 3-second warmup, then:

```sh
wrk -t4 -c128 -d30s --latency http://127.0.0.1:8080/
wrk -t4 -c128 -d30s --latency http://127.0.0.1:8080/api/hello
```

The existing example process on port 8080 used 20 workers and was left running.
A temporary copy of the same example used `app.listen(8081, 4)` and the same
Release flags and limits. Its tests used port 8081; it was stopped afterward.
The project source and configured worker default were not changed.

| Endpoint | Server workers | Requests/sec | p50 | p99 | Average server CPU cores |
|---|---:|---:|---:|---:|---:|
| HTML `/` | 20 | 208,220 | 475 us | 2.44 ms | 13.98 |
| JSON `/api/hello` | 20 | 209,221 | 523 us | 2.41 ms | 13.90 |
| HTML `/` | 4 | 235,899 | 528 us | 0.833 ms | 3.34 |
| JSON `/api/hello` | 4 | 243,933 | 509 us | 0.803 ms | 3.33 |

No socket-error or non-success-status summaries were reported by wrk.
Server CPU is the delta in /proc/PID/stat user+system CPU seconds divided by
elapsed wall time; it excludes the wrk process. Raw wrk output and CPU/RSS
measurements are stored beside this report.

## Findings

- The HTML result reproduces the user's approximately 210,702 requests/sec.
- HTML and small JSON throughput are nearly equal at 20 workers despite a large
  bandwidth difference. Copying the HTML response is unlikely to dominate this
  workload. That does not establish that the copy is free or never important.
- Four workers increased HTML throughput by 13.3% and JSON throughput by 16.6%,
  while reducing server CPU consumption about 76%. p99 improved about 66%.
  HTML median latency was slightly worse with four workers.
- This supports investigating shared scheduler/executor contention and worker
  placement before changing parser validation or removing safety limits.
  Four workers are an observed improvement, not a proven optimum.

## Function-level profiling and its limits

perf was unavailable. A separate -O3 -g -pg build was exercised with 15-second
HTML and JSON runs on port 8081, then shut down cleanly to write gmon.out.
The combined gprof report is in gprof.txt. The leading reported symbols include
shared-pointer reference counting, Asio scheduler work tracking, executor copies,
and timer scheduling/cancellation. Serialization and parsing appear lower.

Treat this as directional evidence only: instrumentation reduced throughput to
about 65k requests/sec, multithreaded call counts are unreliable, optimized code
can have misleading symbol attribution, and gprof does not provide a complete
account of kernel/shared-library costs. In particular it reports implausibly
many exe_dir calls even though the example initializes its mapping once. Exact
percentages and call counts must not be treated as production cost attribution.
Hardware sampling with perf would be the next stronger measurement.

These are single runs on a shared machine, not an isolated capacity study or a
before/after comparison with the old implementation. The first baseline run may
have briefly overlapped completion of the profiler build. No exact attribution
of the historical throughput drop can be made without benchmarking the previous
revision under the same conditions.

## Next experiments

Sweep worker counts (1, 2, 4, 8, 20) with repeated runs and CPU placement controls.
Use sampling profiles to assess scheduler contention, executor/reference-count
traffic, and timer registration. Preserve timeout and cancellation semantics in
any implementation changes. Test scatter/gather file responses afterward if
response-size sweeps show copying becomes significant.
