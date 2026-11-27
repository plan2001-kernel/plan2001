# Installing Plan9-wasm32

As 9front is installed on a PC: boot the live image, run `inst/start`,
reboot from the disk; then the machine builds its own system from its
source contained in the distribution, the locked 9front revision (`upstream/9front.lock`).

## The distribution

A binary base, the tools, and the whole source with its git:

- **The source** is this repository's `tree` branch (`tools/treebranch`),
  a snapshot: 9front's release (its commit's files, not its history -
  9front's own is `github.com/9front/9front`) and this tree's files over
  it (the port, 3c/3a/3l, the patched files), without 9front's jokes and
  quotations (`tools/dist.deny`). `tools/treebranch` records overlay source
  snapshots over the locked 9front revision. Its root is the machine's `/`.
- **The ISO** (`tools/dist`: `build/dist/plan9-wasm32.iso`, plan9port's
  dump9660, `-c9j` as 9front's mkfile) has the branch's files, and built
  for wasm32 only the base system and the tools (`tools/dist.base`: the
  file servers, rc and the commands the live system, termrc, rio and
  inst/start run, the network, an editor; 3c/3a/3l, mk, yacc, lex, ar,
  git), the libraries and the boot materials, the directories mk install
  puts the rest in (/wasm32/bin/games ...) - and in
  `/dist/9front/.git` the branch, its work tree the root, as 9front's
  ISO has its repository.  `tools/dist` builds the whole
  release here too (`build/dist/failed`: what does not build yet): a
  check of the source, and where the base comes from.
- **Live distribution:** serve `build/dist/live` with
  `tools/serve -d build/dist/live`. The page reads its configured ISO with
  HTTP Range requests, or accepts a local ISO with `?iso=file`. It can
  also boot an installed system from the browser's disk. The page and
  kernel are the browser guest's firmware, independently of the HTTP
  host's operating system.

After the install, on the machine (as 9front's sysupdate):

    bind -ac /dist/9front /
    cd /sys/src
    mk install                  # the rest of the system, built here

To update the overlay from the distribution's configured source remote,
keep the distribution's locked 9front revision and rebuild the tools first. Network
access requires a configured gateway (`docs/network.md`); `-n` provides
an isolated test network without Internet access:

    bind -ac /dist/9front /
    cd / && git/pull            # the configured distribution source remote
    cd /sys/src/cmd
    for(i in cc 3c 3a 3l cpp mk) @{cd $i && mk install}
    mk yacc.install             # the tools first
    cd /sys/src && mk install

Rebuild the compilers before the rest of the system when the overlay
requires newer compiler support. The installation medium includes the
source repository at `/dist/9front`; it can be restored from there
without fetching another release.

The kernel, once built (`cd /sys/src/9/wasm32; mk install`), goes onto
the boot FAT as inst/bootsetup puts it there.

## The live image

The live image is the page, the kernel, its boot file system, plan9.ini
and the ISO: `build/dist/live`.

    PLAN9=$HOME/plan9port tools/dist
    tools/serve -d build/dist/live     # http://127.0.0.1:18300/

plan9.ini there says `cdboot=yes` and `iso=plan9-wasm32.iso`.  The page
(the firmware) makes the ISO the machine's read-only disk `#S/sdW1`,
read from the server with HTTP range requests; or, with `?iso=file`, a
copy of the ISO on the person's own computer, chosen on the page (read
with FileReaderSync, nothing downloaded; the server gives the page, the
kernel and its boot file system only) - tools/test/run's isofile.  `/boot/init` does what
9front's bootrc does with `cdboot`: the CD's file system (9660srv) is the
root, and 9front's own init runs from it - `/lib/namespace`, termrc,
glenda's profile, rio.  The live system is a guest: nothing is written.

## inst/start

9front's installer, as it is, from a rio window or the console:

    inst/start

- **configfs**: hjfs, gefs or cwfs64x.
- **partdisk**: the disk is `sdW0`, a file in the origin's private file
  system (OPFS, 4 GB by default, `?disk=MB`); `gpt`, then edisk's
  suggestion (`w`, `q`): esp and plan9.
- **prepdisk**: prep's suggestion: 9fat, nvram, fs (or the esp as 9fat).
- **mountfs**.
- **confignet**: the Ethernet card, `/net/ether0` (`docs/kernel.md`):
  automatic (DHCP from the gateway's network) or manual.
- **mountdist** (`/`: the live root), **copydist** (the whole
  distribution, from the ISO onto the disk).
- **ndbsetup**: the machine's name and its card's address in the
  installed `/lib/ndb/local`, so that termrc configures it when it boots
  (the address is the browser's, the same each time).
- **tzsetup**.
- **bootsetup**: the boot FAT (9fat, or the esp) gets plan9.ini
  (`bootfile=9wasm32.wasm`, `bootargs=local!/dev/sdW0/fs ...`), the kernel
  and `/wasm32/9fat`: the kernel's boot file system (`9wasm32.fs`) and the
  page that is its firmware (`index.html`, `platform.js`) -
  `patches/9front/inst-bootsetup-9fat.diff`.
- **finish**: `fshalt -r`; the kernel flushes the disk and the page loads
  itself again (`#c/reboot`).

## Booting from the disk

The page at the server is the loader.  It looks at the disk first, as a
PC's firmware does (`platform.js`'s `diskboot`): the GPT, then 9fat in the
plan9 partition (prep's table) or the esp, and on it plan9.ini, the kernel
and its boot file system.  If the FAT has its own firmware (`index.html`,
`platform.js`: bootsetup put them there), the page gives the machine over
to it - the disk's page and platform run the disk's kernel - so the system
is the disk's until it is installed again (`window.plan9firmware` is
`disk` there).

`/boot/init` then mounts `bootargs`' file system (hjfs on `/dev/sdW0/fs`)
as the root and runs 9front's init from it.  The root's file server runs
in a name space of its own: the root, bound before `/` for 9front's init,
is not in the server's, or its halt (fshalt) would walk through itself.

`?boot=net` boots the live image again (to install anew); `?disk=0` boots
without the disk.  `?boot=` and `?iso=` are for one boot, as a PC boots
once from its CD: the reboot (`fshalt -r`, the installer's finish) loads
the page without them, so the installed system boots from its disk.

## The tests

`tools/test/run install` (with `build/dist` there): `tools/test/iso` makes a
small distribution (what the installer, termrc and rio run) and its live
image; inst/start is answered on the console, `fshalt -r` reboots, and the
page boots from the disk with the disk's firmware, the root the installed
hjfs.  `PROFILE=dir` keeps the browser's profile, and with it the disk,
between runs of `tools/test/wasmapp`.

`tools/test/run dist`, only when named (the ISO is large): the
distribution's own ISO installed from this computer (`?iso=file`), then
on the disk its git as sysupdate has it (`bind -ac /dist/9front /`: the
branch, at origin, the work tree clean) and a small build with the
machine's own tools - echo and hoc from `/sys/src/cmd` (mk, 3c, 3l,
yacc), installed and run.  The whole system's build (`cd /sys/src; mk
install`) is people's to run.

`tools/test/run pwa`, only when named: the small distribution installed
into a browser profile kept for it (`build/test/pwaprofile`), then
`tools/test/pwaoffline`: online, the service worker caches this
version's page and `platform.js` completely and the installed system
boots from its disk; with the server and the browser stopped, a cold
start of the start URL boots the installed system again and reads back
what it wrote, and `?boot=net` fails visibly instead of starting
something else; an update replaces the cache, and an update with a
missing file is rejected while the last complete version still starts
offline.  The browser's disk is never touched outside that profile.

The browser tests run headless Chromium with `--password-store=basic`:
on a desktop whose GNOME keyring is locked, Chromium otherwise waits for
the keyring and never loads the local page.

## AMD64 UEFI disk image

    tools/native-amd64/build --out O
    tools/native-amd64/mkimage --in O --out O.img
    tools/clients/drawterm/build.sh 9pterm
    tools/native-amd64/boottest --image O.img

`boottest` works on a sparse disposable copy; the image itself is not
modified. It boots the copy three times: offline (NIC only on an empty
QEMU hub; loader, BootInfo, kernel, devices, init and rc observed on the
serial console), networked, and offline again. The networked boot is a
throwaway CPU/auth guest (`tools/native-amd64/cpuvieras.py`): QEMU user
networking with `restrict=on`, only the rcpu and auth ports forwarded to
random `127.0.0.1` ports, random one-off credentials, and a second NIC on
an empty hub for IPv4/IPv6/ARP checks. One short serial line runs a
provisioning script from a separate raw disk; every later command runs
through native 9pterm (`-c` with `-W`/`-T`, stdout, stderr and remote
status kept apart, text stdin ending in EOF, binary files with `put`).
The last boot verifies, without network or serial input, a marker written
during the networked boot. `--offline` runs only the first boot. Results
are `OK`, `FAIL` (wrong guest result), `SIIRTO` (9pterm or serial could
not reach the guest) or `AIKA` (remote command timed out); nothing is
retried. The serial log is written next to the image
(`O.boottest.log`) and contains no credentials.

### Stopping with the power button

A graceful stop presses the ACPI power button: Oracle Cloud's `SOFTSTOP`
(console or `oci compute instance action --action SOFTSTOP`), QEMU's
`system_powerdown`. The kernel reports each press as a line, `power
button`, on `#P/acpievent` (`/dev/acpievent`, readable only by the host
owner); a read blocks until a press. cpurc starts a reader that runs
`fshalt`: the file servers flush and halt, then `scram` powers the machine
off, and the next boot finds cwfs clean. termrc does the same only with
`acpipwrbtn=halt` in plan9.ini; `acpipwrbtn=ignore` turns it off on a cpu
server.

The first read arms the button: the kernel switches the firmware to ACPI
mode if it is not already in it (`SMI_CMD`), enables only the button's
event, disables the GPEs and handles the SCI. Until then, and on a machine
without the file, nothing changes. The file exists only when the FADT
describes a fixed-feature power button with an SCI on an ISA interrupt
(not with `PWR_BUTTON` or `HW_REDUCED_ACPI` set, and not with `*acpi=0`
or `*nomp`).

`fshalt` needs a working userland. A hung system does not halt; stop it
with `STOP` (force) instead, since an instance in `STOPPING` after a
`SOFTSTOP` refuses every other action, force stop included, until Oracle
Cloud's graceful-shutdown timeout cuts the power.

## ARM64 QEMU virt live image

The serial live image uses upstream's `sys/src/9/arm64` QEMU
board and native `7c`, `7a`, `7l`, `mk`, and `aux/aout2efi`. Linux needs
QEMU for AMD64 (the cross-build VM) and AArch64 (the target), OVMF,
AAVMF/EDK2 and mtools. The sources are the assembled tree (`tools/tree`) and
the cross-compilers are those `tools/native-amd64/build` built from it
(`root.tar`); `PLAN2001_BUILDVM` selects the same native build VM used
by `tools/native-amd64/build` (default `~/.cache/plan2001/base.qcow2`).

    tools/native-arm64/build
    tools/native-arm64/mkimage
    tools/native-arm64/boottest

Artifacts and serial captures stay under `build/native-arm64`. `mkimage`
creates a new 128 MiB FAT image containing `EFI/BOOT/BOOTAA64.EFI`,
`9qemu` and `plan9.ini` (`efiplatform=qemu-virt`); it refuses to overwrite an existing path. Use
`--build` and `--image` to select another artifact directory or image.
`boottest --rpi4-refusal` boots the same loader with `efiplatform=rpi4`
and checks its refusals and its device-tree validation in the real
relocated EFI image (docs/rpi4.md).
For SMP, pass the same `--cpus N` (1–16) to `mkimage` and `boottest`;
the test checks the guest CPU count before exercising storage and networking.
`build` accepts `--out`, `--iso` and `--buildvm`; `--loader-only` rebuilds
just `bootaa64.efi` for loader development. Native builds consume the
verified `tools/tree` source assembly and build libc with the
architecture-aware syscall selector.

The target command uses `virt,gic-version=3,highmem=off`, a Cortex-A72,
one CPU and 1024 MiB RAM. These settings match the board's GICv3,
PCI ECAM and MMIO addresses. The virtio PCI devices use
`disable-legacy=on`, matching the board's virtio 1.0 drivers. `AAVMF_CODE` and `AAVMF_VARS` override the
firmware paths. The firmware variables and test disk are disposable; the
boot image is attached read-only. The firmware loads the existing EFI
loader, which passes the shared BootInfo v1 physical pointer in X0.

The embedded paqfs live root contains 9front init, rc and basic commands;
`ramfs` supplies writable temporary files and `hjfs` can use a disposable
virtio disk. The boot volume is not an installed writable root. The
initial direct map covers QEMU RAM below physical `0x80000000`; memory
allocation uses its largest conventional UEFI RAM bank, retaining all
firmware and loader allocations. Other boards, additional CPUs, larger
RAM configurations and graphical consoles require separate validation.
