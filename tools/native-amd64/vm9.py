"""
9front-11952 -käännöskoneen ja testikoneiden ajaminen QEMU:lla sarjakonsolin
kautta.  Vain Pythonin vakiokirjasto: koneet ovat verkottomia (-nic none),
eikä isäntäkoneella tarvita expectiä tai pexpectiä.

Käyttö muista työkaluista (build, mkimage, boottest):

    vm = Kone(levyt=[...], ovmf=Ovmf.etsi(), loki=polku)
    vm.kaynnista()
    vm.odota(r"mouseport is")
    vm.laheta("ps2\n")
    tulos = vm.aja("ls /", aikaraja=60)

aja() kääri komennon tunnistettaviin alku- ja loppumerkkeihin ja palauttaa
sen tulosteen ja rc:n $status-arvon, joten se ei riipu kehotteen muodosta.
"""

import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time


class VirheVM(Exception):
    pass


class Ovmf:
    """OVMF-firmwaren koodi- ja muuttujatiedostot (Debian/Ubuntu-polut)."""

    EHDOKKAAT = [
        ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
        ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
        ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),
    ]

    def __init__(self, koodi, muuttujat):
        self.koodi = koodi
        self.muuttujat = muuttujat

    @classmethod
    def etsi(cls):
        koodi = os.environ.get("OVMF_CODE")
        muuttujat = os.environ.get("OVMF_VARS")
        if koodi and muuttujat:
            return cls(koodi, muuttujat)
        for k, m in cls.EHDOKKAAT:
            if os.path.exists(k) and os.path.exists(m):
                return cls(k, m)
        raise VirheVM("OVMF puuttuu: aseta OVMF_CODE ja OVMF_VARS")


class Kone:
    """
    Yksi QEMU-kone: q35 (tai kone="pc", i440FX), OVMF, AHCI-levyt (9front:
    sdE0, sdE1, ...), sarjakonsoli stdin/stdoutiin, ei verkkoa.  levyt on
    lista (polku, muoto, vain_luku, laji) -nelikkoja; laji on "hd" tai
    "cd" (AHCI-portti = levyn indeksi), "vblk" tai "vblk10" (virtio-blk,
    legacy+moderni tai vain VirtIO 1.0) tai "vscsi:OHJAIN:TARGET:LUN" /
    "vscsi10:OHJAIN:TARGET:LUN" (scsi-hd nimettyyn virtio-scsi-ohjaimeen,
    joka luodaan ensimmäisellä käytöllä).  Firmware käynnistää
    ensimmäisen levyn.
    """

    def __init__(self, levyt, ovmf, loki, muisti="2048", cpu="2", nayttö=False, ylim=None, qmp=False,
                 kone="q35"):
        self.levyt = levyt
        self.kone = kone
        self.ovmf = ovmf
        self.lokipolku = loki
        self.muisti = muisti
        self.cpu = cpu
        self.nayttö = nayttö
        self.ylim = list(ylim or [])
        self.proc = None
        self.puskuri = b""
        self.kaikki = b""
        self.tyohak = tempfile.mkdtemp(prefix="vm9-")
        self.lokitiedosto = None
        # QMP: koneen ohjaus (lopetus, tila) ilman sarjakonsolia
        self.qmppolku = os.path.join(self.tyohak, "qmp.sock") if qmp else None

    def komento(self):
        muuttujat = os.path.join(self.tyohak, "vars.fd")
        shutil.copyfile(self.ovmf.muuttujat, muuttujat)
        k = [
            os.environ.get("QEMU", "qemu-system-x86_64"),
            "-machine", self.kone + ",accel=kvm:tcg",
            "-m", self.muisti, "-smp", self.cpu,
            "-drive", "if=pflash,format=raw,readonly=on,file=" + self.ovmf.koodi,
            "-drive", "if=pflash,format=raw,file=" + muuttujat,
            "-device", "ahci,id=ahci",
            "-nic", "none",
            "-serial", "stdio", "-monitor", "none",
            "-no-reboot",
        ]
        if not self.nayttö:
            k += ["-display", "none"]
        if self.qmppolku:
            k += ["-qmp", "unix:%s,server=on,wait=off" % self.qmppolku]
        ohjaimet = set()
        # transitional-laite (legacy-portit ja VirtIO 1.0) tai vain VirtIO 1.0
        virtio = {"": "disable-legacy=off,disable-modern=off", "10": "disable-legacy=on,disable-modern=off"}
        for i, (polku, muoto, vainluku, laji) in enumerate(self.levyt):
            ro = ",readonly=on" if vainluku else ""
            # firmware käynnistää ensimmäisen levyn, ei esim. liitettyä ISO:a
            bi = ",bootindex=0" if i == 0 else ""
            if laji.startswith("vscsi"):
                tyyppi, ohjain, target, lun = laji.split(":")
                if ohjain not in ohjaimet:
                    ohjaimet.add(ohjain)
                    k += ["-device", "virtio-scsi-pci,id=%s,%s" % (ohjain, virtio[tyyppi[5:]])]
                k += ["-drive", "file=%s,format=%s,if=none,id=d%d%s" % (polku, muoto, i, ro),
                      "-device", "scsi-hd,drive=d%d,bus=%s.0,channel=0,scsi-id=%s,lun=%s%s"
                      % (i, ohjain, target, lun, bi)]
            elif laji in ("vblk", "vblk10"):
                k += ["-drive", "file=%s,format=%s,if=none,id=d%d%s" % (polku, muoto, i, ro),
                      "-device", "virtio-blk-pci,drive=d%d,%s%s" % (i, virtio[laji[4:]], bi)]
            elif laji == "cd":
                k += ["-drive", "file=%s,format=%s,if=none,media=cdrom,id=d%d%s" % (polku, muoto, i, ro),
                      "-device", "ide-cd,drive=d%d,bus=ahci.%d%s" % (i, i, bi)]
            else:
                k += ["-drive", "file=%s,format=%s,if=none,id=d%d%s" % (polku, muoto, i, ro),
                      "-device", "ide-hd,drive=d%d,bus=ahci.%d%s" % (i, i, bi)]
        return k + self.ylim

    def kaynnista(self):
        self.lokitiedosto = open(self.lokipolku, "ab")
        self.proc = subprocess.Popen(self.komento(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, bufsize=0)

    def _lue(self, aika):
        if self.proc.poll() is not None:
            loput = self.proc.stdout.read() or b""
            self._talleta(loput)
            raise VirheVM("QEMU päättyi (koodi %s)" % self.proc.returncode)
        r, _, _ = select.select([self.proc.stdout], [], [], aika)
        if r:
            data = os.read(self.proc.stdout.fileno(), 65536)
            if not data:
                self.proc.wait()
                raise VirheVM("QEMU päättyi (koodi %s)" % self.proc.returncode)
            self._talleta(data)

    def _talleta(self, data):
        if not data:
            return
        self.puskuri += data
        self.kaikki += data
        self.lokitiedosto.write(data)
        self.lokitiedosto.flush()

    def odota(self, malli, aikaraja=120):
        """Odota, kunnes konsolissa näkyy malli (regex); kuluttaa puskurin siihen asti."""
        rx = re.compile(malli.encode() if isinstance(malli, str) else malli)
        loppu = time.time() + aikaraja
        while True:
            m = rx.search(self.puskuri)
            if m:
                self.puskuri = self.puskuri[m.end():]
                return m
            jaljella = loppu - time.time()
            if jaljella <= 0:
                raise VirheVM("aikaraja odotettaessa %r; viimeksi:\n%s" %
                              (malli, self.puskuri[-1500:].decode("latin-1")))
            self._lue(min(jaljella, 1.0))

    def odota_jokin(self, mallit, aikaraja=120):
        """Odota ensimmäistä osuvaa mallia; palauttaa (indeksi, osuma)."""
        rxt = [re.compile(m.encode()) for m in mallit]
        loppu = time.time() + aikaraja
        while True:
            paras = None
            for i, rx in enumerate(rxt):
                m = rx.search(self.puskuri)
                if m and (paras is None or m.start() < paras[1].start()):
                    paras = (i, m)
            if paras:
                self.puskuri = self.puskuri[paras[1].end():]
                return paras
            jaljella = loppu - time.time()
            if jaljella <= 0:
                raise VirheVM("aikaraja odotettaessa %r; viimeksi:\n%s" %
                              (mallit, self.puskuri[-1500:].decode("latin-1")))
            self._lue(min(jaljella, 1.0))

    def laheta(self, teksti):
        # Hitaasti: 9frontin sarjaportin vastaanottopuskuri on pieni.
        for i in range(0, len(teksti), 16):
            self.proc.stdin.write(teksti[i:i + 16].encode())
            self.proc.stdin.flush()
            time.sleep(0.02)

    def aja(self, komento, aikaraja=120, sallittu_virhe=False):
        """
        Aja rc-komento konsolissa.  Palauttaa (tuloste, status).  Komento
        ei saa lukea konsolia.  Merkkijonot kootaan rc:ssä, jotta
        kaiutettu komentorivi ei osu loppumerkkiin.
        """
        tunnus = "%x" % (int(time.time() * 1000) & 0xffffff)
        alku = "P2A" + tunnus
        loppu = "P2L" + tunnus
        rivi = ("echo P2A^%s; { %s } </dev/null; echo P2L^%s^' '^$status^' 'P2Z\n"
                % (tunnus, komento, tunnus))
        self.laheta(rivi)
        self.odota(re.escape(alku) + r"\r?\n", aikaraja)
        m = self.odota(re.escape(loppu) + r" ?(.*?) P2Z", aikaraja)
        tuloste = self.kaikki_ennen(m)
        status = m.group(1).decode("latin-1").strip()
        if status and not sallittu_virhe:
            raise VirheVM("komento epäonnistui (%s): %s\n%s" % (status, komento, tuloste[-3000:]))
        return tuloste, status

    def kaikki_ennen(self, m):
        # odota() on jo kuluttanut puskurin; tuloste haetaan kokonaislokista
        data = self.kaikki
        a = data.rfind(b"P2A")
        a = data.find(b"\n", a) + 1
        b = data.rfind(b"P2L")
        return data[a:b].decode("latin-1").replace("\r", "")

    def sammuta(self, aikaraja=60):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            try:
                self.laheta("fshalt\n")
                self.odota(r"done halting|halted|cpu0: exiting", aikaraja)
            except VirheVM:
                pass
            time.sleep(2)
        self.tapa()

    def qmp(self, komento, aikaraja=10, **argumentit):
        """Yksi QMP-komento; palauttaa vastauksen 'return'-osan."""
        import json
        import socket
        if not self.qmppolku:
            raise VirheVM("kone käynnistettiin ilman QMP:tä")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(aikaraja)
        try:
            s.connect(self.qmppolku)
            f = s.makefile("rwb")

            def vastaus():
                while True:
                    rivi = f.readline()
                    if not rivi:
                        raise VirheVM("QMP-yhteys katkesi")
                    v = json.loads(rivi)
                    if "event" not in v:
                        return v
            vastaus()   # tervehdys
            for k, a in (("qmp_capabilities", {}), (komento, argumentit)):
                f.write(json.dumps({"execute": k, "arguments": a}).encode() + b"\n")
                f.flush()
                v = vastaus()
                if "error" in v:
                    raise VirheVM("QMP %s: %s" % (k, v["error"]))
            return v.get("return")
        except (OSError, ValueError) as e:
            raise VirheVM("QMP %s: %s" % (komento, e))
        finally:
            s.close()

    def lopeta(self, aikaraja=15):
        """QEMU pois QMP:n quit-komennolla (varalla terminate/kill)."""
        if self.proc is not None and self.proc.poll() is None and self.qmppolku:
            try:
                self.qmp("quit")
                self.proc.wait(aikaraja)
            except (VirheVM, subprocess.TimeoutExpired):
                pass
        self.tapa()

    def tapa(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.lokitiedosto:
            self.lokitiedosto.close()
            self.lokitiedosto = None
        shutil.rmtree(self.tyohak, ignore_errors=True)


def kaynnista_9front(vm, aikaraja=300):
    """
    Vie 9front-kone sarjakonsolin rc-kehotteeseen: vastaa ytimen ja
    /boot/bootin kysymyksiin (mouseport, monitor, vgasize, user, root) ja
    odottaa kehotetta.  Palauttaa havaitun kehotteen.
    """
    mallit = [
        r"mouseport is \([^)]*\) *\[[^\]]*\]:? *$",   # 0
        r"monitor is \([^)]*\) *\[[^\]]*\]:? *$",     # 1
        r"vgasize \[[^\]]*\]:? *$",                    # 2
        r"user *\[[^\]]*\]:? *$",                       # 3
        r"(term|cpu)% $",                               # 4
        r"\n[a-z0-9]+# $",                              # 5
        r"root is from \([^)]*\) *\[[^\]]*\]:? *$",   # 6
        r"panic: ",                                     # 7
    ]
    loppu = time.time() + aikaraja
    while True:
        i, m = vm.odota_jokin(mallit, max(1, loppu - time.time()))
        if i == 0:
            vm.laheta("ps2\n")
        elif i == 1:
            vm.laheta("vesa\n")
        elif i == 2:
            vm.laheta("text\n")
        elif i == 3:
            vm.laheta("glenda\n")
        elif i in (4, 5):
            # rio ei käynnisty, kun konsoli on sarjaportissa ilman näyttöä;
            # kehote riittää komentojen ajoon
            return m.group(0).decode().strip()
        elif i == 6:
            raise VirheVM("/boot/boot kysyy juurta: bootargs ei kelpaa\n" +
                          vm.kaikki[-2000:].decode("latin-1"))
        elif i == 7:
            vm.odota(r"\n", 5)
            raise VirheVM("ydin panikoi:\n" + vm.kaikki[-3000:].decode("latin-1"))


# --- yhteiset polut ja tiedostot (build, mkimage, boottest) ---

import hashlib
import io
import tarfile

TAMA = os.path.dirname(os.path.abspath(__file__))
JUURI = os.path.dirname(os.path.dirname(TAMA))

# README.md / tools/release.conf: pinnattu 9front-11952 (purettu ISO)
ISO_NIMI = "9front-11952.amd64.iso"
ISO_SHA256 = "ee90b5e8a8d6b4ee8ad950322f91e338f698dae18c7f5e5800a650bbfa173fd9"

# Codexin omistama yhteinen BootInfo v1 (README: Port boundaries)
JAETUT = [
    "sys/include/bootinfo.h",
    "sys/src/9/port/bootinfo.c",
    "sys/src/9/port/bootargs.c",
    "sys/src/9/port/bootfb.c",
]

# AMD64 ja yhteiset ytimen lähteet samasta 9front-overlaysta.
OVERLAY = ["sys/src/9/pc64", "sys/src/9/pc", "sys/src/9/port", "sys/src/9/ip", "sys/src/boot/efi", "sys/src/cmd/rc", "sys/src/cmd/aux/kbdfs", "sys/src/9/boot", "sys/man/8", "sys/src/libc/9syscall", "sys/src/ape/lib/ap/syscall"]


def overlay_jasenet(jaettu):
    """Sama lähdeoverlay käännöskoneeseen ja asennetulle levylle."""
    jasenet = []
    for hak in OVERLAY:
        lahde = os.path.join(JUURI, hak)
        if not os.path.isdir(lahde):
            raise VirheVM("overlay puuttuu: %s" % lahde)
        jasenet.append(("ov/" + hak, lahde))
    jasenet.append(("ov/sys/include/libc.h", os.path.join(JUURI, "sys/include/libc.h")))
    for p in JAETUT:
        lahde = os.path.join(jaettu, p)
        if not os.path.isfile(lahde):
            raise VirheVM("yhteinen BootInfo v1 puuttuu: %s" % lahde)
        jasenet.append(("ov/" + p, lahde))
    return jasenet


def valimuisti():
    return os.environ.get("PLAN2001_CACHE", os.path.expanduser("~/.cache/plan2001"))


def oletus_ulos():
    return os.environ.get("PLAN2001_NATIVE_OUT", os.path.join(valimuisti(), "native-amd64"))


def release_sha():
    conf = os.path.join(JUURI, "tools", "release.conf")
    if os.path.exists(conf):
        for rivi in open(conf):
            if rivi.startswith("ISO_SHA256="):
                return rivi.split("=", 1)[1].strip()
    return ISO_SHA256


def sha256(polku):
    h = hashlib.sha256()
    with open(polku, "rb") as f:
        for lohko in iter(lambda: f.read(1 << 20), b""):
            h.update(lohko)
    return h.hexdigest()


def etsi_iso(annettu=None):
    """Pinnattu ISO välimuistista; SHA256:n on täsmättävä (README)."""
    ehdokkaat = [annettu] if annettu else [
        os.path.join(os.environ.get("PLAN9WASM32_CACHE", os.path.expanduser("~/.cache/plan9-wasm32")), ISO_NIMI),
        os.path.join(valimuisti(), ISO_NIMI),
    ]
    odotettu = release_sha()
    for p in ehdokkaat:
        if p and os.path.exists(p):
            if sha256(p) != odotettu:
                raise VirheVM("%s: SHA256 ei täsmää pinnattuun (%s)" % (p, odotettu))
            return os.path.realpath(p)
    raise VirheVM("pinnattu %s puuttuu välimuistista (%s); tools/release hakee sen" %
                  (ISO_NIMI, ", ".join(e for e in ehdokkaat if e)))


def etsi_buildvm(annettu=None):
    p = annettu or os.environ.get("PLAN2001_BUILDVM", os.path.join(valimuisti(), "base.qcow2"))
    if not os.path.exists(p):
        raise VirheVM("9front-11952-käännöskone puuttuu: %s (PLAN2001_BUILDVM)" % p)
    return os.path.realpath(p)


def overlay_kone(pohja, tyohak, nimi):
    """Kertakäyttöinen qcow2-kerros pohjan päälle: pohja ei muutu."""
    polku = os.path.join(tyohak, nimi)
    muoto = "qcow2" if pohja.endswith(".qcow2") else "raw"
    subprocess.check_call(["qemu-img", "create", "-q", "-f", "qcow2", "-b", pohja, "-F", muoto, polku])
    return polku


def tar_levylle(polku, jasenet, koko=64 << 20):
    """
    Kirjoita tar (jasenet: lista (nimi tarissa, isäntäpolku tai bytes))
    raakalevykuvaksi, jonka 9front lukee: tar xf /dev/sdEn/data.
    """
    puskuri = io.BytesIO()
    with tarfile.open(fileobj=puskuri, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for nimi, lahde in jasenet:
            if isinstance(lahde, bytes):
                ti = tarfile.TarInfo(nimi)
                ti.size = len(lahde)
                ti.mode = 0o664
                ti.mtime = int(time.time())
                t.addfile(ti, io.BytesIO(lahde))
            else:
                t.add(lahde, arcname=nimi, recursive=True,
                      filter=lambda ti: _tar_suodata(ti))
    data = puskuri.getvalue()
    if len(data) > koko:
        koko = (len(data) + (1 << 20) - 1) & ~((1 << 20) - 1)
    with open(polku, "wb") as f:
        f.write(data)
        f.truncate(koko)
    return polku


def _tar_suodata(ti):
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    if ti.isdir():
        ti.mode = 0o775
    elif ti.mode & 0o111:
        ti.mode = 0o775
    else:
        ti.mode = 0o664
    return ti


def tyhja_levy(polku, koko=64 << 20):
    with open(polku, "wb") as f:
        f.truncate(koko)
    return polku


def tar_levylta(polku, kohde):
    """Pura 9frontin kirjoittama tar raakalevyltä (loppu nollia)."""
    os.makedirs(kohde, exist_ok=True)
    tulos = []
    with tarfile.open(polku, mode="r:") as t:
        for ti in t:
            if not ti.isfile() or "/" in ti.name or ti.name.startswith("."):
                continue
            with t.extractfile(ti) as lahde, open(os.path.join(kohde, ti.name), "wb") as f:
                shutil.copyfileobj(lahde, f)
            tulos.append(ti.name)
    return tulos


def aout_entry(polku):
    """9front a.out -otsakkeen sisääntulo (64-bittinen HDR_MAGIC: entry otsakkeen perässä)."""
    with open(polku, "rb") as f:
        h = f.read(40)
    import struct
    magic, teksti, data, bss, syms, entry, spsz, pcsz = struct.unpack(">8I", h[:32])
    if magic & 0x8000:
        entry = struct.unpack(">Q", h[32:40])[0]
    return magic, entry


def source_jasenet(tyohak, jaettu=JUURI):
    """Build from the same verified patch/overlay assembly as tools/tree."""
    tree = os.path.join(tyohak, "source")
    subprocess.check_call([os.path.join(JUURI, "tools/tree"), tree])
    if os.path.realpath(jaettu) != os.path.realpath(JUURI):
        for path in JAETUT:
            shutil.copyfile(os.path.join(jaettu, path), os.path.join(tree, path))
    return [("src", os.path.join(tree, "sys/src")),
            ("include", os.path.join(tree, "sys/include"))]
