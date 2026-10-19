"""WebSocket adversarial loopback tests. Run once per OCTANE_TRANSPORT backend."""
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def masked(payload=b'', opcode=1, fin=True):
    key = b'\x12\x34\x56\x78'
    header = bytes([(0x80 if fin else 0) | opcode])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    elif len(payload) < 65536:
        header += b'\xfe' + struct.pack('!H', len(payload))
    else:
        header += b'\xff' + struct.pack('!Q', len(payload))
    return header + key + bytes(byte ^ key[i % 4] for i, byte in enumerate(payload))


with tempfile.TemporaryFile(mode='w+') as errors:
    proc = subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE, stderr=errors, text=True)
    try:
        assert select.select([proc.stdout], [], [], 5)[0], 'Server startup timed out'
        line = proc.stdout.readline().strip()
        assert 'listening on port' in line, line
        port = int(line.split()[-1])

        def connect():
            return socket.create_connection(('127.0.0.1', port), timeout=2)

        def exact(sock, count):
            result = b''
            while len(result) < count:
                chunk = sock.recv(count - len(result))
                assert chunk, ('Unexpected EOF', result, count)
                result += chunk
            return result

        def frame(sock):
            header = exact(sock, 2)
            assert not header[1] & 0x80, header
            size = header[1] & 0x7f
            if size == 126:
                size = struct.unpack('!H', exact(sock, 2))[0]
            elif size == 127:
                size = struct.unpack('!Q', exact(sock, 8))[0]
            return header[0], exact(sock, size)

        def headers(sock):
            result = b''
            while not result.endswith(b'\r\n\r\n'):
                result += exact(sock, 1)
                assert len(result) < 16384
            return result

        def upgrade(path='/ws', extra=b'', status=101, key=b'dGhlIHNhbXBsZSBub25jZQ==', tail=b''):
            sock = connect()
            sock.sendall(b'GET ' + path.encode() + b' HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                         b'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ' + key + b'\r\n' + extra + b'\r\n' + tail)
            response = headers(sock)
            assert response.startswith(f'HTTP/1.1 {status} '.encode()), response
            if status == 101:
                assert b'Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=' in response
            else:
                sock.close()
                return None
            return sock

        def eof(sock):
            try:
                assert sock.recv(1) == b'', 'Connection did not close'
            except ConnectionResetError:
                pass

        def rejected(data, code):
            with upgrade() as sock:
                sock.sendall(data)
                opcode, payload = frame(sock)
                assert opcode == 0x88 and struct.unpack('!H', payload[:2])[0] == code, (opcode, payload)
                eof(sock)

        # Owner-thread dispatch must wake an idle socket without a client frame.
        with upgrade('/ws/async', extra=b'X-Connect: context\r\n') as sock:
            assert frame(sock) == (0x81, b'server push')
        # Disconnected handles must safely expire while the producer is running.
        for _ in range(3):
            upgrade('/ws/async', extra=b'X-Connect: context\r\n').close()

        upgrade(key=b'invalid', status=400)
        upgrade(extra=b'Sec-WebSocket-Key: abc\r\n', status=400)
        upgrade(extra=b'Sec-WebSocket-Version: 12\r\n', status=400)
        upgrade(extra=b'Origin: https://evil.example\r\n', status=403)
        upgrade(extra=b'Origin: \r\n', status=403)
        upgrade('/ws/private', status=403)
        upgrade('/ws/private', extra=b'Origin: https://app.example\r\n', status=403)
        upgrade('/ws/throw-auth', extra=b'Origin: https://app.example\r\n', status=500)
        with upgrade('/ws/private', extra=b'Origin: https://app.example\r\nAuthorization: Bearer test\r\n') as sock:
            sock.sendall(masked(b'authenticated'))
            assert frame(sock) == (0x81, b'authenticated')
        with upgrade(extra=b'Origin: https://app.example\r\n') as sock:
            sock.sendall(masked(b'allowed'))
            assert frame(sock) == (0x81, b'allowed')

        # Upgrade and a frame in the same TCP write must preserve buffered bytes.
        with upgrade(tail=masked(b'coalesced')) as sock:
            assert frame(sock) == (0x81, b'coalesced')
            for payload in (b'hello', b'x' * 126, b'x' * 1024):
                sock.sendall(masked(payload))
                assert frame(sock) == (0x81, payload)
            sock.sendall(masked(b'\xff\x00', opcode=2))
            assert frame(sock) == (0x82, b'\xff\x00')
            sock.sendall(masked(b'\xe2', fin=False) + masked(b'ping', opcode=9) + masked(b'\x82\xac', opcode=0))
            assert frame(sock) == (0x8a, b'ping')
            assert frame(sock) == (0x81, b'\xe2\x82\xac')
            sock.sendall(masked(struct.pack('!H', 1000), opcode=8))
            assert frame(sock) == (0x88, struct.pack('!H', 1000))
            eof(sock)

        rejected(b'\x81\x01x', 1002)  # Unmasked
        rejected(b'\xc1\x80', 1002)  # Unsupported extension bit
        rejected(b'\x83\x80', 1002)  # Unknown opcode
        rejected(b'\x09\x80', 1002)  # Fragmented Ping
        rejected(b'\x89\xfe', 1002)  # Oversized Ping, header alone
        rejected(b'\x81\xfe\x00\x01', 1002)  # Non-minimal length
        rejected(b'\x81\xff\x80\x00\x00\x00\x00\x00\x00\x00', 1002)
        rejected(b'\x81\xfe\x04\x01\x00\x00\x00\x00', 1009)  # Oversize, header alone
        rejected(masked(b'unexpected', opcode=0), 1002)
        rejected(masked(b'a', fin=False) + masked(b'b'), 1002)
        rejected(masked(b'x' * 1024, fin=False) + masked(b'x', opcode=0), 1009)
        rejected(masked(b'\xc0\xaf'), 1007)
        rejected(masked(b'x', opcode=8), 1002)
        rejected(masked(struct.pack('!H', 1005), opcode=8), 1002)
        rejected(masked(struct.pack('!H', 1000) + b'\xff', opcode=8), 1007)
        rejected(masked(b'throw'), 1011)

        for opcode in (9, 10):
            with upgrade('/ws/throw-control') as sock:
                sock.sendall(masked(b'x', opcode=opcode))
                header, payload = frame(sock)
                assert header == 0x88 and payload[:2] == struct.pack('!H', 1011)
                eof(sock)
        with upgrade('/ws/throw-on-open') as sock:
            header, payload = frame(sock)
            assert header == 0x88 and payload[:2] == struct.pack('!H', 1011)
            eof(sock)
        with upgrade('/ws/open') as sock:
            assert frame(sock) == (0x81, b'hello')
        with upgrade() as sock:
            sock.sendall(masked(b'', opcode=8))
            assert frame(sock) == (0x88, b'')
            eof(sock)
        with upgrade() as sock:
            sock.sendall(masked(b'first') + masked(b'second') + masked(struct.pack('!H', 1000), opcode=8))
            assert frame(sock) == (0x81, b'first')
            assert frame(sock) == (0x81, b'second')
            assert frame(sock) == (0x88, struct.pack('!H', 1000))
            eof(sock)

        # Server-initiated Close waits for the peer, bounded by close_timeout.
        with upgrade() as sock:
            sock.sendall(masked(b'close'))
            assert frame(sock) == (0x88, struct.pack('!H', 1000) + b'bye')
            sock.sendall(masked(struct.pack('!H', 1000), opcode=8))
            eof(sock)
        with upgrade('/ws/close-on-open') as sock:
            assert frame(sock) == (0x88, struct.pack('!H', 1000))
            start = time.monotonic()
            eof(sock)
            assert time.monotonic() - start < 1

        # Idle and trickled partial frames/messages cannot retain a slot forever.
        for initial in (b'', b'\x81\x85\x00', masked(b'a', fin=False)):
            with upgrade() as sock:
                sock.sendall(initial)
                start = time.monotonic()
                if initial == b'\x81\x85\x00':
                    for _ in range(3):
                        time.sleep(.07)
                        sock.sendall(b'\x00')
                eof(sock)
                assert time.monotonic() - start < 1
        with upgrade() as sock:
            sock.sendall(masked(b'a', fin=False))
            for _ in range(3):
                time.sleep(.07)
                sock.sendall(masked(b'ping', opcode=9))
                assert frame(sock) == (0x8a, b'ping')
            eof(sock)

        # A callback generating many responses hits the bounded output queue.
        with upgrade() as sock:
            sock.sendall(masked(b'flood'))
            received = b''
            try:
                while chunk := sock.recv(65536):
                    received += chunk
            except ConnectionResetError:
                pass
            assert len(received) <= 2048, len(received)

        # A peer that stops reading must hit the write deadline, independently
        # of its longer read timeout. Check server-side closure without draining
        # the peer's already-buffered bytes.
        with upgrade('/ws/slow-reader') as sock:
            time.sleep(.6)
            with connect() as stats:
                stats.sendall(b'GET /stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                headers(stats)
                body = b''
                while chunk := stats.recv(1024):
                    body += chunk
                opened, closed = map(int, body.split(b':'))
                assert opened == closed, ('Slow writer still alive', opened, closed)

        # Repeated abrupt disconnects release slots and trigger on_close once.
        for _ in range(40):
            with upgrade() as sock:
                sock.sendall(masked(b'bye'))
                assert frame(sock) == (0x81, b'bye')
        time.sleep(.1)
        with connect() as sock:
            sock.sendall(b'GET /stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            response = headers(sock)
            body = b''
            while chunk := sock.recv(1024):
                body += chunk
            opened, closed = map(int, body.split(b':'))
            assert opened == closed, (opened, closed)
        with connect() as sock:
            sock.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
            assert headers(sock).startswith(b'HTTP/1.1 200')

        # Shutdown with an active WebSocket must terminate within the server deadline.
        with upgrade() as sock:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=3)
        assert proc.returncode == 0, proc.returncode
        print(f'WebSocket transport hardening passed: {line}')
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        errors.seek(0)
        diagnostics = errors.read()
        if diagnostics:
            print(diagnostics, file=sys.stderr)
        assert proc.returncode == 0, f'Server failed ({proc.returncode}): {diagnostics}'
        assert 'Sanitizer' not in diagnostics, diagnostics
