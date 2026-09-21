"""Black-box tests of real TCP framing, deadlines, capacity, and shutdown."""
import signal
import socket
import subprocess
import sys
import time

proc = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE, text=True)
try:
    port = int(proc.stdout.readline().strip().split()[-1])
    def connect():
        s = socket.create_connection(('127.0.0.1', port), timeout=2)
        s.settimeout(3)
        return s
    def all_bytes(s):
        data = b''
        while True:
            try:
                chunk = s.recv(65536)
            except ConnectionResetError:
                break
            if not chunk:
                break
            data += chunk
        return data
    def request(raw, status):
        with connect() as s:
            s.sendall(raw)
            data = all_bytes(s)
            assert data.startswith(f'HTTP/1.1 {status} '.encode()), data
            assert b'Connection: close\r\n' in data
        time.sleep(.03)
    request(b'GET / HTTP/1.0\r\n\r\n', 200)
    request(b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n', 200)
    with connect() as s:
        for part in [b'POST /echo HT', b'TP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhe', b'llo']:
            s.sendall(part)
            time.sleep(.01)
        s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n' * 2)
        data = all_bytes(s)
        assert data.count(b'HTTP/1.1 200') == 3, data
        assert b'\r\n\r\nhelloHTTP/1.1' in data, data
    time.sleep(.03)
    request(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65\r\n\r\n', 413)
    request(b'GET / HTTP/1.1\r\nHost: x\r\nX: ' + b'a' * 300, 431)
    request(b'GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n', 400)
    request(b'GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n', 501)
    request(b'GET /throw HTTP/1.1\r\nHost: x\r\n\r\n', 500)
    with connect() as s:
        body = b'payload'
        s.sendall(
            b'POST /shared/42?value=query HTTP/1.1\r\nHost: x\r\n'
            b'X-Test: header\r\nContent-Length: 7\r\nConnection: close\r\n\r\n' + body)
        data = all_bytes(s)
        assert data.startswith(b'HTTP/1.1 200') and data.endswith(
            b'42:query:header:payload'), data
    time.sleep(.03)
    with connect() as s:
        s.sendall(
            b'POST /named/7 HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n'
            b'Connection: close\r\n\r\ndata')
        data = all_bytes(s)
        assert data.startswith(b'HTTP/1.1 200') and data.endswith(b'7:data'), data
    time.sleep(.03)
    request(b'GET /named-throw HTTP/1.1\r\nHost: x\r\n\r\n', 500)
    request(b'GET /missing-queue HTTP/1.1\r\nHost: x\r\n\r\n', 503)
    request(b'GET /close HTTP/1.1\r\nHost: x\r\n\r\n', 200)
    with connect() as s:
        s.sendall(b'HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
        data = all_bytes(s)
        assert data.endswith(b'\r\n\r\n') and b'Content-Length: 10' in data, data
    time.sleep(.03)
    request(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 64\r\nConnection: close\r\n\r\n' + b'x' * 64, 200)
    base = b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX: '
    request(base + b'x' * (256 - len(base) - 4) + b'\r\n\r\n', 200)
    # Idle, incomplete body, and slow trickle all expire at an absolute deadline.
    for initial in [b'', b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\na', b'GET /']:
        with connect() as s:
            s.sendall(initial)
            start = time.monotonic()
            if initial == b'GET /':
                for _ in range(3):
                    time.sleep(.07)
                    s.sendall(b'a')
            assert all_bytes(s) == b''
            assert time.monotonic() - start < 1.5
        time.sleep(.03)
    # Abrupt disconnect must release its slot and leave workers alive.
    with connect() as s:
        s.sendall(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\na')
    time.sleep(.05)
    request(b'GET / HTTP/1.0\r\n\r\n', 200)
    with connect() as a, connect() as b:
        time.sleep(.03)
        with connect() as extra:
            assert all_bytes(extra) == b''
    time.sleep(.05)
    # A client that never reads cannot occupy a connection indefinitely.
    with connect() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        s.sendall(b'GET /large HTTP/1.1\r\nHost: x\r\n\r\n')
        time.sleep(.8)
        # Two fresh requests should both be admitted after the writer times out.
        with connect() as a, connect() as b:
            for client in (a, b):
                client.sendall(b'GET / HTTP/1.0\r\n\r\n')
                assert all_bytes(client).startswith(b'HTTP/1.1 200')
    time.sleep(.05)
    # Shutdown drains a body already in progress, and closes idle connections.
    with connect() as active, connect() as idle:
        active.sendall(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nh')
        time.sleep(.05)
        proc.send_signal(signal.SIGTERM)
        time.sleep(.05)
        active.sendall(b'ello')
        data = all_bytes(active)
        assert data.startswith(b'HTTP/1.1 200') and data.endswith(b'hello'), data
        assert b'Connection: close' in data
        assert all_bytes(idle) == b''
    assert proc.wait(timeout=3) == 0
    # Force shutdown before the much longer read deadline.
    proc = subprocess.Popen([sys.argv[1], '0', 'drain-test'], stdout=subprocess.PIPE, text=True)
    port = int(proc.stdout.readline().strip().split()[-1])
    with connect() as active:
        active.sendall(b'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nh')
        time.sleep(.05)
        start = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        assert all_bytes(active) == b''
        assert time.monotonic() - start < 1.5
    assert proc.wait(timeout=3) == 0
finally:
    if proc.poll() is None:
        proc.kill()
        proc.wait()
