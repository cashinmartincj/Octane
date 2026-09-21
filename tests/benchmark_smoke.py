"""Validate the benchmark against the real server; print a reproducible baseline."""
import json
from pathlib import Path
import signal
import subprocess
import sys

server = subprocess.Popen([sys.argv[1], '0'], stdout=subprocess.PIPE, text=True)
try:
    port = int(server.stdout.readline().split()[-1])
    for path, body in [('/', 0), ('/echo', 4096)]:
        result = subprocess.run([
            sys.executable, str(Path(__file__).resolve().parents[1] / 'benchmarks/load.py'),
            '--port', str(port), '--connections', '4', '--requests', '500',
            '--path', path, '--body-bytes', str(body), '--label', 'CTest smoke baseline',
        ], check=True, capture_output=True, text=True, timeout=30)
        data = json.loads(result.stdout)
        assert data['errors'] == 0 and data['successful_requests'] == 2000, data
        print(result.stdout)
    server.send_signal(signal.SIGTERM)
    assert server.wait(timeout=10) == 0
finally:
    if server.poll() is None:
        server.kill()
        server.wait()
