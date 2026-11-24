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
| (c) Plan2001 BootInfo v1 hand-off | EDK2 → Plan2001 EFI loader → kernel, X0 = BootInfo | not yet possible, see below |
| (d) 9front kernel → init → rc | after (a), or after (c) once it exists | hardware; `tools/rpi4/boottest` (emulated, partial) |

Stage (a) uses the Raspberry Pi firmware's `kernel8`-style boot. It is
the upstream baseline for diagnosing the hardware and the port; it is not
Plan2001's UEFI boot, which remains the native boot policy (README).

## Building

    tools/rpi4/build            # kernels, arm64 userland, SD image
    tools/rpi4/build --no-image # kernels and userland only
    tools/rpi4/boottest         # QEMU raspi4b/raspi3b serial smoke test
    tools/rpi4/stock            # 9front-11952.pi3.img, SHA256-checked

`tools/rpi4/build` runs in the 9front-11952 build VM of
`tools/native-amd64` (`PLAN2001_BUILDVM`, default
`~/.cache/plan2001/base.qcow2`) with no network, from the pinned ISO
(SHA256-checked) with this tree's files over it (`tools/tree` must
succeed first). In the VM it runs `objtype=arm64 mk install` in
`/sys/src`, builds and installs `9pi4` and `9pi3`, and makes the SD
image with 9front's own `/sys/lib/dist/mkfile` rule `%.pi3.img`. The Pi
firmware files and `bcm2711-rpi-4-b.dtb` are the release's
`/sys/src/boot/bcm` files; nothing is fetched.

Output (default `~/.cache/plan2001/rpi4`, or `--out`/`PLAN2001_RPI4_OUT`):

- `9pi4`, `s9pi4` (with symbols), `9pi3`
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

### Why `9pi4` does not run under the Plan2001 EFI loader yet

These are properties of the frozen sources, to be resolved before stage
(c):

1. **Fixed low physical layout.** `bcm64/mem.h` maps `KZERO`
   (`0xffffffffc0000000`) to physical 0 and uses physical `0xd8` (CPU spin
   table), `0x100` (boot arguments), `0x1c00` (reboot code), `0x3400`
   (VideoCore mailbox buffer), and the kernel image, page tables and
   `Mach` structures from `0x80000` down. Under EDK2, `RPI_EFI.fd` (TF-A
   BL31 and the firmware volume) and the DTB occupy physical
   `0x0`–`0x400000`, and TF-A stays resident at EL3. The kernel's low
   memory and image would overwrite it.
2. **Secondary CPUs.** `bcm64/main.c` starts CPUs 1–3 through the spin
   table at `0xd8` that the VideoCore armstub provides. Under TF-A they are
   held by PSCI and need `CPU_ON` (SMC); `bcm64` has no PSCI path.
3. **Entry argument.** `bcm64/main.c` passes X0 to `bootargsinit()` as a
   DTB pointer (`bcm/bootargs.c`). BootInfo v1 puts the BootInfo address
   in X0, with the DTB as BootInfo's optional FDT section.
4. **The shared loader's ARM64 platform.** `sys/src/boot/efi/archaa64.c`
   assumes QEMU `virt`: RAM at `0x40000000`, an ELF kernel linked at
   `0xffffffff80000000 + pa`, and a BootInfo window below 2 GB. The Pi 4's
   RAM starts at 0 and `9pi4` is a raw (`-H6`) image linked at
   `0xffffffffc0080000`.

Items 1–3 are `bcm64` changes (a relocatable kernel base, PSCI CPU start,
a BootInfo entry that takes memory, command line and FDT from BootInfo);
item 4 is a change to the shared loader and belongs to the ARM64/QEMU
`virt` integration, not to a Pi 4 copy of it.
