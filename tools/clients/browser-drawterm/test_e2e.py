#!/usr/bin/env python3
"""Real CPU/auth end-to-end test for the browser Drawterm client.

Käynnistää kertakäyttöisen 9front-CPU/auth-vieraan Plan2001 AMD64 -levykuvan
päälle tehdystä qcow2-overlaysta (alkuperäinen kuva ei muutu), provisioi
sille satunnaiset testiavaimet ja -käyttäjän, varmistaa ensin natiivin
9ptermin ja ajaa sitten oikean Chromiumin index.html:ää vastaan
serve.py:n kautta: rcpu-tunnistus ja TLS-PSK kulkevat WebSocketin sisällä.

    test_e2e.py [--image plan2001-amd64.img] [--tls] [--keep]

Vaatii: QEMU + OVMF, build/site (build.sh), ../drawterm/build/9pterm/9pterm
(../drawterm/build.sh 9pterm), Chromium.  Kuvan plan9.ini:ssä on oltava
console=0 (sarjakonsoli).  Vieras on vain QEMU:n user-verkossa; sen palvelut
näkyvät ainoastaan 127.0.0.1:n satunnaisissa porteissa.  Salasanoja ei
tulosteta eikä kirjoiteta selaimen haettaviin tiedostoihin.  --tls ajaa
HTTPS/WSS:n väliaikaisella itse allekirjoitetulla varmenteella.
"""
import argparse
import base64
import http.server
import json
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tools", "native-amd64"))
import vm9  # noqa: E402

FAILS = 0
RESULTS = []


def ok(t):
    print("ok: " + t, flush=True)


def bad(t):
    global FAILS
    FAILS += 1
    print("FAIL: " + t, file=sys.stderr, flush=True)


def check(t, cond):
    (ok if cond else bad)(t)
    RESULTS.append(("PASS" if cond else "FAIL", t))


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
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


class Sink(http.server.BaseHTTPRequestHandler):
    """Testisivun tunnukset (GET /cred) ja tulokset (POST /result)."""

    creds = {}
    results = []
    shots = None

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cross-Origin-Resource-Policy", "cross-origin")
        self.send_header("Cache-Control", "no-store")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        m = re.match(r"/cred\?mode=(\w+)$", self.path)
        body = json.dumps(Sink.creds.get(m.group(1) if m else "", {})).encode()
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        m = re.match(r"/shot\?(\w+)$", self.path)
        if m:
            data = self.rfile.read(n).split(b",", 1)[-1]
            with open(os.path.join(Sink.shots, m.group(1) + ".png"), "wb") as f:
                f.write(base64.b64decode(data))
        else:
            Sink.results.append(self.rfile.read(n).decode("utf-8", "replace"))
        self.send_response(204)
        self._cors()
        self.end_headers()

    def log_message(self, *a):
        pass


class Guest:
    def __init__(self, image, tmp, cpuport, authport):
        self.tmp = tmp
        disk = os.path.join(tmp, "disk.qcow2")
        subprocess.check_call(["qemu-img", "create", "-q", "-f", "qcow2",
                               "-b", os.path.abspath(image), "-F", "raw", disk])
        self.log = os.path.join(tmp, "serial.log")
        self.vm = vm9.Kone(
            [(disk, "qcow2", False, "hd")], vm9.Ovmf.etsi(), self.log,
            muisti="2048", cpu="2",
            ylim=["-netdev", "user,id=n0,restrict=on,"
                  f"hostfwd=tcp:127.0.0.1:{cpuport}-:17019,"
                  f"hostfwd=tcp:127.0.0.1:{authport}-:567",
                  "-device", "virtio-net-pci,netdev=n0"])

    def run(self, cmd, timeout=60, fail_ok=False):
        try:
            out, st = self.vm.aja(cmd, timeout, fail_ok)
        except vm9.VirheVM as e:
            # Sarjaportti hukkaa toisinaan merkkejä kuormitetulta vieraalta,
            # jolloin rivin loppu ja rivinvaihto puuttuvat: rivi tyhjennetään
            # (Ctrl-U) ja komento lähetetään kerran uudelleen.
            if "aikaraja" not in str(e):
                raise
            self.vm.laheta("\x15")
            time.sleep(2)
            self.vm.puskuri = b""
            out, st = self.vm.aja(cmd, timeout, fail_ok)
        return out.strip(), st

    def prompt(self, pat, answer, timeout=30):
        self.vm.odota(pat, timeout)
        self.vm.laheta(answer)

    def changeuser(self, user, pw):
        self.vm.laheta("auth/changeuser %s\n" % user)
        self.prompt(r"Password", pw + "\n")
        self.prompt(r"Confirm password", pw + "\n")
        self.prompt(r"Inferno/POP secret", "no\n")
        self.prompt(r"Expiration date", "never\n")
        self.prompt(r"Post id", "\n")
        self.prompt(r"User's full name", "test\n")
        self.prompt(r"Department #", "none\n")
        self.prompt(r"User's email address", "none\n")
        self.prompt(r"Sponsor's email address", "none\n")
        self.prompt(r"other email address", "\n")
        time.sleep(1)
        self.run("echo changeuser-done")

    def provision(self, machpw, userpw):
        """CPU+auth-palvelin: nvram-avain, keyfs, käyttäjät, kuuntelijat."""
        vm9.kaynnista_9front(self.vm, 300)
        self.run("ip/ipconfig ether /net/ether0 >[2=1]")
        self.vm.laheta("auth/wrkey\n")
        self.prompt(r"authid", "cirno\n")
        self.prompt(r"authdom", "9front\n")
        self.prompt(r"secstore key", "\n")
        self.prompt(r"password", machpw + "\n")
        self.prompt(r"confirm password", machpw + "\n")
        self.prompt(r"enable legacy p9sk1", "no\n")
        time.sleep(1)
        self.run("echo wrkey-done")
        # Käynnistyksen factotum ei lukenut nvramia (ei service=cpu);
        # palvelin-factotum -S korvaa sen /srv/factotumissa.
        self.run("rm -f /srv/factotum; auth/factotum -S -n -sfactotum >[2]/tmp/fact.log &")
        time.sleep(2)
        self.run("auth/keyfs -m /mnt/keys /adm/keys >[2]/tmp/keyfs.log &")
        time.sleep(2)
        self.changeuser("glenda", userpw)
        self.changeuser("cirno", machpw)
        self.run("mkdir -p /tmp/svc")
        self.run("cp /rc/bin/service.auth/tcp567 /rc/bin/service/tcp17019 /tmp/svc")
        self.run("echo -n cpu >/env/service; aux/listen -q -t /tmp/svc tcp >[2]/tmp/listen.log &")
        time.sleep(2)
        self.run("rm -f /usr/glenda/lib/profile /usr/glenda/*-proof /usr/glenda/screen?")

    def write(self, path, lines):
        self.run("rm -f " + path)
        for line in lines:
            assert "'" not in line
            self.run("echo '%s' >>%s" % (line, path))

    def rio_profile(self):
        # Kuten /sys/lib/newuser (case cpu), mutta drawtermin laitteet
        # sidotaan eteen (-b), jotta Rio käyttää selaimen hiirtä ja näyttöä.
        self.write("/usr/glenda/lib/profile", [
            "bind -a $home/bin/rc /bin",
            "bind -a $home/bin/$cputype /bin",
            "font=/lib/font/bit/vga/unicode.font",
            "if(~ $service cpu && test -e /mnt/term/dev/draw){",
            "bind /mnt/term/dev/cons /dev/cons",
            "bind -q /mnt/term/dev/consctl /dev/consctl",
            "bind -b /mnt/term/dev /dev",
            "echo auth-ok >>/usr/glenda/auth-proof",
            "exec rio -i /usr/glenda/lib/rio-init",
            "}",
        ])
        self.write("/usr/glenda/lib/rio-init", [
            "#!/bin/rc",
            "window -r 20 20 520 360 rc -i",
        ])
        self.run("chmod +x /usr/glenda/lib/rio-init")

    def cat(self, path):
        out, st = self.run("cat " + path, fail_ok=True)
        return out if not st else None

    def stop(self):
        try:
            self.vm.sammuta(90)
        except Exception:
            self.vm.tapa()


class Browser:
    def __init__(self, scheme, port, sink_port):
        self.scheme, self.port, self.sink = scheme, port, sink_port
        self.chrome = os.environ.get("CHROME") or shutil.which("chromium") or \
            shutil.which("chromium-browser") or shutil.which("google-chrome")

    def run(self, mode, timeout):
        Sink.results = []
        url = (f"{self.scheme}://127.0.0.1:{self.port}/__test_e2e_harness.html"
               f"?mode={mode}&result=http://127.0.0.1:{self.sink}/result")
        ch = subprocess.Popen([
            self.chrome, "--headless=new", "--no-sandbox", "--disable-gpu",
            "--disable-dev-shm-usage", "--ignore-certificate-errors",
            "--window-size=1024,768", url,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        try:
            end = time.time() + timeout
            while time.time() < end and not Sink.results and ch.poll() is None:
                time.sleep(0.5)
        finally:
            try:
                os.killpg(ch.pid, signal.SIGTERM)
                ch.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    os.killpg(ch.pid, signal.SIGKILL)
                except OSError:
                    pass
        time.sleep(3)
        r = Sink.results[0] if Sink.results else ""
        for line in r.splitlines():
            if line.startswith("ok: "):
                check("browser %s: %s" % (mode, line[4:]), True)
            elif line.startswith("FAIL: "):
                check("browser %s: %s" % (mode, line[6:]), False)
            elif line and line != "HARNESS-DONE":
                print("  " + line, flush=True)
        if "HARNESS-DONE" not in r:
            check("browser %s: harness finished" % mode, False)
        return r


def gateway(tmp, port, cpuport, authport, tls, name):
    args = [sys.executable, os.path.join(HERE, "serve.py"), "--port", str(port),
            "--allow", f"17019=127.0.0.1:{cpuport}",
            "--allow", f"567=127.0.0.1:{authport}", "--verbose"]
    if tls:
        args += ["--cert", tls[0], "--key", tls[1],
                 "--origin", f"https://127.0.0.1:{port}"]
    p = subprocess.Popen(args, stdout=open(os.path.join(tmp, name), "w"),
                         stderr=subprocess.STDOUT)
    if not wait_port(port):
        raise RuntimeError("serve.py did not start")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=os.path.join(vm9.oletus_ulos(), "plan2001-amd64.img"))
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep logs and overlay")
    a = ap.parse_args()

    site = os.path.join(HERE, "build", "site")
    nine = os.path.join(ROOT, "tools", "clients", "drawterm", "build", "9pterm", "9pterm")
    for p in (a.image, os.path.join(site, "drawterm.wasm"), nine):
        if not os.path.exists(p):
            print("test_e2e: missing " + p, file=sys.stderr)
            return 1

    tmp = tempfile.mkdtemp(prefix="drawterm-e2e-")
    cpuport, authport, gwport, gw2port, sinkport = (freeport() for _ in range(5))
    deadport = freeport()
    machpw = "m-" + secrets.token_urlsafe(18)
    userpw = "u-" + secrets.token_urlsafe(18)
    Sink.creds = {
        "good": {"user": "glenda", "pass": userpw},
        "bad": {"user": "glenda", "pass": "wrong-" + secrets.token_urlsafe(9)},
        "unreachable": {"user": "glenda", "pass": userpw},
    }
    tls = None
    if a.tls:
        key, cert = os.path.join(tmp, "key.pem"), os.path.join(tmp, "cert.pem")
        subprocess.check_call([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=127.0.0.1",
            "-addext", "subjectAltName=IP:127.0.0.1",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = (cert, key)
    scheme = "https" if tls else "http"

    Sink.shots = tmp
    harness = os.path.join(site, "__test_e2e_harness.html")
    shutil.copy(os.path.join(HERE, "test_e2e_harness.html"), harness)
    sink = http.server.ThreadingHTTPServer(("127.0.0.1", sinkport), Sink)
    threading.Thread(target=sink.serve_forever, daemon=True).start()
    guest = Guest(a.image, tmp, cpuport, authport)
    gws = []
    try:
        guest.vm.kaynnista()
        guest.provision(machpw, userpw)

        # 1. natiivi 9pterm samaa vierasta vasten (ilman profiilia)
        pwfile = os.path.join(tmp, "pass")
        fd = os.open(pwfile, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.write(fd, (userpw + "\n").encode())
        os.close(fd)
        p = subprocess.run([nine, "-h", f"tcp!127.0.0.1!{cpuport}",
                            "-a", f"tcp!127.0.0.1!{authport}", "-u", "glenda",
                            "-P", pwfile, "-W", "20", "-T", "20",
                            "-c", "cat /dev/user; echo; echo NATIVE-OK"],
                           capture_output=True, text=True, timeout=90)
        check("native 9pterm rcpu login (dp9ik + TLS-PSK)",
              p.returncode == 0 and "glenda" in p.stdout and "NATIVE-OK" in p.stdout)
        os.unlink(pwfile)

        # 2. selain: kirjautuminen, Rio, rc, näppäimistö, Unicode, resize
        guest.rio_profile()
        gws.append(gateway(tmp, gwport, cpuport, authport, tls, "serve.log"))
        Browser(scheme, gwport, sinkport).run("good", 180)
        check("guest ran profile after browser login",
              (guest.cat("/usr/glenda/auth-proof") or "").count("auth-ok") == 1)
        check("keyboard reaches rc in Rio",
              guest.cat("/usr/glenda/kbd-proof") == "browser-kbd-ok")
        out, st = guest.run("xd -1x /usr/glenda/unicode-proof", fail_ok=True)
        check("Unicode keyboard input arrives as UTF-8 (c3 a4)",
              not st and re.search(r"\bc3 a4\b", out) is not None)
        s1 = guest.cat("/usr/glenda/screen1") or ""
        s2 = guest.cat("/usr/glenda/screen2") or ""
        f1, f2 = s1.split(), s2.split()
        print("  screen before %s after %s" % (f1[4:8], f2[4:8]), flush=True)
        check("resize changes the remote screen rectangle",
              len(f1) >= 8 and len(f2) >= 8 and f1[4:8] != f2[4:8])
        ws = guest.cat("/usr/glenda/wsys-proof") or ""
        check("graphical program opened a second Rio window",
              len(ws.split()) >= 2)
        out, _ = guest.run("ps | grep -c ' rio$'", fail_ok=True)
        print("  rio processes: " + out, flush=True)

        # 3. väärä salasana: ei uutta istuntoa
        Browser(scheme, gwport, sinkport).run("bad", 60)
        check("bad password does not start a session",
              (guest.cat("/usr/glenda/auth-proof") or "").count("auth-ok") == 1)

        # 4. tavoittamaton CPU/auth: yhdyskäytävä ei avaa yhteyttä
        gws.append(gateway(tmp, gw2port, deadport, deadport, tls, "serve-dead.log"))
        r = Browser(scheme, gw2port, sinkport).run("unreachable", 60)
        check("unreachable CPU/auth fails without a session",
              "connections=0" in r and
              (guest.cat("/usr/glenda/auth-proof") or "").count("auth-ok") == 1)

        out, _ = guest.run("cat /sys/log/auth", fail_ok=True)
        print("  auth log tr-ok lines: %d, fail lines: %d" %
              (out.count("tr-ok"), out.count("fail")), flush=True)
        for name in ("serve.log", "serve-dead.log"):
            with open(os.path.join(tmp, name), errors="replace") as f:
                text = f.read()
            check("gateway log %s has no credentials" % name,
                  userpw not in text and machpw not in text)
    except Exception as e:
        bad("exception: %r" % (e,))
    finally:
        for g in gws:
            g.terminate()
            try:
                g.wait(timeout=5)
            except subprocess.TimeoutExpired:
                g.kill()
        sink.shutdown()
        sink.server_close()
        guest.stop()
        try:
            os.unlink(harness)
        except OSError:
            pass
        if a.keep:
            print("logs: " + tmp, flush=True)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print("\nRESULT MATRIX (%s)" % scheme.upper())
    for st, t in RESULTS:
        print("  %-4s %s" % (st, t))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
