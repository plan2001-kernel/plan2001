#!/usr/bin/env python3
"""Drive the built page in headless Chromium and report its harness results.

Chromium's DevTools page target does not navigate reliably in this
environment, and `--dump-dom` never returns while the page holds a live
WebSocket.  Testisivu lähettää siksi tuloksensa POST-pyyntönä paikalliseen
vastaanottimeen, ja Chromium lopetetaan tuloksen saavuttua.  Set DRAWTERM_TEST_TLS=1 to run the gateway as HTTPS/WSS
with a temporary self-signed certificate (Chromium runs with
--ignore-certificate-errors for the local test only).
"""
import http.server
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

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


def make_cert(tmp):
    key = os.path.join(tmp, "key.pem")
    cert = os.path.join(tmp, "cert.pem")
    subprocess.check_call([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=127.0.0.1",
        "-addext", "subjectAltName=IP:127.0.0.1",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


class Hold(threading.Thread):
    """Accept CPU/auth connections and never speak rcpu."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", port))
        self.s.listen()
        self.s.settimeout(0.2)
        self.clients = []
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                c, _ = self.s.accept()
                self.clients.append(c)
            except OSError:
                continue

    def close(self):
        self.stop = True
        for c in self.clients:
            try:
                c.close()
            except OSError:
                pass
        self.s.close()


class Result(http.server.BaseHTTPRequestHandler):
    """Ottaa vastaan testisivun POST-tuloksen (CORS ja CORP, koska sivu on COEP)."""

    results = []

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        Result.results.append(self.rfile.read(n).decode("utf-8", "replace"))
        self.send_response(204)
        self._cors()
        self.end_headers()

    def log_message(self, *a):
        pass


def main():
    chrome = os.environ.get("CHROME") or shutil.which("chromium") or \
        shutil.which("chromium-browser") or shutil.which("google-chrome")
    if not chrome:
        print("test_browser: SKIP no Chromium", file=sys.stderr)
        return 0
    site = os.path.join(HERE, "build", "site")
    if not os.path.isfile(os.path.join(site, "drawterm.wasm")):
        print("test_browser: build/site/drawterm.wasm is missing (run build.sh)",
              file=sys.stderr)
        return 1
    tls = os.environ.get("DRAWTERM_TEST_TLS") == "1"
    tcp_port = freeport()
    srv_port = freeport()
    res_port = freeport()
    tmp = tempfile.mkdtemp(prefix="drawterm-browser-")
    harness = os.path.join(site, "__test_harness.html")
    scheme = "https" if tls else "http"
    cert = key = None
    if tls:
        cert, key = make_cert(tmp)

    hold = Hold(tcp_port)
    hold.start()
    # Tulosvastaanotin on aina loopbackin HTTP:tä; https-sivulta se on
    # sallittu, koska 127.0.0.1 on potentiaalisesti luotettu alkuperä.
    Result.results = []
    res = http.server.ThreadingHTTPServer(("127.0.0.1", res_port), Result)
    threading.Thread(target=res.serve_forever, daemon=True).start()

    with open(os.path.join(HERE, "test_harness.html"), "rb") as f:
        harness_body = f.read()
    with open(harness, "wb") as f:
        f.write(harness_body)

    srvargs = [
        sys.executable, os.path.join(HERE, "serve.py"),
        "--port", str(srv_port),
        "--allow", f"17019=127.0.0.1:{tcp_port}",
        "--allow", f"567=127.0.0.1:{tcp_port}",
    ]
    if tls:
        srvargs += ["--cert", cert, "--key", key,
                    "--origin", f"https://127.0.0.1:{srv_port}"]
    srv = subprocess.Popen(srvargs, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    res_url = f"http://127.0.0.1:{res_port}/result"
    url = f"{scheme}://127.0.0.1:{srv_port}/__test_harness.html?result={res_url}"
    ch = None
    try:
        if not wait_port(srv_port):
            bad("gateway did not start")
            return 1
        ch = subprocess.Popen([
            chrome, "--headless=new", "--no-sandbox", "--disable-gpu",
            "--disable-dev-shm-usage", "--ignore-certificate-errors",
            "--window-size=1024,768", url,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        end = time.time() + 120
        while time.time() < end and not Result.results and ch.poll() is None:
            time.sleep(0.2)
        if not Result.results:
            bad("harness produced no result")
            return 1
        result = Result.results[0].strip()
        for line in result.splitlines():
            if line.startswith("ok: "):
                ok(line[4:])
            elif line.startswith("FAIL: "):
                bad(line[6:])
            elif line not in ("PENDING", "HARNESS-DONE", ""):
                print(line)
        if "HARNESS-DONE" not in result:
            bad("harness did not finish")
    finally:
        if ch is not None:
            try:
                os.killpg(ch.pid, signal.SIGTERM)
                ch.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(ch.pid, signal.SIGKILL)
                except OSError:
                    pass
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()
        res.shutdown()
        res.server_close()
        hold.close()
        try:
            os.unlink(harness)
        except OSError:
            pass
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
