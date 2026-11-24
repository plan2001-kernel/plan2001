"""Minimal RFC 6455 client for the local bridge/browser tests.

No third-party packages are needed; this is not used by the served page.
"""
import base64
import hashlib
import os
import socket
import struct
import urllib.parse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WSError(Exception):
    pass


def _recv_exact(sock, n):
    b = bytearray()
    while len(b) < n:
        d = sock.recv(n - len(b))
        if not d:
            raise WSError("unexpected EOF")
        b += d
    return bytes(b)


class WSClient:
    def __init__(self, url, origin, timeout=10):
        u = urllib.parse.urlsplit(url)
        if u.scheme != "ws":
            raise WSError("only ws:// in tests")
        host = u.hostname
        port = u.port or 80
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = u.path or "/"
        if u.query:
            path += "?" + u.query
        hosth = host if u.port is None else f"{host}:{port}"
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {hosth}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Origin: {origin}\r\n"
            "\r\n"
        ).encode()
        self.sock.sendall(req)
        head = bytearray()
        while b"\r\n\r\n" not in head:
            d = self.sock.recv(4096)
            if not d:
                raise WSError("EOF in handshake")
            head += d
        text, self.rest = head.split(b"\r\n\r\n", 1)
        lines = text.decode("iso-8859-1").split("\r\n")
        if " 101 " not in lines[0]:
            raise WSError("handshake: " + lines[0])
        hdrs = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                hdrs[k.strip().lower()] = v.strip()
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if hdrs.get("sec-websocket-accept") != want:
            raise WSError("bad Sec-WebSocket-Accept")
        self.buf = bytearray(self.rest)
        self.closed = False

    def _read(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(65536)
            if not d:
                raise WSError("unexpected EOF")
            self.buf += d
        b = bytes(self.buf[:n])
        del self.buf[:n]
        return b

    def send(self, payload, opcode=0x2):
        if isinstance(payload, str):
            payload = payload.encode()
            opcode = 0x1
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            h = bytes((0x80 | opcode, 0x80 | n))
        elif n < 65536:
            h = bytes((0x80 | opcode, 0x80 | 126)) + struct.pack("!H", n)
        else:
            h = bytes((0x80 | opcode, 0x80 | 127)) + struct.pack("!Q", n)
        self.sock.sendall(h + mask + bytes(c ^ mask[i & 3] for i, c in enumerate(payload)))

    def send_close(self, code=1000):
        self.send(struct.pack("!H", code), opcode=0x8)

    def recv(self):
        msg = bytearray()
        mop = None
        while True:
            b = self._read(2)
            fin = bool(b[0] & 0x80)
            opcode = b[0] & 0x0F
            if b[1] & 0x80:
                raise WSError("masked server frame")
            n = b[1] & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read(8))[0]
            payload = self._read(n)
            if opcode == 0x9:
                self.send(payload, opcode=0xA)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                self.closed = True
                return 0x8, payload
            if opcode in (0x1, 0x2):
                mop = opcode
                msg = bytearray(payload)
            elif opcode == 0x0:
                msg += payload
            else:
                raise WSError(f"opcode {opcode}")
            if fin:
                return mop, bytes(msg)

    def close(self):
        try:
            self.send_close()
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
