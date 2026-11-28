"""
Kertakäyttöinen 9front-CPU/auth-vieras QEMU:ssa ja sen komennot natiivilla
9ptermillä (tools/clients/drawterm, Make.9pterm).  Sarjakonsoli jää
käynnistyksen ja vianetsinnän kanavaksi; käynnistyksen jälkeiset komennot
kulkevat rcpu:lla (dp9ik + TLS-PSK), eivät sarjaportin syötteenä.

    vieras = CpuVieras(levy, tyohak, loki)       # levy: kertakäyttöinen kopio
    vieras.kaynnista()                            # OVMF -> ... -> rc (sarja)
    vieras.provisioi()                            # yksi lyhyt sarjarivi
    t = vieras.pterm.aja("cat /dev/user", aikaraja=30)
    t.stdout, t.stderr, t.status                  # status: etäkomennon $status

Verkko: QEMU:n user-verkko restrict=on (vieras ei näe isännän verkkoja eikä
internetiä); vain CPU- (17019) ja auth-portti (567) ohjataan isännän
127.0.0.1:n satunnaisiin portteihin.  Lisäksi vieraalla voi olla
eristettyjä testiverkkokortteja tyhjässä QEMU-hubissa (testi_nicit).

Provisiointi on deterministinen: isäntä kirjoittaa rc-skriptin ja
satunnaiset kertakäyttötunnukset erilliselle raakalevylle (sdE1), ja
sarjakonsoliin kirjoitetaan vain "rc /dev/sdE1/data".  Skripti tekee saman
kuin browser-drawtermin test_e2e.py (auth/wrkey, palvelin-factotum, keyfs,
changeuser, aux/listen), mutta vastaukset annetaan putken kautta eikä
sarjaportista.  Salasanoja ei tulosteta eikä kirjoiteta lokiin;
tunnustiedostot ovat tilassa 0600 työhakemistossa, joka poistetaan.

Virheluokat: Siirtovirhe (9pterm ei saanut yhteyttä tai istunto katkesi,
paluuarvo 2), Aikaraja (-T ylittyi, 124) ja etäkomennon oma status
(Tulos.status, paluuarvo 1) pidetään erillään; mitään ei yritetä
uudelleen sokeasti.
"""

import collections
import os
import re
import secrets
import socket
import subprocess
import time

import vm9

TAMA = os.path.dirname(os.path.abspath(__file__))
JUURI = os.path.dirname(os.path.dirname(TAMA))

CPU_PORTTI = 17019
AUTH_PORTTI = 567
OHJAUS_MAC = "52:54:00:50:32:01"   # ohjausyhteyden kortti (user-verkko)

Tulos = collections.namedtuple("Tulos", "stdout stderr status koodi kesto")


class Siirtovirhe(vm9.VirheVM):
    """9pterm ei tavoittanut vierasta tai yhteys katkesi (ei vieraan virhe)."""


class Aikaraja(vm9.VirheVM):
    """Etäkomento ei päättynyt -T:n aikarajassa (9pterm: 124)."""


def vapaa_portti():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def etsi_9pterm():
    """9pterm: NINEPTERM-ympäristömuuttuja tai build.sh:n oletuspaikka."""
    p = os.environ.get("NINEPTERM") or os.path.join(
        JUURI, "tools", "clients", "drawterm", "build", "9pterm", "9pterm")
    if not os.access(p, os.X_OK):
        raise vm9.VirheVM("9pterm puuttuu: %s (tools/clients/drawterm/build.sh 9pterm "
                          "tai NINEPTERM=polku)" % p)
    return p


class Pterm:
    """Yksittäiset komennot 9pterm -c:llä; jokainen tunnistautuu erikseen."""

    def __init__(self, ohjelma, cpuportti, authportti, kayttaja, salasanatiedosto):
        self.ohjelma = ohjelma
        self.cpuportti = cpuportti
        self.authportti = authportti
        self.kayttaja = kayttaja
        self.salasanatiedosto = salasanatiedosto

    def put(self, paikallinen, eta, aikaraja=120, yhteysraja=30):
        """Tiedosto vieraaseen (9pterm put: MD5 tarkistetaan); binääri kulkee tätä tietä."""
        return self._suorita(["put", paikallinen, eta], "put " + eta, aikaraja, yhteysraja, b"")

    def aja(self, komento, aikaraja=60, yhteysraja=30, stdin=b""):
        """
        Aja rc-komento vieraassa.  stdin (bytes) menee komennolle, ja sen
        loppu on EOF.  Palauttaa Tuloksen; etäkomennon epätyhjä status ei
        ole poikkeus.  Siirtovirhe: yhteys tai tunnistautuminen epäonnistui
        tai istunto katkesi.  Aikaraja: komento ylitti aikarajan.
        """
        return self._suorita(["-c", komento], komento, aikaraja, yhteysraja, stdin)

    def _suorita(self, toimenpide, komento, aikaraja, yhteysraja, stdin):
        k = [self.ohjelma, "-h", "tcp!127.0.0.1!%d" % self.cpuportti,
             "-a", "tcp!127.0.0.1!%d" % self.authportti, "-u", self.kayttaja,
             "-P", self.salasanatiedosto, "-W", str(yhteysraja), "-T", str(aikaraja)] + toimenpide
        alku = time.time()
        try:
            # isännän oma vahti vain jumiutuneen 9ptermin varalle
            p = subprocess.run(k, input=stdin, capture_output=True,
                               timeout=yhteysraja + aikaraja + 30)
        except subprocess.TimeoutExpired:
            raise Siirtovirhe("9pterm ei päättynyt omassa aikarajassaan: %s" % komento[:200])
        kesto = time.time() - alku
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        if p.returncode == 0:
            return Tulos(out, err, "", 0, kesto)
        if p.returncode == 1:
            # 9pterm kirjoittaa etästatuksen stderr:n viimeiseksi riviksi
            rivit = err.rstrip("\n").split("\n")
            return Tulos(out, "\n".join(rivit[:-1]) + ("\n" if len(rivit) > 1 else ""),
                         rivit[-1] or "?", 1, kesto)
        if p.returncode == 124:
            raise Aikaraja("komento ylitti %d s: %s\n%s" % (aikaraja, komento[:200], err[-1000:]))
        raise Siirtovirhe("9pterm paluuarvo %d: %s\n%s" % (p.returncode, komento[:200], err[-1000:]))


def _rc_lainaus(s):
    return "'" + s.replace("'", "''") + "'"


def provisiointiskripti(tunnus, ohjaus_mac, userpw):
    """
    rc-skripti, jonka vieras ajaa sdE1:ltä.  Vastaukset auth/wrkeylle ja
    auth/changeuserille kulkevat putken kautta (readcons lukee /dev/consia
    tavu kerrallaan), joten sarjaporttiin ei kirjoiteta niitä.
    Kulku kuten tools/clients/browser-drawterm/test_e2e.py:n provision(),
    mutta authid on glenda (ks. wrkey alla).
    """
    for s in (userpw,):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", s):
            raise ValueError("salasanassa on rc:lle hankala merkki")
    # authid glenda: sama käyttäjä on hostowner (kmesg, #c/cons, fshalt)
    # ja rcpu-käyttäjä, kuten 9frontin yhden käyttäjän CPU/auth-palvelimessa
    wrkey = "glenda\n9front\n\n%s\n%s\nno\n" % (userpw, userpw)

    def changeuser(pw):
        # Password, Confirm, Inferno/POP, Expiration, Post id, full name,
        # Department, email, Sponsor, other email (tyhjä lopettaa)
        return "%s\n%s\nno\nnever\n\ntest\nnone\nnone\nnone\n\n" % (pw, pw)

    return "\n".join([
        "#!/bin/rc",
        "# Plan2001-testin kertakäyttöinen CPU/auth-provisiointi (cpuvieras.py).",
        "# Älä tulosta tunnuksia: tämän skriptin tuloste menee sarjalokiin.",
        "rfork e",
        "fn virhe { echo P2CPU-VIRHE " + tunnus + " $*; exit virhe }",
        "fn vaihe { echo P2CPU-VAIHE " + tunnus + " $* }",
        # konsolilla VASTAUKSET KOMENTO...: KOMENTO lukee /dev/consin putkesta
        "fn konsolilla {",
        "\tvast=$1",
        "\tshift",
        "\t@{",
        "\t\trfork n",
        "\t\tbind '#|' /tmp/p2pipe",
        "\t\tbind /dev/null /dev/consctl",
        # Putken kumpikin pää pidetään auki koko ajan: devpipe sulkee jonon,
        # kun pään viimeinen avaus suljetaan, ja readcons avaa /dev/consin
        # joka kehotteelle uudelleen.  Kirjoittaja pitää datan pään.
        "\t\t{ echo -n $vast; exec sleep 600 } >/tmp/p2pipe/data &",
        "\t\tkirj=$apid",
        "\t\tbind /tmp/p2pipe/data1 /dev/cons",
        "\t\t$* <[9]/dev/cons >/dev/null",
        "\t\tst=$status",
        "\t\techo kill >/proc/$kirj/note",
        "\t\texit $st",
        "\t}",
        "}",
        "mkdir -p /tmp/p2pipe /tmp/svc || virhe mkdir",
        "vaihe verkko",
        "ohj=()",
        "for(e in /net/ether*)",
        "\tif(~ `{cat $e/addr} " + ohjaus_mac.replace(":", "").lower() + ") ohj=$e",
        "if(~ $#ohj 0) virhe ohjauskortti puuttuu",
        "ip/ipconfig ether $ohj >[2]/tmp/ipconfig.log || virhe ipconfig",
        "vaihe wrkey",
        "konsolilla " + _rc_lainaus(wrkey) + " auth/wrkey || virhe wrkey",
        # Käynnistyksen factotum ei lukenut nvramia (ei service=cpu);
        # palvelin-factotum -S korvaa sen /srv/factotumissa.
        "vaihe factotum",
        "rm -f /srv/factotum",
        "auth/factotum -S -n -sfactotum >[2]/tmp/fact.log &",
        "for(i in `{seq 1 100}) if(! test -e /srv/factotum) sleep 0.1",
        "test -e /srv/factotum || virhe factotum",
        "vaihe keyfs",
        "auth/keyfs -m /mnt/keys /adm/keys >[2]/tmp/keyfs.log &",
        "for(i in `{seq 1 100}) if(! ns | grep -s ' /mnt/keys( |$)') sleep 0.1",
        "ns | grep -s ' /mnt/keys( |$)' || virhe keyfs",
        "vaihe changeuser",
        "konsolilla " + _rc_lainaus(changeuser(userpw)) + " auth/changeuser glenda || virhe changeuser glenda",
        "vaihe listen",
        "cp /rc/bin/service.auth/tcp567 /rc/bin/service/tcp17019 /tmp/svc || virhe svc",
        "@{ echo -n cpu >/env/service; aux/listen -q -t /tmp/svc tcp >[2]/tmp/listen.log & }",
        "for(p in 17019 567){",
        "\tfor(i in `{seq 1 100}) if(! grep -s '!'^$p^'$' /net/tcp/*/local >[2]/dev/null) sleep 0.1",
        "\tgrep -s '!'^$p^'$' /net/tcp/*/local >[2]/dev/null || virhe listen $p",
        "}",
        "echo P2CPU-VALMIS " + tunnus,
        "exit",
        "",
    ])


class CpuVieras:
    """
    QEMU-vieras levykuvasta (raw tai qcow2, jonka kutsuja on jo kopioinut
    kertakäyttöiseksi), ohjausverkko restrict=on ja loopback-porttiohjaukset.
    testi_nicit: lista MAC-osoitteita eristetyille testikorteille, jotka
    liitetään tyhjään QEMU-hubiin (ei yhteyttä minnekään).
    """

    def __init__(self, levy, tyohak, loki, muoto="raw", testi_nicit=(), muisti="2048", cpu="2",
                 laji="hd", kone="q35", provisiointi=("hd", "sdE1"), lisalevyt=(), lisaportit=()):
        self.tyohak = tyohak
        self.cpuportti = vapaa_portti()
        self.authportti = vapaa_portti()
        self.tunnus = secrets.token_hex(6)
        self.userpw = "u" + secrets.token_hex(10)  # 9front: enintään 27 merkkiä
        self.salasanatiedosto = os.path.join(tyohak, "pterm.pass")
        fd = os.open(self.salasanatiedosto, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.write(fd, (self.userpw + "\n").encode())
        os.close(fd)
        self.kiinnike = os.path.join(tyohak, "provisiointi.img")
        data = provisiointiskripti(self.tunnus, OHJAUS_MAC, self.userpw).encode()
        # rc lopettaa exit-riviin; täyte rivinvaihtoja eikä nollia
        koko = max(1 << 20, (len(data) + 511) & ~511)
        fd = os.open(self.kiinnike, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.write(fd, data + b"\n" * (koko - len(data)))
        os.close(fd)
        # lisaportit: vieraan TCP-portit, joille isännän 127.0.0.1:n vapaa
        # portti (self.portit[vieras] = isäntä), esim. testin HTTP-palvelu
        self.portit = {p: vapaa_portti() for p in lisaportit}
        verkko = ["-netdev", "user,id=ohj,restrict=on,"
                  "hostfwd=tcp:127.0.0.1:%d-:%d,hostfwd=tcp:127.0.0.1:%d-:%d"
                  % (self.cpuportti, CPU_PORTTI, self.authportti, AUTH_PORTTI)
                  + "".join(",hostfwd=tcp:127.0.0.1:%d-:%d" % (h, v) for v, h in self.portit.items()),
                  "-device", "virtio-net-pci,netdev=ohj,mac=" + OHJAUS_MAC
                  # PLAN2001_VIRTIONET=",disable-legacy=on": virtio 1.0 -kortti
                  # (ethervirtio10) legacy-ajurin (ethervirtio) sijaan
                  + os.environ.get("PLAN2001_VIRTIONET", "")]
        for i, mac in enumerate(testi_nicit):
            verkko += ["-netdev", "hubport,id=t%d,hubid=%d" % (i, 10 + i),
                       "-device", "virtio-net-pci,netdev=t%d,mac=%s" % (i, mac)]
        # provisiointi: (vm9.Kone-laji, levyn nimi vieraassa); oletus AHCI-portti 1 (sdE1)
        self.provisiointilaji, self.provisiointilevy = provisiointi
        # lisalevyt: vm9.Kone-levyt provisiointilevyn jälkeen (esim. virtio-testilevyt)
        self.vm = vm9.Kone([(levy, muoto, False, laji), (self.kiinnike, "raw", False, self.provisiointilaji)]
                           + list(lisalevyt),
                           vm9.Ovmf.etsi(), loki, muisti=muisti, cpu=cpu, ylim=verkko, qmp=True,
                           kone=kone)
        self.pterm = Pterm(etsi_9pterm(), self.cpuportti, self.authportti, "glenda",
                           self.salasanatiedosto)

    def kaynnista(self, aikaraja=300):
        """Käynnistä ja vie rc-kehotteeseen (sarjakonsoli); palauttaa kehotteen."""
        self.vm.kaynnista()
        return vm9.kaynnista_9front(self.vm, aikaraja)

    def provisioi(self, aikaraja=180):
        """
        CPU/auth-palvelut pystyyn yhdellä lyhyellä sarjarivillä.  Jos rc:n
        kaiku ei vastaa riviä, sarjaportti vääristi sen: Siirtovirhe.
        Skriptin virhe tai jumi vieraassa on VirheVM.  Ei uusintayritystä.
        """
        rivi = "rc /dev/%s/data" % self.provisiointilevy
        self.vm.laheta(rivi + "\n")
        try:
            # rc kaiuttaa rivin: vääristynyt kaiku on sarjasiirron virhe
            m = self.vm.odota(r"[^\r\n]*\r?\n", 20)
        except vm9.VirheVM as e:
            raise Siirtovirhe("provisiointirivin kaiku puuttuu: %s" % e)
        kaiku = m.group(0).decode("latin-1").strip()
        if not kaiku.endswith(rivi):
            raise Siirtovirhe("sarjaportti vääristi provisiointirivin: %r" % kaiku)
        i, m = self.vm.odota_jokin([r"P2CPU-VALMIS " + self.tunnus,
                                    r"P2CPU-VIRHE " + self.tunnus + r" ([^\r\n]*)\r?\n",
                                    r"/dev/%s/data:[^\r\n]*\r?\n" % self.provisiointilevy], aikaraja)
        if i != 0:
            raise vm9.VirheVM("provisiointi epäonnistui vieraassa: %s"
                              % m.group(1 if i == 1 else 0).decode("latin-1").strip())
        # palvelut kuuntelevat; ensimmäinen yhteys todentaa koko ketjun
        return self.pterm.aja("cat /dev/user", aikaraja=30)

    def sammuta(self, aikaraja=120):
        """
        Tiedostojärjestelmä alas 9ptermin kautta (fshalt omassa
        note-ryhmässään, jottei rcpu-istunnon loppu tapa sitä), odota
        sarjakonsolista 'done halting' ja lopeta QEMU QMP:llä.
        Palauttaa True, jos fshalt valmistui.
        """
        valmis = False
        try:
            # tuloste ytimen konsoliin ('#c/cons' = sarjaportti), ei rcpu:n /dev/consiin
            self.pterm.aja("@{ rfork s; sleep 1; fshalt } </dev/null >'#c/cons' >[2=1] &", aikaraja=30)
            self.vm.odota(r"done halting", aikaraja)
            valmis = True
        except vm9.VirheVM:
            pass
        self.vm.lopeta()
        return valmis

    def siivoa(self):
        self.vm.tapa()
        for p in (self.salasanatiedosto, self.kiinnike):
            try:
                os.unlink(p)
            except OSError:
                pass
