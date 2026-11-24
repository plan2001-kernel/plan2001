#!/usr/bin/env python3
"""Serve the browser Drawterm page and its allowlisted WebSocket/TCP bridge.

The page is static; /ws/tcp/PORT upgrades to a binary WebSocket whose bytes
are passed unchanged to the one host:port configured for that logical port.
No host or port comes from the browser: only the configured map is reachable.
The same origin serves the page so the page can use wss:// without CORS, and
COOP/COEP make SharedArrayBuffer available for drawterm's pthreads.

    serve.py --allow 17019=127.0.0.1:17019 --allow 567=127.0.0.1:567
    serve.py --cert cert.pem --key key.pem --allow 17019=cpu.example:17019 ...

Plain HTTP is refused on a non-loopback listen address unless --insecure:
real connections must use TLS (directly with --cert/--key, or in front of a
loopback gateway such as a TLS reverse proxy).  Origin is the page's own
origin unless --origin names the exact allowed origin.
"""
import argparse
import asyncio
import base64
import hashlib
import ipaddress
import os
import ssl
import struct
import sys
import urllib.parse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAXHEADER = 16 * 1024
MAXMSG = 8 * 1024 * 1024


class BadRequest(Exception):
    pass


class Closed(Exception):
    pass


def parse_allow(items):
    allow = {}
    for item in items:
        try:
            logical, target = item.split("=", 1)
            logical = int(logical, 10)
            if not 1 <= logical <= 65535:
                raise ValueError
            if target.startswith("["):
                host, port = target.rsplit(":", 1)
                host = host[1:-1]
            else:
                host, port = target.rsplit(":", 1)
            port = int(port, 10)
            if not host or not 1 <= port <= 65535:
                raise ValueError
        except ValueError:
            sys.exit(f"serve.py: bad --allow {item!r}; want PORT=HOST:PORT")
        allow[logical] = (host, port)
    return allow


def is_loopback(host):
    if host in ("localhost", ""):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def read_headers(block):
    lines = block.decode("iso-8859-1").split("\r\n")
    try:
        method, target, version = lines[0].split(" ", 2)
    except ValueError:
        raise BadRequest("bad request line")
    if version not in ("HTTP/1.1", "HTTP/1.0"):
        raise BadRequest("bad HTTP version")
    headers = {}
    for line in lines[1:]:
        if not line:
            continue
        if ":" not in line:
            raise BadRequest("bad header")
        name, value = line.split(":", 1)
        name = name.strip().lower()
        if name in headers:
            headers[name] += ", " + value.strip()
        else:
            headers[name] = value.strip()
    return method, target, headers


def response(status, reason, headers=(), body=b"", head=False):
    h = [f"HTTP/1.1 {status} {reason}",
         *[f"{k}: {v}" for k, v in headers],
         "Cross-Origin-Opener-Policy: same-origin",
         "Cross-Origin-Embedder-Policy: require-corp",
         "Cross-Origin-Resource-Policy: same-origin",
         "Cache-Control: no-cache",
         "X-Content-Type-Options: nosniff",
         "Connection: close",
         f"Content-Length: {len(body)}",
         "", ""]
    return ("\r\n".join(h)).encode("iso-8859-1") + (b"" if head else body)


def same_origin_ok(origin, host, tls):
    if not origin or not host:
        return False
    u = urllib.parse.urlsplit(origin)
    if u.scheme not in ("http", "https"):
        return False
    if tls and u.scheme != "https":
        return False
    if not tls and u.scheme != "http":
        return False
    return u.netloc.lower() == host.lower()


async def send_ws(writer, lock, opcode, payload=b""):
    n = len(payload)
    if n < 126:
        header = bytes((0x80 | opcode, n))
    elif n < 65536:
        header = bytes((0x80 | opcode, 126)) + struct.pack("!H", n)
    else:
        header = bytes((0x80 | opcode, 127)) + struct.pack("!Q", n)
    async with lock:
        if writer.is_closing():
            raise Closed
        writer.write(header + payload)
        await writer.drain()


async def send_close(writer, lock, code=1000, reason=b""):
    try:
        await send_ws(writer, lock, 0x8, struct.pack("!H", code) + reason)
    except (Closed, ConnectionError):
        pass


async def recv_frame(reader):
    try:
        b = await reader.readexactly(2)
    except (asyncio.IncompleteReadError, ConnectionError):
        raise Closed
    fin = bool(b[0] & 0x80)
    if b[0] & 0x70:
        raise BadRequest("reserved bits")
    opcode = b[0] & 0x0F
    if not b[1] & 0x80:
        raise BadRequest("unmasked client frame")
    n = b[1] & 0x7F
    if n == 126:
        n = struct.unpack("!H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", await reader.readexactly(8))[0]
        if n >> 63:
            raise BadRequest("bad frame length")
    if opcode >= 0x8 and (not fin or n > 125):
        raise BadRequest("bad control frame")
    if n > MAXMSG:
        raise BadRequest("frame too large")
    mask = await reader.readexactly(4)
    p = bytearray(await reader.readexactly(n))
    for i in range(n):
        p[i] ^= mask[i & 3]
    return fin, opcode, bytes(p)


async def ws_to_tcp(reader, wswriter, wslock, upwriter):
    msg = bytearray()
    fragmented = False
    try:
        while True:
            fin, opcode, payload = await recv_frame(reader)
            if opcode == 0x8:
                await send_close(wswriter, wslock, 1000)
                return
            if opcode == 0x9:
                await send_ws(wswriter, wslock, 0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x1:
                await send_close(wswriter, wslock, 1003, b"binary only")
                return
            if opcode == 0x2:
                if fragmented:
                    await send_close(wswriter, wslock, 1002, b"bad continuation")
                    return
                fragmented = True
                msg = bytearray()
            elif opcode == 0x0:
                if not fragmented:
                    await send_close(wswriter, wslock, 1002, b"unexpected continuation")
                    return
            else:
                await send_close(wswriter, wslock, 1002, b"bad opcode")
                return
            msg += payload
            if len(msg) > MAXMSG:
                await send_close(wswriter, wslock, 1009, b"message too large")
                return
            if fin:
                if msg:
                    upwriter.write(msg)
                    await upwriter.drain()
                msg = bytearray()
                fragmented = False
    except BadRequest as e:
        await send_close(wswriter, wslock, 1002, str(e).encode()[:120])
    except (Closed, ConnectionError, asyncio.IncompleteReadError):
        pass
    finally:
        upwriter.close()


async def tcp_to_ws(upreader, wswriter, wslock):
    try:
        while True:
            data = await upreader.read(65536)
            if not data:
                break
            await send_ws(wswriter, wslock, 0x2, data)
        await send_close(wswriter, wslock, 1000)
    except (Closed, ConnectionError):
        pass
    finally:
        wswriter.close()


async def bridge(reader, writer, host, port):
    try:
        upreader, upwriter = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=10)
    except (OSError, asyncio.TimeoutError):
        writer.write(response(502, "Bad Gateway", [("Content-Type", "text/plain")],
                             b"upstream connection failed\n"))
        await writer.drain()
        return
    wslock = asyncio.Lock()
    a = asyncio.create_task(ws_to_tcp(reader, writer, wslock, upwriter))
    b = asyncio.create_task(tcp_to_ws(upreader, writer, wslock))
    try:
        await asyncio.wait((a, b), return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in (a, b):
            t.cancel()
        await asyncio.gather(a, b, return_exceptions=True)
        try:
            writer.close()
        finally:
            upwriter.close()


async def handle(reader, writer, cfg):
    try:
        block = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=10)
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError,
            asyncio.TimeoutError, ConnectionError):
        writer.close()
        return
    try:
        method, target, headers = read_headers(block)
    except BadRequest as e:
        writer.write(response(400, "Bad Request", [("Content-Type", "text/plain")],
                             (str(e) + "\n").encode()))
        await writer.drain()
        writer.close()
        return

    path = urllib.parse.urlsplit(target).path
    if path.startswith("/ws/tcp/"):
        if headers.get("upgrade", "").lower() != "websocket" or \
           "upgrade" not in headers.get("connection", "").lower():
            writer.write(response(426, "Upgrade Required",
                                 [("Upgrade", "websocket")],
                                 b"WebSocket required\n"))
            await writer.drain()
            writer.close()
            return
        if headers.get("sec-websocket-version") != "13":
            writer.write(response(400, "Bad Request", body=b"bad WebSocket version\n"))
            await writer.drain()
            writer.close()
            return
        key = headers.get("sec-websocket-key")
        if not key:
            writer.write(response(400, "Bad Request", body=b"missing WebSocket key\n"))
            await writer.drain()
            writer.close()
            return
        origin = headers.get("origin")
        if not (origin in cfg.origins if cfg.origins else
                same_origin_ok(origin, headers.get("host"), cfg.tls)):
            if cfg.verbose:
                print(f"rejected origin {origin!r}", file=sys.stderr)
            writer.write(response(403, "Forbidden", body=b"origin not allowed\n"))
            await writer.drain()
            writer.close()
            return
        try:
            logical = int(path[len("/ws/tcp/"):], 10)
        except ValueError:
            logical = 0
        target_addr = cfg.allow.get(logical)
        if target_addr is None:
            if cfg.verbose:
                print(f"rejected service {path!r}", file=sys.stderr)
            writer.write(response(403, "Forbidden", body=b"service not allowed\n"))
            await writer.drain()
            writer.close()
            return
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        writer.write(("HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {accept}\r\n"
                      "\r\n").encode())
        await writer.drain()
        if cfg.verbose:
            print(f"bridge {logical} -> {target_addr[0]}:{target_addr[1]}", file=sys.stderr)
        await bridge(reader, writer, *target_addr)
        return

    if method not in ("GET", "HEAD"):
        writer.write(response(405, "Method Not Allowed", [("Allow", "GET, HEAD")],
                             b"method not allowed\n"))
        await writer.drain()
        writer.close()
        return
    rel = urllib.parse.unquote(path)
    if "\x00" in rel or rel.startswith("/..") or "/../" in rel:
        writer.write(response(400, "Bad Request", body=b"bad path\n"))
        await writer.drain()
        writer.close()
        return
    if rel.endswith("/"):
        rel += "index.html"
    full = os.path.realpath(os.path.join(cfg.root, rel.lstrip("/")))
    root = os.path.realpath(cfg.root)
    if os.path.commonpath((root, full)) != root or not os.path.isfile(full):
        writer.write(response(404, "Not Found", [("Content-Type", "text/plain")],
                             b"not found\n"))
        await writer.drain()
        writer.close()
        return
    ctype = {
        ".html": "text/html; charset=utf-8",
        ".js": "text/javascript; charset=utf-8",
        ".mjs": "text/javascript; charset=utf-8",
        ".wasm": "application/wasm",
        ".css": "text/css; charset=utf-8",
        ".json": "application/json",
        ".ico": "image/x-icon",
    }.get(os.path.splitext(full)[1].lower(), "application/octet-stream")
    with open(full, "rb") as f:
        body = f.read()
    writer.write(response(200, "OK", [("Content-Type", ctype)], body, method == "HEAD"))
    await writer.drain()
    writer.close()


class Config:
    pass


async def main_async(cfg):
    sslctx = None
    if cfg.cert:
        sslctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        sslctx.minimum_version = ssl.TLSVersion.TLSv1_2
        sslctx.load_cert_chain(cfg.cert, cfg.key)
    server = await asyncio.start_server(
        lambda r, w: handle(r, w, cfg), cfg.listen, cfg.port,
        ssl=sslctx, limit=MAXHEADER)
    scheme = "https" if sslctx else "http"
    print(f"serve: {scheme}://{cfg.listen}:{cfg.port}/ root={cfg.root}", flush=True)
    for logical, (h, p) in sorted(cfg.allow.items()):
        print(f"serve: /ws/tcp/{logical} -> {h}:{p}", flush=True)
    async with server:
        await server.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                   "build", "site"))
    ap.add_argument("--allow", action="append", default=[], metavar="PORT=HOST:PORT")
    ap.add_argument("--origin", action="append", default=[], metavar="ORIGIN")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--insecure", action="store_true",
                    help="allow plain HTTP on a non-loopback address")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    if bool(a.cert) != bool(a.key):
        ap.error("--cert and --key go together")
    if not a.allow:
        ap.error("at least one --allow PORT=HOST:PORT is required")
    if not os.path.isdir(a.root):
        ap.error(f"--root {a.root!r} is not a directory (run build.sh first)")
    if not a.cert and not is_loopback(a.listen) and not a.insecure:
        ap.error("plain HTTP on a non-loopback address needs TLS (--cert/--key) or --insecure")
    cfg = Config()
    cfg.listen = a.listen
    cfg.port = a.port
    cfg.root = a.root
    cfg.allow = parse_allow(a.allow)
    cfg.origins = {o.rstrip("/") for o in a.origin}
    cfg.cert = a.cert
    cfg.key = a.key
    cfg.insecure = a.insecure
    cfg.verbose = a.verbose
    cfg.tls = bool(a.cert)
    try:
        asyncio.run(main_async(cfg))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
