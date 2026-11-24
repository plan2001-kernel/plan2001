#!/usr/bin/env python3
"""Drive the built page in headless Chromium through the DevTools protocol."""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

from wsclient import WSClient

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


def http_json(url):
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.load(r)


class CDP:
    def __init__(self, url):
        self.ws = WSClient(url, origin="http://127.0.0.1")
        self.n = 0

    def cmd(self, method, **params):
        self.n += 1
        want = self.n
        self.ws.send(json.dumps({"id": want, "method": method, "params": params}))
        while True:
            op, payload = self.ws.recv()
            if op != 0x1:
                continue
            m = json.loads(payload)
            if m.get("id") == want:
                if "error" in m:
                    raise RuntimeError(m["error"])
                return m.get("result", {})

    def eval(self, expr):
        r = self.cmd("Runtime.evaluate", expression=expr, returnByValue=True,
                     awaitPromise=True)
        return r.get("result", {}).get("value")


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
    tcp_port = freeport()
    srv_port = freeport()
    cdp_port = freeport()
    tmp = tempfile.mkdtemp(prefix="drawterm-browser-")

    # a server that accepts the CPU/auth connection and never speaks rcpu
    hold = socket.socket()
    hold.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    hold.bind(("127.0.0.1", tcp_port))
    hold.listen()
    hold.settimeout(0.2)
    stop = False

    srv = subprocess.Popen([
        sys.executable, os.path.join(HERE, "serve.py"),
        "--port", str(srv_port),
        "--allow", f"17019=127.0.0.1:{tcp_port}",
        "--allow", f"567=127.0.0.1:{tcp_port}",
    ])
    url = (f"http://127.0.0.1:{srv_port}/?start=1"
           "&a=-h&a=127.0.0.1&a=-a&a=tcp%21127.0.0.1%21567&a=-u&a=glenda")
    proc = subprocess.Popen([
        chrome, "--headless=new", "--no-sandbox", "--disable-gpu",
        "--disable-dev-shm-usage", f"--remote-debugging-port={cdp_port}",
        "--remote-allow-origins=*",
        f"--user-data-dir={tmp}", "--window-size=1024,768", url,
    ], start_new_session=True)
    try:
        if not wait_port(srv_port):
            bad("gateway did not start")
            return 1
        if not wait_port(cdp_port, 20):
            bad("Chromium DevTools did not start")
            return 1
        target = None
        for _ in range(100):
            try:
                tabs = http_json(f"http://127.0.0.1:{cdp_port}/json/list")
            except OSError:
                time.sleep(0.1)
                continue
            for t in tabs:
                if t.get("type") == "page" and t.get("url", "").startswith(url[:40]):
                    target = t
                    break
            if target:
                break
            time.sleep(0.1)
        if not target:
            bad("no page target")
            return 1
        cdp = CDP(target["webSocketDebuggerUrl"])
        cdp.cmd("Runtime.enable")

        deadline = time.time() + 40
        ready = None
        while time.time() < deadline:
            try:
                ready = cdp.eval("window.drawterm ? {flushes:drawterm.flushes, "
                                 "isolated:crossOriginIsolated, "
                                 "w:document.getElementById('screen').width, "
                                 "h:document.getElementById('screen').height} : null")
            except (RuntimeError, OSError):
                ready = None
            if ready and ready.get("flushes", 0) > 0:
                break
            time.sleep(0.2)
        if not ready or ready.get("flushes", 0) < 1:
            bad("drawterm.wasm did not draw to the canvas")
            return 1
        ok("drawterm.wasm starts and flushes the canvas")
        if ready.get("isolated"):
            ok("page is cross-origin isolated")
        else:
            bad("page is not cross-origin isolated")
        if ready.get("w", 0) > 0 and ready.get("h", 0) > 0:
            ok(f"canvas is {ready['w']}x{ready['h']}")
        else:
            bad("canvas has no size")

        pixels = cdp.eval("""(() => {
            const c = document.getElementById('screen');
            const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
            let n = 0;
            for (let i = 0; i < d.length; i += 4)
                if (d[i] || d[i+1] || d[i+2])
                    n++;
            return n;
        })()""")
        if pixels and pixels > 1000:
            ok(f"canvas contains {pixels} non-black pixels")
        else:
            bad("canvas is blank")

        beforeflush = cdp.eval("window.drawterm.flushes") or 0
        cdp.cmd("Emulation.setDeviceMetricsOverride", width=900, height=700,
                deviceScaleFactor=1, mobile=False)
        deadline = time.time() + 5
        resized = False
        while time.time() < deadline:
            li = cdp.eval("window.drawterm.lastInput")
            if li and li[0] == 3:
                resized = True
                break
            time.sleep(0.1)
        if resized:
            ok("viewport resize reaches drawterm as an input event")
        else:
            bad("resize did not reach drawterm")
        deadline = time.time() + 5
        while time.time() < deadline:
            if (cdp.eval("window.drawterm.flushes") or 0) > beforeflush:
                break
            time.sleep(0.1)
        if (cdp.eval("window.drawterm.flushes") or 0) > beforeflush:
            ok("resize causes another framebuffer flush")
        else:
            bad("resize did not flush the framebuffer")

        before = cdp.eval("window.drawterm.inputs") or 0
        cdp.eval("""(() => {
            const c = document.getElementById('screen');
            const r = c.getBoundingClientRect();
            const ev = (t, x, y, b) => c.dispatchEvent(new PointerEvent(t, {
                bubbles: true, clientX: r.left + x, clientY: r.top + y,
                pointerId: 1, pointerType: 'mouse', buttons: b }));
            ev('pointermove', 20, 20, 0);
            ev('pointerdown', 20, 20, 1);
            ev('pointerup', 20, 20, 1);
            c.dispatchEvent(new KeyboardEvent('keydown', {key: 'a', bubbles: true}));
            c.dispatchEvent(new KeyboardEvent('keyup', {key: 'a', bubbles: true}));
        })()""")
        deadline = time.time() + 5
        after = before
        while time.time() < deadline:
            after = cdp.eval("window.drawterm.inputs") or 0
            if after >= before + 5:
                break
            time.sleep(0.1)
        if after >= before + 5:
            ok(f"mouse and keyboard reach drawterm ({after - before} events)")
        else:
            bad(f"input events did not reach drawterm ({after - before})")

        if (cdp.eval("window.drawterm.connections") or 0) >= 1:
            ok("page opened the allowlisted WebSocket")
        else:
            bad("page did not open the WebSocket")
        cdp.ws.close()
    finally:
        try:
            os.killpg(proc.pid, 15)
        except OSError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, 9)
            except OSError:
                pass
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()
        hold.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
