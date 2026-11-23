# Plan2001

**Plan2001 is 9front, ported to AMD64, ARM64, RISC-V64 and WASM32 while preserving the original 9front operating system, source tree and interfaces as far as practical.**

This README is the project's authoritative software specification and list of goals. It describes intended behavior, not a claim that every target is already implemented.

## Foundation and source of truth

- **Base system:** the frozen **9front-11952** release already incorporated in `plan2001-kernel/plan9-wasm32/main`. Its source release is pinned by `tools/release.conf`: `9front-11952.amd64.iso.gz`, SHA256 `59ba79f1304f02c1aae573b337fa2569fc78027ce8e4c2056c4117e971025b91` (compressed), SHA256 `ee90b5e8a8d6b4ee8ad950322f91e338f698dae18c7f5e5800a650bbfa173fd9` (uncompressed ISO). Keep this exact baseline throughout consolidation; do not update, rebase onto or merge a newer 9front.
- **Working WASM32 system:** `plan9-wasm32/main` is an existing, functioning 9front port, not a prototype to replace. Preserve its kernel, WebAssembly user programs, process execution, Rio, browser integration, mobile touch behavior, storage, and established functionality. Change its boot path only as required for the common boot architecture.
- **Authoritative native boot:** `plan2001-kernel/kernel/main` defines the modern UEFI boot implementation and shared BootInfo ABI. Its already implemented EFI/ESP boot path, including boot via GRUB and booting an installed system from its own disk, **must be preserved**, not redesigned or reverted to legacy boot.
- **Supplementary source:** `plan2001-kernel/monolith` provides relevant boot and WASM32 implementation details only where needed. Where sources differ, `kernel/main` is authoritative for native boot and `plan9-wasm32/main` for the working browser/WASM32 system.
- **Selected software:** retain explicitly approved improvements and applications from previous work, including the existing **9pterm** (also referred to as p9term; verify actual source name before integration), corrected drawterm, browser-based drawterm, and WireGuard. Integrate only verified, approved implementations into their natural places in the 9front tree; do not import unrelated experiments. Do not duplicate the same application under competing names.

The old separate `userland` repository, the experimental GCC integration, and the separate custom Plan2001 source-tree architecture are **obsolete** and must not be reintroduced. Preserve the 9front compiler/toolchain model and the working WASM32 port. The host tools used to produce an existing EFI binary do not justify adding a GCC-based Plan2001 userland.

## Architecture

### One 9front source tree

Keep 9front's original directory layout and its native abstractions. Architecture-specific files and approved enhancements belong in their natural 9front locations. Do not reorganize the system into parallel `kernel/`, `userland/` and `web/` operating-system trees.

**Source assembly:** retain the working `plan9-wasm32` model: acquire the SHA256-verified pinned release, apply its existing patches and overlay, and assemble one complete 9front tree for building. The assembled build tree is the source tree used to compile the OS; it need not be a second checked-in copy of every upstream 9front file. The release fetch may require network access on the first build; subsequent builds may use a verified local cache. A failed or mismatched release fetch must fail the build rather than silently substituting another 9front. Do not independently import the entire base release into multiple feature branches.

**Port boundaries:** share the `sys/include/bootinfo.h` ABI and `sys/src/9/port/bootinfo.c` validation/runtime logic. Keep CPU entry, early physical mappings, framebuffer mapping and device access in architecture-specific code. Import native boot functionality selectively from `kernel/main`: `boot/efi/`, the necessary AMD64 early-boot routines, BootInfo integration, boot-argument handling and framebuffer progress markings. Do not bulk-copy the GE/O-modified `kernel/core`, `userland` or unrelated experiments. Preserve the existing accepted AMD64 driver configuration and its prior legacy-driver pruning; do not carry out new indiscriminate driver source deletions.

Native targets are AMD64, ARM64 and RISC-V64. WASM32 is another **native 9front architecture**: its 9front user-space applications are compiled to and execute as WebAssembly. It is **not** a Linux userland, a Linux syscall compatibility layer, a container, or a Linux-like userspace port. The target list must not be confused with an assertion of present feature parity.

### One boot contract, minimal platform-specific loaders

The shared contract between loader and kernel is **BootInfo v1**, retaining the ABI established in `kernel/main` and the existing working WASM32 interpretation. One common header and one common C implementation own BootInfo layout, parsing, validation and version compatibility across targets. Reconcile the distinct `kernel/main` and `plan9-wasm32/main` validators; preserve the stricter compatible checks (section bounds/overlap, architecture, memory types, root image, framebuffer, optional FDT and entropy). No architecture may maintain a divergent private copy of shared validation.

BootInfo v1 is append-only: missing optional trailing fields mean zero/absent, and incompatible layout or semantics require a new ABI version. The legacy GE/O boot-process manifest is **not** part of the 9front boot model. Its optional trailing fields may remain accepted for binary compatibility, but the 9front kernel must neither require nor execute the GE/O process graph. `plan9.ini` remains ordinary 9front configuration data.

**Native machines:** keep the modern UEFI-only loader and boot behavior already implemented in `kernel/main`. Keep local EFI/ESP boot of installed systems, including current GRUB-assisted boot where needed. Do not restore legacy BIOS/9boot as active native boot routes. Integrate and adapt the existing native EFI loader instead of designing another loader. It already handles 9front a.out kernels and a restricted ELF64 path; **retain both initially**, with 9front a.out the primary native kernel format. An ELF implementation useful to the existing loader is not grounds to restore the abandoned GCC-based OS/userland. The standard target build uses 9front's `mk`, compiler/linker and EFI conversion tools; any optional host-GCC EFI build must be explicitly isolated from the OS toolchain and must not displace the working native build.

Existing legacy boot source files may remain as unused frozen-upstream source, but must not be selected by new native build, boot or installer configurations. Change legacy `inst/bootsetup` assumptions only where a new native UEFI installer actually needs it; do not alter the functioning WASM32 installer merely to remove historic terminology.

Extend the loader for network boot rather than replacing it. The proposed UEFI HTTP Boot flow is: firmware obtains a small Plan2001 EFI program; that program loads the kernel and required boot materials over the network and enters the kernel using BootInfo. Firmware compatibility and transports must be handled explicitly; do not assume every firmware can perform HTTPS or HTTP Boot reliably.

**Browser/WASM32:** keep its existing small web bootstrap, **not a full UEFI firmware emulator**. JavaScript may assemble the binary BootInfo v1 blob and use browser facilities to fetch and instantiate WebAssembly, initialize shared memory and provide devices. Its C kernel consumes the same BootInfo layout and shared validation as native targets; the browser need not run the native EFI C program. WASM32 boot does not require GPT or ESP. Preserve the working `plan9-wasm32` runtime, UI, Web Workers, canvas, touch interfaces and installer behavior. Keep the browser-specific boundary thin and explicit.

**Native implementation order:** complete AMD64 on UEFI first. ARM64 starts with QEMU `virt` under UEFI and its BootInfo pointer in `X0`; later hardware platforms (including Raspberry Pi) are distinct follow-up work. RISC-V64 is a new 9front port, not an existing upstream port: its first platform is QEMU `virt` with UEFI and a BootInfo pointer in `a0`, after the AMD64 and ARM64 baselines work. The ABI and intended targets do not imply these ports are already bootable.

The purpose of this unification is a common kernel hand-off and maintained source code, **not** binary-identical firmware on fundamentally different execution platforms.

### Boot source is chosen explicitly

The user chooses what to start. Opening a URL starts that URL's system; selecting a local server or launching an installed offline PWA starts that environment. Do **not** automatically redirect to a different origin, choose another operating system, or silently fall back to an unrelated boot source when the requested server is unavailable.

## Distribution, installation and offline use

- **Web:** `plan2001.com` hosts the browser-bootable WASM32 system. The same application must be usable through a local HTTP server (including `tools/serve`) and as an explicitly launched offline-capable installed PWA. A bare `file://` HTML launch is not required.
- **Browser storage:** retain the functioning OPFS-based installed system. OPFS is scoped to the page's origin: separate origins do not share an installation implicitly. The offline PWA must explicitly cache the boot resources it needs; the present service worker's empty fetch handler is not an offline cache.
- **Native network boot:** prefer UEFI HTTP Boot when supported and reliable. The EFI first stage may retrieve the kernel and boot resources from `plan2001.com`. Prefer HTTPS, but do not treat HTTPS alone as a substitute for authenticating boot artifacts. Before enabling public network boot, require a trusted signature on an authenticated boot manifest that identifies and hashes the exact kernel and boot resources, with verification anchored in a key provisioned independently of the downloaded manifest. Plain HTTP may only carry payloads when this signature policy is enforced. Keep UEFI Secure Boot as a distinct, separately designed feature. On missing, invalid or untrusted boot artifacts, fail with a clear diagnostic, without silently booting a different source. Network boot must not break local UEFI boot.
- **Native installation:** initially, an architecture-specific UEFI-bootable raw disk/USB image is sufficient: ESP, EFI loader, kernel and required 9front system files. A live environment may load its installation payload directly from the server or from an explicitly downloaded package. Once installed, a native system must boot from its own disk without network access, using the existing UEFI/ESP mechanism. A new package manager is out of scope for boot consolidation.
- **Removable media:** if required, distribute the simplest practical UEFI-bootable raw USB disk image, architecture-specific when simpler. A single multi-architecture image is not mandatory.
- **ISO 9660/CD-ROM:** not a design requirement or preferred **new** native distribution format. **Preserve the currently working WASM32 ISO/live-image loading, `?iso=file` path and installer workflow.** Do not build new architecture around ISO, CD boot, PXE legacy paths or historical installer assumptions. The old ISO workflow and the new ISO-independent distribution goal can coexist during migration.

## Preservation and scope

1. Do not replace a working component merely to make the architecture look cleaner.
2. Keep existing 9front behavior except where a change is explicitly specified here.
3. Keep the already completed legacy-driver removal from the appropriate source repository; do not redo it from scratch or indiscriminately delete more drivers.
4. Preserve the mobile Rio touch semantics, text selection, Android keyboard handling, fullscreen behavior, display, and existing browser UI **unchanged** through the boot migration.
5. Migrate only approved programs and improvements; do not automatically import whole obsolete repositories.
6. Preserve 9front's existing network protocols and user-space behavior, including older authentication or remote-access mechanisms, unless a distinct approved change removes them; protocol retirement is not part of the boot migration.
7. Preserve working native EFI boot diagnostics: the loader's framebuffer stage marks before and after ExitBootServices and before kernel transfer, with kernel-side marks continuing the same row. These marks and `[P2 Lxx]` output are troubleshooting signals, not substitutes for successful boot.
8. This document defines architecture and scope. Implementation, CI additions and new test campaigns are separate subsequent work, not prerequisites for writing this specification.

### Integration acceptance

- The initial native AMD64 path is accepted only when UEFI firmware -> Plan2001 EFI loader -> valid BootInfo v1 -> **9front AMD64 kernel -> 9front init -> usable 9front userland** works, including an installed offline UEFI/ESP boot. Loader messages or coloured framebuffer marks alone do not meet acceptance.
- The existing WASM32 build and browser boot must remain functional, including its existing UI, process execution, Rio input/touch semantics, OPFS and ISO/live workflow.
- Run targeted builds and smoke/integration tests **locally**, including QEMU with `-accel kvm:tcg` where supported, plus appropriate browser tests. Do not commit diagnostic artifacts. CI stays disabled.
- ARM64 and RISC-V64 are separate later milestones, with explicit build/boot acceptance on their chosen QEMU platforms. Neither is a blocker for AMD64/WASM32 consolidation.

## Continuous integration policy

**CI is disabled until the project owner explicitly decides to enable it.** There are no GitHub Actions workflows in the repository, and none are to be created, enabled or imported during normal development or migration.

- Agents must complete assigned software work without requesting CI decisions or pausing for CI approval. Integration tasks may be developed concurrently on isolated branches, with shared ABI ownership assigned to exactly one branch; a single integration owner merges and tests the combined result after the individual branches are ready.
- Agents may write, update and run appropriate **local tests**, but must not create GitHub Actions workflows, automatic test triggers, required CI checks, matrices or external CI pipelines.
- No feature-branch or temporary CI workflows are permitted while CI is disabled. Do not import CI configuration from the source repositories.
- If the owner later enables CI, prefer one centrally maintained minimal workflow; its scope and triggers must be defined by the owner separately.
- Do not commit logs, reports, screenshots, hashes or other work documentation to the repository.

## Repository and agent policy

**This root README is the single authoritative statement of project goals. Update it when goals or architecture decisions change.**

**Do not create or retain documentation of work performed in this repository, including in feature branches.** No agent progress reports, migration diaries, implementation-status Markdown, per-branch plans, completion summaries, or files created merely to describe what a branch changed. Remove such work-documentation files from proposed branches before merging. Source code and its commit diff show what was implemented; the README states what should be implemented.

Keep only documentation necessary for the software itself (such as interfaces, build inputs, or source comments), not accounts of agent activity. See `AGENTS.md` for mandatory contributor instructions.
