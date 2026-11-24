#!/usr/bin/env python3
"""Local tests for serve.py's WebSocket/TCP bridge (no browser needed)."""
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

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
    def __init__(self, port, greeting=None, close_after_greeting=False):
        super().__init__(daemon=True)
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", port))
        self.s.listen()
        self.port = port
        self.greeting = greeting
        self.close_after_greeting = close_after_greeting
        self.saw_eof = threading.Event()

    def run(self):
        while True:
            try:
                c, _ = self.s.accept()
            except OSError:
                return
            threading.Thread(target=self.echo, args=(c,), daemon=True).start()

    def echo(self, c):
        try:
            if self.greeting:
                c.sendall(self.greeting)
            if self.close_after_greeting:
                c.shutdown(socket.SHUT_WR)
                c.close()
                return
            while True:
                d = c.recv(65536)
                if not d:
                    self.saw_eof.set()
                    break
                c.sendall(d)
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass


def wait_port(port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def start_server(allow, extra=()):
    port = freeport()
    args = [sys.executable, os.path.join(HERE, "serve.py"), "--verbose",
            "--port", str(port)]
    for a in allow:
        args += ["--allow", a]
    args += list(extra)
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if not wait_port(port):
        proc.kill()
        raise RuntimeError("server did not start")
    return proc, port


def stop_server(proc):
    proc.terminate()
    try:
        out, err = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    return out, err


def masked_frame(data, opcode=0x2, fin=True):
    if isinstance(data, str):
        data = data.encode()
    n = len(data)
    mask = b"abcd"
    b0 = (0x80 if fin else 0) | opcode
    if n < 126:
        h = bytes((b0, 0x80 | n))
    elif n < 65536:
        h = bytes((b0, 0x80 | 126)) + struct.pack("!H", n)
    else:
        h = bytes((b0, 0x80 | 127)) + struct.pack("!Q", n)
    return h + mask + bytes(c ^ mask[i & 3] for i, c in enumerate(data))


def recv_bytes(ws, n):
    data = b""
    while len(data) < n:
        op, got = ws.recv()
        if op != 0x2:
            raise WSError(f"wanted binary, got opcode {op}")
        data += got
    return data


def handshake(port, path, origin, method="GET", version="13",
              key="dGhlIHNhbXBsZSBub25jZQ==", upgrade=True,
              connection=True, host=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    lines = [f"{method} {path} HTTP/1.1",
             f"Host: {host or f'127.0.0.1:{port}'}"]
    if upgrade:
        lines.append("Upgrade: websocket")
    if connection:
        lines.append("Connection: Upgrade")
    lines += [f"Sec-WebSocket-Key: {key}",
              f"Sec-WebSocket-Version: {version}"]
    if origin is not None:
        lines.append(f"Origin: {origin}")
    s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
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
    head, _, rest = data.partition(b"\r\n\r\n")
    return s, head.split(b"\r\n", 1)[0].decode("iso-8859-1", "replace"), rest


def raw_status(port, path, origin, **kw):
    s, line, _ = handshake(port, path, origin, **kw)
    s.close()
    return line


def main():
    echo_port = freeport()
    echo = Echo(echo_port)
    echo.start()
    allow = [f"17019=127.0.0.1:{echo_port}", f"567=127.0.0.1:{echo_port}"]
    srv, bridge_port = start_server(allow)
    origin = f"http://127.0.0.1:{bridge_port}"
    secret = b"auth-secret-" + os.urandom(8).hex().encode()
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{bridge_port}/drawterm.json", timeout=5) as r:
            cfg = json.load(r)
        if cfg["cpu"]["host"] == "127.0.0.1" and cfg["auth"]["logical"] == 567:
            ok("gateway publishes its configured CPU/auth mappings")
        else:
            bad(f"gateway config: {cfg!r}")

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

        ws.sock.sendall(masked_frame(b"frag", fin=False))
        ws.sock.sendall(masked_frame(b"mented", fin=True, opcode=0x0))
        if recv_bytes(ws, 10) == b"fragmented":
            ok("bridge reassembles fragmented binary messages")
        else:
            bad("fragmented message was not reassembled")

        ws.sock.sendall(masked_frame("text", opcode=0x1))
        op, _ = ws.recv()
        if op == 0x8:
            ok("text messages are refused with a WebSocket close")
        else:
            bad(f"text message got opcode {op}")
        ws.close()

        # a client close half-closes upstream, which the echo server reports
        echo.saw_eof.clear()
        ws = WSClient(f"ws://127.0.0.1:{bridge_port}/ws/tcp/17019", origin)
        ws.send(b"before-close")
        recv_bytes(ws, len(b"before-close"))
        ws.send_close()
        op, _ = ws.recv()
        if op == 0x8 and echo.saw_eof.wait(5):
            ok("client close reaches upstream and returns a close")
        else:
            bad("client close was not propagated to upstream")
        ws.close()

        line = raw_status(bridge_port, "/ws/tcp/9999", origin)
        if " 403 " in line:
            ok("disallowed service is refused")
        else:
            bad("disallowed service response: " + line)
        line = raw_status(bridge_port, "/ws/tcp/17019", "http://evil.example")
        if " 403 " in line:
            ok("foreign origin is refused")
        else:
            bad("foreign origin response: " + line)
        line = raw_status(bridge_port, "/ws/tcp/17019", None)
        if " 403 " in line:
            ok("missing origin is refused")
        else:
            bad("missing origin response: " + line)
        line = raw_status(bridge_port, "/ws/tcp/17019", origin, method="POST")
        if " 405 " in line:
            ok("non-GET WebSocket request is refused")
        else:
            bad("POST response: " + line)
        line = raw_status(bridge_port, "/ws/tcp/17019", origin, upgrade=False)
        if " 426 " in line:
            ok("request without Upgrade is refused")
        else:
            bad("no-upgrade response: " + line)
        line = raw_status(bridge_port, "/ws/tcp/17019", origin, version="12")
        if " 400 " in line:
            ok("wrong WebSocket version is refused")
        else:
            bad("version response: " + line)
        line = raw_status(bridge_port, "/ws/tcp/17019", origin, key="short")
        if " 400 " in line:
            ok("malformed WebSocket key is refused")
        else:
            bad("key response: " + line)

        # a frame larger than the gateway's message limit closes 1009 without
        # waiting for the payload
        ws = WSClient(f"ws://127.0.0.1:{bridge_port}/ws/tcp/17019", origin)
        ws.sock.sendall(bytes((0x82, 0x80 | 127)) + struct.pack("!Q", 8 * 1024 * 1024 + 1) + b"abcd")
        op, payload = ws.recv()
        if op == 0x8 and len(payload) >= 2 and struct.unpack("!H", payload[:2])[0] == 1009:
            ok("oversized message closes with 1009")
        else:
            bad(f"oversized message got opcode {op} payload {payload!r}")
        ws.close()
    finally:
        out, err = stop_server(srv)
        if secret in out or secret in err:
            bad("gateway logged payload bytes")
        else:
            ok("gateway does not log payload bytes")
        echo.s.close()

    # upstream connection failure is an HTTP error before any upgrade
    deadport = freeport()
    srv, bridge_port = start_server(
        [f"17019=127.0.0.1:{deadport}", f"567=127.0.0.1:{deadport}"])
    try:
        s, line, _ = handshake(bridge_port, "/ws/tcp/17019",
                               f"http://127.0.0.1:{bridge_port}")
        s.close()
        if " 502 " in line:
            ok("failed upstream connect returns HTTP 502 before upgrade")
        else:
            bad("upstream failure response: " + line)
    finally:
        stop_server(srv)

    # upstream data then EOF is delivered before the close
    greet_port = freeport()
    greet = Echo(greet_port, greeting=b"hello-then-eof", close_after_greeting=True)
    greet.start()
    srv, bridge_port = start_server(
        [f"17019=127.0.0.1:{greet_port}", f"567=127.0.0.1:{greet_port}"])
    try:
        ws = WSClient(f"ws://127.0.0.1:{bridge_port}/ws/tcp/17019",
                      f"http://127.0.0.1:{bridge_port}")
        data = b""
        while len(data) < len(b"hello-then-eof"):
            op, part = ws.recv()
            if op != 0x2:
                break
            data += part
        op, _ = ws.recv()
        if data == b"hello-then-eof" and op == 0x8:
            ok("upstream EOF delivers buffered bytes before the close")
        else:
            bad(f"upstream EOF: data={data!r} opcode={op}")
        ws.close()
    finally:
        stop_server(srv)
        greet.s.close()

    # per-peer connection limit is enforced before an upgrade
    limit_port = freeport()
    limit = Echo(limit_port)
    limit.start()
    srv, bridge_port = start_server(
        [f"17019=127.0.0.1:{limit_port}", f"567=127.0.0.1:{limit_port}"],
        ["--max-per-ip", "1"])
    try:
        origin = f"http://127.0.0.1:{bridge_port}"
        held = WSClient(f"ws://127.0.0.1:{bridge_port}/ws/tcp/17019", origin)
        line = raw_status(bridge_port, "/ws/tcp/17019", origin)
        if " 503 " in line:
            ok("per-peer connection limit is enforced")
        else:
            bad("connection limit response: " + line)
        held.close()
    finally:
        stop_server(srv)
        limit.s.close()

    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
