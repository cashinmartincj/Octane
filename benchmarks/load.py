"""Closed-loop HTTP/1.1 baseline; Python/client overhead limits peak throughput."""
import argparse
import concurrent.futures
import http.client
import json
import os
import platform
import time

parser = argparse.ArgumentParser()
parser.add_argument('--host', default='127.0.0.1')
parser.add_argument('--port', type=int, default=8080)
parser.add_argument('--connections', type=int, default=16)
parser.add_argument('--requests', type=int, default=1000, help='Requests per connection worker')
parser.add_argument('--path', default='/')
parser.add_argument('--body-bytes', type=int, default=0, help='POST body size; use /echo')
parser.add_argument('--label', default='', help='Build revision, compiler and machine details')
args = parser.parse_args()
if args.connections < 1 or args.requests < 1 or args.body_bytes < 0:
    parser.error('connections/requests must be positive and body-bytes nonnegative')
body = b'x' * args.body_bytes if args.body_bytes else None

def worker(_):
    client = http.client.HTTPConnection(args.host, args.port, timeout=10)
    latencies, errors, received = [], 0, 0
    try:
        for _ in range(args.requests):
            start = time.perf_counter()
            try:
                client.request('POST' if body is not None else 'GET', args.path, body=body)
                response = client.getresponse()
                data = response.read()
                received += len(data)
                if response.status != 200:
                    errors += 1
                else:
                    latencies.append((time.perf_counter() - start) * 1000)
            except (OSError, http.client.HTTPException):
                errors += 1
                client.close()
    finally:
        client.close()
    return latencies, errors, received

start = time.perf_counter()
with concurrent.futures.ThreadPoolExecutor(max_workers=args.connections) as pool:
    results = list(pool.map(worker, range(args.connections)))
elapsed = time.perf_counter() - start
latencies = sorted(x for latency, _, _ in results for x in latency)
def percentile(p):
    return latencies[min(len(latencies) - 1, int((len(latencies) - 1) * p))] if latencies else None
print(json.dumps({
    'configuration': vars(args), 'platform': platform.platform(), 'cpu_count': os.cpu_count(),
    'python': platform.python_version(), 'elapsed_seconds': elapsed,
    'successful_requests': len(latencies), 'errors': sum(x[1] for x in results),
    'response_body_bytes': sum(x[2] for x in results),
    'successful_requests_per_second': len(latencies) / elapsed,
    'latency_ms': {'p50': percentile(.50), 'p95': percentile(.95), 'p99': percentile(.99)},
    'mode': 'closed-loop; includes connection/reconnection time; no warmup',
}, indent=2))
