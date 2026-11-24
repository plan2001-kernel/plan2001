#!/usr/bin/env python3
"""Local tests for serve.py's WebSocket/TCP bridge (no browser needed)."""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

from wsclient import WSClient, WSError

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = 0


def ok(t):
    print("ok: " + t)


def bad(t):
    global FAILS
    FAILS += 1
    print("FAIL: " + t, file=sys.stderr)


def freeport():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Echo(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", port))
        self.s.listen()
        self.port = port

    def run(self):
        while True:
            try:
                c, _ = self.s.accept()
            except OSError:
                return
            threading.Thread(target=self.echo, args=(c,), daemon=True).start()

    def echo(self, c):
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    break
                c.sendall(d)
        except OSError:
            pass
        finally:
            c.close()


def wait_port(port):
    for _ in range(100):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def masked_frame(data, opcode=0x2):
    n = len(data)
    mask = b"abcd"
    if n < 126:
        h = bytes((0x80 | opcode, 0x80 | n))
    elif n < 65536:
        h = bytes((0x80 | opcode, 0x80 | 126)) + struct.pack("!H", n)
    else:
        h = bytes((0x80 | opcode, 0x80 | 127)) + struct.pack("!Q", n)
    return h + mask + bytes(c ^ mask[i & 3] for i, c in enumerate(data))


def recv_bytes(ws, n):
    data = b""
    while len(data) < n:
        op, got = ws.recv()
        if op != 0x2:
            raise WSError(f"wanted binary, got opcode {op}")
        data += got
    return data


def raw_http(port, path, origin):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall((
        f"GET {path} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Origin: {origin}\r\n\r\n"
    ).encode())
    data = b""
    s.settimeout(5)
    try:
        while b"\r\n\r\n" not in data:
            d = s.recv(4096)
            if not d:
                break
            data += d
    except OSError:
        pass
    s.close()
    return data.split(b"\r\n", 1)[0].decode("iso-8859-1", "replace")


def main():
    echo_port = freeport()
    bridge_port = freeport()
    echo = Echo(echo_port)
    echo.start()
    proc = subprocess.Popen([
        sys.executable, os.path.join(HERE, "serve.py"), "--verbose",
        "--port", str(bridge_port),
        "--allow", f"17019=127.0.0.1:{echo_port}",
        "--allow", f"567=127.0.0.1:{echo_port}",
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    origin = f"http://127.0.0.1:{bridge_port}"
    secret = b"auth-secret-" + os.urandom(8).hex().encode()
    try:
        if not wait_port(bridge_port):
            bad("bridge did not start")
            return 1
        ws = WSClient(f"ws://127.0.0.1:{bridge_port}/ws/tcp/17019", origin)
        ws.send(secret)
        if recv_bytes(ws, len(secret)) == secret:
            ok("gateway relays opaque bytes")
        else:
            bad("gateway did not relay opaque bytes")
        for payload in (b"hello", b"a" * 125, b"b" * 126, b"c" * 70000):
            ws.send(payload)
            got = recv_bytes(ws, len(payload))
            if got == payload:
                ok(f"bridge echoes {len(payload)} binary bytes")
            else:
                bad(f"bridge echo of {len(payload)} bytes")
                break
        frame = masked_frame(b"partial" * 1000)
        ws.sock.sendall(frame[:1])
        time.sleep(0.05)
        ws.sock.sendall(frame[1:3])
        time.sleep(0.05)
        ws.sock.sendall(frame[3:9])
        time.sleep(0.05)
        ws.sock.sendall(frame[9:])
        got = recv_bytes(ws, len(b"partial" * 1000))
        if got == b"partial" * 1000:
            ok("bridge assembles a frame split across TCP writes")
        else:
            bad("partial frame was not reassembled")
        ws.send_close()
        op, _ = ws.recv()
        if op == 0x8:
            ok("bridge echoes close")
        else:
            bad("bridge did not echo close")
        ws.close()

        line = raw_http(bridge_port, "/ws/tcp/9999", origin)
        if " 403 " in line:
            ok("disallowed service is refused")
        else:
            bad("disallowed service response: " + line)
        line = raw_http(bridge_port, "/ws/tcp/17019", "http://evil.example")
        if " 403 " in line:
            ok("foreign origin is refused")
        else:
            bad("foreign origin response: " + line)
        line = raw_http(bridge_port, "/ws/tcp/17019", "")
        if " 403 " in line:
            ok("missing origin is refused")
        else:
            bad("missing origin response: " + line)
    finally:
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
        if secret in out or secret in err:
            bad("gateway logged payload bytes")
        else:
            ok("gateway does not log payload bytes")
        echo.s.close()
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
