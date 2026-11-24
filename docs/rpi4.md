# Raspberry Pi 4 (bcm64)

The Raspberry Pi 4 (BCM2711) runs 9front-11952's own `bcm64` port: the
`pi4` kernel configuration (`sys/src/9/bcm64/pi4`, kernel `9pi4`) and the
ordinary 9front arm64 userland, built with 9front's own compilers
(`mk`, `7c`, `7a`, `7l`). The QEMU `virt` ARM64 platform is a separate
target; a `virt` boot says nothing about the Pi 4.

## Boot stages

Each stage is tested and reported on its own:

| Stage | Chain | Tool |
|---|---|---|
| (a) stock 9front Pi 4 boot | VideoCore firmware (`start4cd.elf`, `config.txt`) → `9pi4` at physical `0x80000`, X0 = DTB | `tools/rpi4/build` image; release image `tools/rpi4/stock` |
| (b) Pi 4 UEFI/EDK2 launch | VideoCore firmware → `RPI_EFI.fd` (TF-A + EDK2) → `EFI/BOOT/BOOTAA64.EFI` | `tools/rpi4/uefi-esp` |
| (c) Plan2001 BootInfo v1 hand-off | EDK2 → Plan2001 EFI loader → `9pi4efi`, X0 = BootInfo | `tools/rpi4/uefi-esp --base`; loader with the Pi 4 platform (below); `tools/rpi4/bootinfotest` (emulated entry) |
| (d) 9front kernel → init → rc | after (a) or (c) | hardware; `tools/rpi4/boottest` (emulated, partial) |

Stage (a) uses the Raspberry Pi firmware's `kernel8`-style boot. It is
the upstream baseline for diagnosing the hardware and the port; it is not
Plan2001's UEFI boot, which remains the native boot policy (README).

## Building

    tools/rpi4/build            # kernels, arm64 userland, SD image
    tools/rpi4/build --no-image # kernels and userland only
    tools/rpi4/boottest         # QEMU raspi4b/raspi3b serial smoke test
    tools/rpi4/stock            # 9front-11952.pi3.img, SHA256-checked
    tools/rpi4/bootinfotest     # 9pi4efi's BootInfo entry in QEMU raspi4b

`tools/rpi4/build` assembles the pinned release with `tools/tree` before
transferring the complete patched source and headers to the 9front-11952 build VM of
`tools/native-amd64` (`PLAN2001_BUILDVM`, default
`~/.cache/plan2001/base.qcow2`) with no network, from the pinned ISO
(SHA256-checked) with this tree's files over it (`tools/tree` must
succeed first). In the VM it runs `objtype=arm64 mk install` in
`/sys/src`, builds and installs `9pi4`, `9pi3` and `9pi4efi` (each
after `mk clean`: `9pi4efi`'s objects are built with `-DUEFI`), builds
the shared `bootaa64.efi` from `/sys/src/boot/efi` as it is, and makes the SD
image with 9front's own `/sys/lib/dist/mkfile` rule `%.pi3.img`. The Pi
firmware files and `bcm2711-rpi-4-b.dtb` are the release's
`/sys/src/boot/bcm` files; nothing is fetched.

Output (default `~/.cache/plan2001/rpi4`, or `--out`/`PLAN2001_RPI4_OUT`):

- `9pi4`, `s9pi4` (with symbols), `9pi3`
- `9pi4efi`, `s9pi4efi`: the UEFI kernel (below); `build` checks its
  a.out header and layout against the loader contract
- `bootaa64.efi`: the shared ARM64 EFI loader of this tree
- `plan2001-rpi4.img`: MBR, partition 1 FAT32 (active; `config.txt`,
  `cmdline.txt`, firmware, DTB, kernels), partition 2 Plan 9 (`nvram`,
  `fs`: hjfs with the arm64 distribution)
- `build.log`

Its `config.txt` is 9front's: `[pi4] kernel=9pi4 arm_64bit=1`,
`enable_uart=1`, `core_freq=250`; `cmdline.txt` is `console=0`.

`tools/rpi4/boottest` runs the results in QEMU with the console on the
mini UART (QEMU's second serial port). QEMU loads the kernel as the Pi
firmware does (physical `0x80000`, X0 = DTB) but does not run
`start4cd.elf` or read `config.txt`.

- `raspi4b`, `9pi4`: the kernel prints its memory map and banner, then
  stops. QEMU sets `CBAR_EL1` to the peripheral base (`0xfe000000`);
  `bcm64/gic.c` derives the GIC address from it, which on a BCM2711 is in
  the ARM-local block, so the kernel takes an external abort before its
  vectors are set. QEMU's `raspi4b` has no PCIe (USB) or GENET either.
- `raspi3b`, `9pi3` from the same image: the same `bcm64` port and arm64
  userland boot from the card's hjfs to rc, and `7c`/`7l` build and run a
  program.

Neither is a Pi 4 boot. `tools/rpi4/stock` fetches the release's own
`9front-11952.pi3.img` (SHA256 from the release announcement) into
`~/.cache/plan2001/rpi4-stock` for comparison on the same hardware.

## Writing the SD card (owner action)

No tool here writes to a device. Copying the image to a card erases the
card. Identify the card by its size and model, not by its name:

    lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
    sudo dd if=~/.cache/plan2001/rpi4/plan2001-rpi4.img of=/dev/sdX \
        bs=4M conv=fsync status=progress

`/dev/sdX` is the whole card (not a partition), not mounted. The image
is 2 GB; the card may be larger.

## Serial console

3.3 V TTL UART, 115200 8N1, no flow control: GPIO14 (TXD, header pin 8),
GPIO15 (RXD, pin 10), GND (pin 6). With `enable_uart=1` the firmware puts
the mini UART on these pins; `console=0` is the kernel's first UART.

    picocom -b 115200 /dev/ttyUSB0

Expected on the console: the memory map, `Plan 9`, `cpu0: ... Cortex-A72`
and memory, then `bootargs is (tcp, tls, il, local!device) [...]` with the
SD card's `fs` partition as default (Enter), user `glenda`. Check after
boot:

    cat '#c/kmesg'; cat '#S/sdctl'; ls '#l'; cat '#$/pci/'*ctl; cat /dev/sysname

## Rollback

The Pi 4 boots whatever is on its boot medium. To return to the previous
system, put the previous card back, or write its saved image to the card
with the same `dd` command. The EEPROM bootloader is not changed by these
images; do not run `rpi-eeprom-update` for these tests.

## UEFI (EDK2) on the Pi 4

The external prerequisite is the community EDK2 port **pftf/RPi4
v1.53**, `RPi4_UEFI_Firmware_v1.53.zip`, SHA256
`ca9973e2a7a546b3df871cfb7382e656829114b6dfa424f40dc67cc90a217d88`.
`tools/rpi4/uefi-esp` checks it, writes an MBR/FAT32 image with it
(`~/.cache/plan2001/rpi4-uefi/plan2001-rpi4-uefi.img`) and, with
`--loader`, the Plan2001 EFI loader as `EFI/BOOT/BOOTAA64.EFI`. Its
`config.txt` is the firmware's: `armstub=RPI_EFI.fd`,
`device_tree_address=0x3e0000`, `enable_gic=1`, PL011 on GPIO14/15
(`dtoverlay=miniuart-bt`). The EDK2 default limits RAM to 3 GB until
changed in its setup menu (Device Manager → Raspberry Pi Configuration
→ Advanced Configuration). Its settings are kept in `RPI_EFI.fd` on the
card.

`tools/rpi4/uefi-esp --base IMG --loader BOOTAA64.EFI --kernel 9pi4efi`
makes the whole card for stage (c): a copy of `plan2001-rpi4.img` whose
Plan 9 partition (the hjfs root) is kept and whose FAT partition is
formatted again with the firmware above, `EFI/BOOT/BOOTAA64.EFI`,
`9pi4efi` and `plan9.ini` (`bootfile=/9pi4efi`, `console=1`,
`efiplatform=rpi4`, `efibootmap=0,0x40000000`,
`efiscratch=0x1000000,0x80000`). The latter settings are checked by the
loader and must match the kernel's early map and pre-text scratch.

EDK2 settings the kernel needs, in its setup menu (Esc at boot; kept in
`RPI_EFI.fd` on the card):

- Device Manager → Raspberry Pi Configuration → ACPI / Device Tree →
  System Table Selection: **ACPI+Devicetree** (or Devicetree). The
  default, ACPI only, gives the loader no device tree; the loader then
  stops with `firmware did not provide an FDT (EDK2: System Table
  Selection must include Devicetree)` before loading the kernel.
- Advanced Configuration → Limit RAM to 3 GB: either. With the limit,
  the kernel has 3 GB.

Checkpoints on the serial console for stages (c) and (d), in order
(owner-run, physical Pi 4; the card from `uefi-esp --base`):

1. EDK2's own messages, then `[P2 L01] Plan2001 loader`, the
   `plan9.ini` lines above and `[P2 L13] kernel opened`.
2. `[P2 L16]` kernel sizes, `[P2 L17] kernel memory claimed executable`,
   `[P2 L18] entry state checked`. A loader refusal instead names the
   failed check (device tree, memory map, image layout or `plan9.ini`)
   and returns to the loader prompt; nothing has been written to RAM.
3. `pi4efi: BootInfo v1 at 0x...`, then `Plan 9`, the memory banks and
   `cpu0` to `cpu3` (PSCI `CPU_ON`; only `cpu0` if the firmware's tree
   has no `/psci`). A `pi4efi:` line followed by `halted` names the
   kernel's refusal (docs above).
4. `bootargs is (...)` with the SD card's `fs` partition as default
   (Enter), user `glenda`, the rc prompt. Then `cat '#c/sysstat'` (one line per CPU), `cat
   '#S/sdctl'`, `ls '#l'` (GENET ether0), `ls '#u/usb'` and `fshalt -r`
   (PSCI `SYSTEM_RESET`).

Rollback is as above: the Pi 4 boots whatever is on the card, and the
EEPROM is not changed.

### The UEFI kernel `9pi4efi`

`CONF=pi4efi` is the `pi4` configuration built with `-DUEFI`
(`sys/src/9/bcm64/uefi.c`, `patches/9front/bcm64-uefi-bootinfo.diff`).
The stock `9pi4` stays the firmware-boot kernel; neither takes the
other's entry.

- **Layout.** `mem.h`'s `PHYSBASE` (`0x01000000`) moves the stock low
  area up past TF-A and EDK2 (`0x0`–`0x400000`): spin table `+0xd8`,
  boot arguments `+0x100`, reboot code `+0x1c00`, mailbox buffer
  `+0x3400`, page tables and `Mach` structures below the text at
  `+0x80000`. `KZERO` still maps physical 0, so `PADDR`, `KADDR` and DMA
  bus addresses are those of the stock kernel.
- **Loader contract.** The primary image is a native 9front arm64 a.out (magic
  `0x8c47`), entry `0xffffffffc1080000` = first text byte, loaded at
  physical entry − `0xffffffffc0000000`, data at the next 64 KiB
  boundary, then bss. The shared loader also accepts a strictly checked
  linked AArch64 ELF64 ET_EXEC image. It claims the image and the explicit
  pre-text scratch `[0x1000000,0x1080000)` as loader memory; together
  these protect `[PHYSBASE,end)` from the kernel allocator. Image and
  scratch pages must be conventional RAM before allocation. The BootInfo
  blob is placed on a 64 KiB boundary below 1 GiB, outside image, scratch,
  firmware FDT and FDT-reserved ranges. Entry is at EL2 or EL1 with
  interrupts off and caches cleaned, X0 = BootInfo.
- **Firmware description.** `rpi4` requires a bounded FDT (at most 2 MiB)
  from the UEFI configuration table, lying in memory the UEFI map
  describes (not MMIO). The loader validates its header, block ranges,
  reservation map, structure tokens, BCM2711 compatibility, CPU data,
  eMMC/PCIe DMA windows and any PSCI conduit before copying it into BootInfo.
  Fixed `/reserved-memory` ranges and reservation-map entries are excluded
  from image, scratch and BootInfo placement, and the kernel keeps them out
  of its memory. Dynamic pools (`size`/`alloc-ranges` without `reg`, such
  as the firmware's `linux,cma`) are requests to an operating system, not
  memory in use: they are checked for well-formedness and otherwise ignored
  by both. Missing, malformed or non-BCM2711 FDTs fail with a loader
  diagnostic. QEMU `virt` retains its separate platform contract and does
  not forward an FDT to the kernel.
- **Entry checks.** Before anything else the kernel checks X0 and the
  blob and, on a mismatch, prints the reason on the PL011 and halts: X0 a
  device tree (started by the Pi firmware), not BootInfo, version, other
  architecture, above 1 GiB, kernel range not claimed by the loader,
  blob over the kernel, no device tree, not a BCM2711 device tree. The
  shared `port/bootinfo.c` then validates the whole blob (it halts
  without a message).
- **Configuration.** `plan9.ini` from BootInfo, after values taken from
  the device tree: `*ncpu`, and the BCM2711 DMA windows `*emmc2bus`,
  `*pciwin`, `*pcidmawin` that the stock kernel reads from the firmware
  DTB. The device tree's `/chosen/bootargs` is not used.
- **Initial map.** The kernel's initial `KZERO` window maps physical 0 to
  1 GiB (`efibootmap=0,0x40000000`) and stays: `INITMAP` in `mem.h` is the
  whole window for `9pi4efi`, and `../arm64/mmu.c`'s `kmapram()` maps only
  RAM above it. The BCM2711's peripherals are above 1 GiB. The stock `9pi3`
  and `9pi4` keep the release's map: the window is cut above the kernel's
  512 MiB block (the Pi 3's peripherals at `0x3F000000` are in it) and
  their RAM there is mapped again.
- **Memory** is BootInfo's map. Loader scratch pages are claimed before
  ExitBootServices and appear as `BootMemLoaderData`; the kernel accepts
  only loader-owned descriptors across its complete early region. Runtime,
  reserved, MMIO and VideoCore ranges are left alone. The four largest
  usable banks are used.
- **CPUs and reset.** When the firmware FDT provides `/psci`, CPUs 1–3
  are started one by one with PSCI `CPU_ON` through its declared conduit
  (`smc` with TF-A). Without `/psci`, the kernel runs on one CPU. `exit` turns
  secondary CPUs off with `CPU_OFF` and resets with `SYSTEM_RESET`.
  Loading another kernel through `#c/reboot` is refused under UEFI.

`tools/test/arm64-loader --kernel 9pi4efi` runs the loader's platform
code on the host with the pinned release's and pftf v1.53's BCM2711
device trees (Pi 4 B, 400, CM4), refuses QEMU virt's tree as the wrong
board, and passes the built kernel's header through the loader's whole
preflight. `tools/native-arm64/boottest --rpi4-refusal` runs the real
`BOOTAA64.EFI` under AAVMF with `efiplatform=rpi4`: QEMU's own tree and a
`plan9.ini` without `efiscratch` are refused, and a tree carrying the
BCM2711 data passes validation and the copy into BootInfo.

`tools/rpi4/bootinfotest` loads `9pi4efi` and a BootInfo blob made on
the host into QEMU's `raspi4b` and enters it as the loader would (EL2,
X0): a valid blob reaches the banner and the memory banks from its map;
the invalid cases stop with their messages. It runs no TF-A, EDK2 or
loader and no PSCI.
