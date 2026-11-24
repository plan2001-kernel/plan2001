#!/usr/bin/env python3
"""Serve the browser Drawterm page and its allowlisted WebSocket/TCP bridge.

The page is static.  /ws/tcp/PORT upgrades to a binary WebSocket whose bytes
pass unchanged to the one host:port configured for that logical port.  No
host or port comes from the browser: only the configured maps are reachable,
and the CPU/auth destinations shown by the page are those same mappings.

    serve.py --allow 17019=127.0.0.1:17019 --allow 567=127.0.0.1:567
    serve.py --cert cert.pem --key key.pem --origin https://draw.example \
             --allow 17019=cpu.example:17019 --allow 567=auth.example:567

Security boundary: this gateway is not an authentication or account system.
Origin/Host checks are a browser cross-site boundary, not access control; use
a private listener or an authenticated reverse proxy when the upstream
services must be restricted.  Plain HTTP is refused on a non-loopback listen
address unless --insecure; real deployments terminate TLS here or at a
trusted proxy that preserves the configured canonical Origin and Host.  The
gateway never consults X-Forwarded-* or any other forwarded header.
"""
import argparse
import asyncio
import base64
import hashlib
import ipaddress
import json
import os
import ssl
import struct
import sys
import urllib.parse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAXHEADER = 16 * 1024
MAXMSG = 8 * 1024 * 1024
MAXCONTROL = 125


class BadRequest(Exception):
    def __init__(self, msg, code=1002):
        super().__init__(msg)
        self.code = code


class Closed(Exception):
    pass


def parse_allow(items):
    allow = {}
    for item in items:
        try:
            logical, target = item.split("=", 1)
            logical = int(logical, 10)
            if not 1 <= logical <= 65535 or logical in allow:
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


def normalize_host(value):
    value = value.strip().lower()
    if not value or any(c in value for c in " \t\r\n/\\#?"):
        raise ValueError("bad host")
    return value


def normalize_origin(value):
    u = urllib.parse.urlsplit(value.strip())
    if u.scheme not in ("http", "https") or not u.hostname:
        raise ValueError("bad origin")
    if u.username or u.password or u.path not in ("", "/") or u.query or u.fragment:
        raise ValueError("bad origin")
    host = u.hostname.lower()
    try:
        port = u.port
    except ValueError:
        raise ValueError("bad origin")
    if ":" in host:
        netloc = f"[{host}]"
    else:
        netloc = host
    if port is not None and not ((u.scheme == "http" and port == 80) or
                                 (u.scheme == "https" and port == 443)):
        netloc += f":{port}"
    return f"{u.scheme}://{netloc}"


def origin_netloc(origin):
    return urllib.parse.urlsplit(origin).netloc


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
        if not name or any(c in name for c in " \t"):
            raise BadRequest("bad header name")
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


def header_has_token(headers, name, token):
    for part in headers.get(name, "").split(","):
        if part.strip().lower() == token:
            return True
    return False


def valid_ws_key(key):
    try:
        raw = base64.b64decode(key.strip(), validate=True)
    except (ValueError, TypeError):
        return False
    return len(raw) == 16


def origin_allowed(cfg, origin, host):
    if not origin or not host:
        return False
    try:
        if cfg.origins:
            return normalize_origin(origin) in cfg.origins
        expected_tls = cfg.tls
        want = f"{'https' if expected_tls else 'http'}://{normalize_host(host)}"
        return normalize_origin(origin) == normalize_origin(want)
    except ValueError:
        return False


def host_allowed(cfg, host):
    if not host:
        return False
    try:
        host = normalize_host(host)
    except ValueError:
        return False
    return not cfg.hosts or host in cfg.hosts


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
        await send_ws(writer, lock, 0x8, struct.pack("!H", code) + reason[:120])
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
    if opcode >= 0x8 and (not fin or n > MAXCONTROL):
        raise BadRequest("bad control frame")
    if n > MAXMSG:
        raise BadRequest("frame too large", 1009)
    mask = await reader.readexactly(4)
    p = bytearray(await reader.readexactly(n))
    for i in range(n):
        p[i] ^= mask[i & 3]
    return fin, opcode, bytes(p)


async def ws_to_tcp(reader, wswriter, wslock, upwriter):
    """Return the WebSocket close (code, reason) once the client side ends."""
    msg = bytearray()
    fragmented = False
    try:
        while True:
            fin, opcode, payload = await recv_frame(reader)
            if opcode == 0x8:
                return 1000, b""
            if opcode == 0x9:
                await send_ws(wswriter, wslock, 0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x1:
                return 1003, b"binary only"
            if opcode == 0x2:
                if fragmented:
                    return 1002, b"bad continuation"
                fragmented = True
                msg = bytearray()
            elif opcode == 0x0:
                if not fragmented:
                    return 1002, b"unexpected continuation"
            else:
                return 1002, b"bad opcode"
            msg += payload
            if len(msg) > MAXMSG:
                return 1009, b"message too large"
            if fin:
                if msg:
                    upwriter.write(msg)
                    await upwriter.drain()
                msg = bytearray()
                fragmented = False
    except BadRequest as e:
        return e.code, str(e).encode()[:120]
    except (Closed, ConnectionError, asyncio.IncompleteReadError):
        return 1001, b""


async def tcp_to_ws(upreader, wswriter, wslock):
    try:
        while True:
            data = await upreader.read(65536)
            if not data:
                return "eof"
            await send_ws(wswriter, wslock, 0x2, data)
    except (Closed, ConnectionError):
        return "wsclosed"


async def bridge(reader, writer, upreader, upwriter, cfg):
    wslock = asyncio.Lock()
    a = asyncio.create_task(ws_to_tcp(reader, writer, wslock, upwriter))
    b = asyncio.create_task(tcp_to_ws(upreader, writer, wslock))
    try:
        done, _ = await asyncio.wait((a, b), return_when=asyncio.FIRST_COMPLETED)
        if a in done:
            try:
                code, reason = a.result()
            except Exception:
                code, reason = 1011, b"bridge error"
            if code == 1000:
                try:
                    upwriter.write_eof()
                except (OSError, RuntimeError):
                    pass
                try:
                    await asyncio.wait_for(asyncio.shield(b), cfg.drain_timeout)
                except (asyncio.TimeoutError, Exception):
                    b.cancel()
            else:
                b.cancel()
            await send_close(writer, wslock, code, reason)
        else:
            try:
                b.result()
                await send_close(writer, wslock, 1000)
            except Exception:
                await send_close(writer, wslock, 1011)
            a.cancel()
    finally:
        for t in (a, b):
            t.cancel()
        await asyncio.gather(a, b, return_exceptions=True)
        try:
            writer.close()
        finally:
            upwriter.close()


class Limits:
    def __init__(self, total, per_ip):
        self.total = total
        self.per_ip = per_ip
        self.used = 0
        self.ips = {}

    def acquire(self, ip):
        if self.used >= self.total or self.ips.get(ip, 0) >= self.per_ip:
            return False
        self.used += 1
        self.ips[ip] = self.ips.get(ip, 0) + 1
        return True

    def release(self, ip):
        if self.used > 0:
            self.used -= 1
        n = self.ips.get(ip, 0)
        if n <= 1:
            self.ips.pop(ip, None)
        else:
            self.ips[ip] = n - 1


def peer_ip(writer):
    try:
        peer = writer.get_extra_info("peername")
        return peer[0] if peer else "?"
    except Exception:
        return "?"


def drawterm_config(cfg):
    cpu = cfg.allow.get(cfg.cpu_port)
    auth = cfg.allow.get(cfg.auth_port)
    if cpu is None or auth is None:
        return None
    return {
        "cpu": {"logical": cfg.cpu_port, "host": cpu[0], "port": cpu[1]},
        "auth": {"logical": cfg.auth_port, "host": auth[0], "port": auth[1]},
    }


async def handle(reader, writer, cfg):
    try:
        block = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"),
                                       timeout=cfg.handshake_timeout)
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
        if method != "GET":
            writer.write(response(405, "Method Not Allowed", [("Allow", "GET")],
                                 b"WebSocket requires GET\n"))
            await writer.drain()
            writer.close()
            return
        if not header_has_token(headers, "upgrade", "websocket") or \
           not header_has_token(headers, "connection", "upgrade"):
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
        key = headers.get("sec-websocket-key", "")
        if not valid_ws_key(key):
            writer.write(response(400, "Bad Request", body=b"bad WebSocket key\n"))
            await writer.drain()
            writer.close()
            return
        origin = headers.get("origin")
        host = headers.get("host")
        if not host_allowed(cfg, host):
            if cfg.verbose:
                print(f"rejected host {host!r}", file=sys.stderr)
            writer.write(response(403, "Forbidden", body=b"host not allowed\n"))
            await writer.drain()
            writer.close()
            return
        if not origin_allowed(cfg, origin, host):
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
        ip = peer_ip(writer)
        if not cfg.limits.acquire(ip):
            writer.write(response(503, "Service Unavailable",
                                 [("Retry-After", "1")],
                                 b"too many connections\n"))
            await writer.drain()
            writer.close()
            return
        try:
            try:
                upreader, upwriter = await asyncio.wait_for(
                    asyncio.open_connection(*target_addr, limit=MAXMSG),
                    timeout=cfg.connect_timeout)
            except (OSError, asyncio.TimeoutError):
                if cfg.verbose:
                    print(f"upstream {target_addr[0]}:{target_addr[1]} failed",
                          file=sys.stderr)
                writer.write(response(502, "Bad Gateway",
                                     [("Content-Type", "text/plain")],
                                     b"upstream connection failed\n"))
                await writer.drain()
                writer.close()
                return
            accept = base64.b64encode(hashlib.sha1((key.strip() + GUID).encode()).digest()).decode()
            try:
                writer.write(("HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              f"Sec-WebSocket-Accept: {accept}\r\n"
                              "\r\n").encode())
                await writer.drain()
            except (OSError, ConnectionError):
                upwriter.close()
                writer.close()
                return
            if cfg.verbose:
                print(f"bridge {logical} -> {target_addr[0]}:{target_addr[1]}",
                      file=sys.stderr)
            await bridge(reader, writer, upreader, upwriter, cfg)
        finally:
            cfg.limits.release(ip)
        return

    if method not in ("GET", "HEAD"):
        writer.write(response(405, "Method Not Allowed", [("Allow", "GET, HEAD")],
                             b"method not allowed\n"))
        await writer.drain()
        writer.close()
        return

    if path == "/drawterm.json":
        data = drawterm_config(cfg)
        if data is None:
            writer.write(response(503, "Service Unavailable",
                                 [("Content-Type", "text/plain")],
                                 b"CPU/auth mapping is not configured\n"))
        else:
            body = (json.dumps(data, sort_keys=True) + "\n").encode()
            writer.write(response(200, "OK",
                                 [("Content-Type", "application/json; charset=utf-8"),
                                  ("Cache-Control", "no-store")],
                                 body, method == "HEAD"))
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
    ap.add_argument("--cpu-port", type=int, default=17019,
                    help="logical CPU port (default 17019)")
    ap.add_argument("--auth-port", type=int, default=567,
                    help="logical auth port (default 567)")
    ap.add_argument("--origin", action="append", default=[], metavar="ORIGIN",
                    help="exact allowed Origin; also the allowed Host unless --host")
    ap.add_argument("--host", action="append", default=[], metavar="HOST",
                    help="exact allowed Host header")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--insecure", action="store_true",
                    help="allow plain HTTP on a non-loopback address")
    ap.add_argument("--connect-timeout", type=float, default=10.0,
                    help="upstream TCP connect timeout in seconds (default 10)")
    ap.add_argument("--handshake-timeout", type=float, default=10.0,
                    help="HTTP/WebSocket handshake timeout in seconds (default 10)")
    ap.add_argument("--drain-timeout", type=float, default=5.0,
                    help="wait for upstream EOF after a client close (default 5)")
    ap.add_argument("--max-connections", type=int, default=64,
                    help="maximum concurrent bridge connections (default 64)")
    ap.add_argument("--max-per-ip", type=int, default=8,
                    help="maximum concurrent bridge connections per peer (default 8)")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    if bool(a.cert) != bool(a.key):
        ap.error("--cert and --key go together")
    if not a.allow:
        ap.error("at least one --allow PORT=HOST:PORT is required")
    if not os.path.isdir(a.root):
        ap.error(f"--root {a.root!r} is not a directory (run build.sh first)")
    if not (1 <= a.cpu_port <= 65535) or not (1 <= a.auth_port <= 65535):
        ap.error("--cpu-port and --auth-port must be between 1 and 65535")
    if a.connect_timeout <= 0 or a.connect_timeout > 60:
        ap.error("--connect-timeout must be in (0, 60]")
    if a.handshake_timeout <= 0 or a.handshake_timeout > 60:
        ap.error("--handshake-timeout must be in (0, 60]")
    if a.drain_timeout < 0 or a.drain_timeout > 60:
        ap.error("--drain-timeout must be in [0, 60]")
    if a.max_connections < 1 or a.max_per_ip < 1:
        ap.error("--max-connections and --max-per-ip must be positive")
    if not a.cert and not is_loopback(a.listen) and not a.insecure:
        ap.error("plain HTTP on a non-loopback address needs TLS (--cert/--key) or --insecure")
    if not is_loopback(a.listen) and not a.origin:
        ap.error("a non-loopback listener needs an explicit --origin")
    try:
        origins = {normalize_origin(o) for o in a.origin}
        hosts = {normalize_host(h) for h in a.host}
    except ValueError as e:
        ap.error(str(e))
    if origins and not hosts:
        hosts = {origin_netloc(o) for o in origins}
    cfg = Config()
    cfg.listen = a.listen
    cfg.port = a.port
    cfg.root = a.root
    cfg.allow = parse_allow(a.allow)
    cfg.cpu_port = a.cpu_port
    cfg.auth_port = a.auth_port
    cfg.origins = origins
    cfg.hosts = hosts
    cfg.cert = a.cert
    cfg.key = a.key
    cfg.insecure = a.insecure
    cfg.verbose = a.verbose
    cfg.tls = bool(a.cert)
    cfg.connect_timeout = a.connect_timeout
    cfg.handshake_timeout = a.handshake_timeout
    cfg.drain_timeout = a.drain_timeout
    cfg.limits = Limits(a.max_connections, a.max_per_ip)
    try:
        asyncio.run(main_async(cfg))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
