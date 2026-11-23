# Plan2001

**Plan2001 is 9front, ported to AMD64, ARM64, RISC-V64 and WASM32 while preserving the original 9front operating system, source tree and interfaces as far as practical.**

This README is the project's authoritative software specification and list of goals. It describes intended behavior, not a claim that every target is already implemented.

## Foundation and source of truth

- **Base system:** the frozen 9front revision already incorporated in `plan2001-kernel/plan9-wasm32` on `main`. Keep that exact upstream baseline for this migration. Do not update, rebase onto, or merge a newer 9front.
- **Working WASM32 system:** `plan9-wasm32/main` is an existing, functioning 9front port, not a prototype to replace. Preserve its kernel, WebAssembly user programs, process execution, Rio, browser integration, mobile touch behavior, storage, and established functionality. Change its boot path only as required for the common boot architecture.
- **Authoritative native boot:** `plan2001-kernel/kernel/main` defines the modern UEFI boot implementation and shared BootInfo ABI. Its already implemented EFI/ESP boot path, including boot via GRUB and booting an installed system from its own disk, **must be preserved**, not redesigned or reverted to legacy boot.
- **Supplementary source:** `plan2001-kernel/monolith` provides relevant boot and WASM32 implementation details only where needed. Where sources differ, `kernel/main` is authoritative for native boot and `plan9-wasm32/main` for the working browser/WASM32 system.
- **Selected software:** retain explicitly approved improvements and applications from previous work, including p9term, corrected drawterm, browser-based drawterm, WireGuard, and other individually approved components. Integrate them into their natural places in the 9front tree; do not import unrelated experiments.

The old separate `userland` repository, the experimental GCC integration, and the separate custom Plan2001 source-tree architecture are **obsolete** and must not be reintroduced. Preserve the 9front compiler/toolchain model and the working WASM32 port. The host tools used to produce an existing EFI binary do not justify adding a GCC-based Plan2001 userland.

## Architecture

### One 9front source tree

Keep 9front's original directory layout and its native abstractions. Architecture-specific files and approved enhancements belong in their natural 9front locations. Do not reorganize the system into parallel `kernel/`, `userland/` and `web/` operating-system trees.

Native targets are AMD64, ARM64 and RISC-V64. WASM32 is another **native 9front architecture**: its 9front user-space applications are compiled to and execute as WebAssembly. It is **not** a Linux userland, a Linux syscall compatibility layer, a container, or a Linux-like userspace port. The target list must not be confused with an assertion of present feature parity.

### One boot contract, minimal platform-specific loaders

The shared contract between loader and kernel is the existing **BootInfo v1** ABI from `kernel/main`, with a shared C-language boot core where this is technically meaningful.

**Native machines:** keep the modern UEFI-only loader and boot behavior already implemented in `kernel/main`. Keep local EFI/ESP boot of installed systems. Do not restore legacy BIOS or 9front legacy boot paths. Extend the existing loader for network boot rather than replacing it. The proposed UEFI HTTP Boot flow is: firmware obtains a small Plan2001 EFI program; that program loads the kernel and required boot materials over the network and enters the kernel using BootInfo. Network transport support and firmware compatibility must be handled explicitly; do not assume every firmware can perform HTTPS or HTTP Boot reliably.

**Browser/WASM32:** implement a small, portable web bootstrap, **not a full UEFI firmware emulator**. Share the necessary C-language boot logic and BootInfo representation with native boot, but use browser facilities to fetch and instantiate WebAssembly, initialize shared memory, and provide devices. WASM32 boot does not require GPT or ESP. Preserve the currently working `plan9-wasm32` runtime, UI, Web Workers, canvas, and touch interfaces. Keep the browser-specific boundary thin and explicit.

The purpose of this unification is a common kernel hand-off and maintained source code, **not** binary-identical firmware on fundamentally different execution platforms.

### Boot source is chosen explicitly

The user chooses what to start. Opening a URL starts that URL's system; selecting a local server or launching an installed offline PWA starts that environment. Do **not** automatically redirect to a different origin, choose another operating system, or silently fall back to an unrelated boot source when the requested server is unavailable.

## Distribution, installation and offline use

- **Web:** `plan2001.com` hosts the browser-bootable WASM32 system. The same application must be usable through a local HTTP server (including `tools/serve`) and as an explicitly launched offline-capable installed PWA. A bare `file://` HTML launch is not required.
- **Browser storage:** retain the functioning OPFS-based installed system. OPFS is scoped to the page's origin: separate origins do not share an installation implicitly. The offline PWA must explicitly cache the boot resources it needs; the present service worker's empty fetch handler is not an offline cache.
- **Native network boot:** prefer UEFI HTTP Boot when supported and reliable. A small EFI first-stage loader may retrieve the kernel and boot resources from `plan2001.com`. Network boot is a goal, not a reason to remove working local UEFI boot.
- **Native installation:** a live environment can load its installation payload directly from the server, or from an explicitly downloaded package. Once installed, a native system must boot from its own disk without network access, using the already supported UEFI/ESP mechanism.
- **Removable media:** if required, distribute the simplest practical UEFI-bootable raw USB disk image, architecture-specific when simpler. A single multi-architecture image is not mandatory.
- **ISO 9660/CD-ROM:** not a design requirement or preferred distribution format. Do not build new architecture around ISO, CD boot, PXE legacy paths, or historical installer assumptions.

## Preservation and scope

1. Do not replace a working component merely to make the architecture look cleaner.
2. Keep existing 9front behavior except where a change is explicitly specified here.
3. Keep the already completed legacy-driver removal from the appropriate source repository; do not redo it from scratch or indiscriminately delete more drivers.
4. Preserve the mobile Rio touch semantics, text selection, Android keyboard handling, fullscreen behavior, display, and existing browser UI **unchanged** through the boot migration.
5. Migrate only approved programs and improvements; do not automatically import whole obsolete repositories.
6. This document defines architecture and scope. Implementation, CI additions and new test campaigns are separate subsequent work, not prerequisites for writing this specification.

## Continuous integration policy

**CI configuration is centrally controlled; normal agent work is autonomous and must not pause for CI approval.** The intended permanent configuration is at most one small GitHub Actions workflow, `.github/workflows/ci.yml`, when the project owner explicitly assigns CI setup.

- Development agents complete their assigned implementation, add or update relevant test code, and run available local tests without asking the user to make CI decisions. They do not create or change CI workflows as a side effect.
- Only an explicitly assigned CI task may change jobs, triggers, matrices, schedules or required checks. CI-related ideas arising during ordinary work are not reasons to stop work or request approval.
- Keep automatic checks few, small, and relevant. Expensive suites may be run manually; an ordinary source test need not become a CI job. Do not introduce a workflow per feature or branch.
- Temporary branch-specific workflows must not be merged into `main`. An owner-assigned CI change can incorporate selected permanent checks into the single central workflow.
- Do not commit work diaries, CI logs, screenshots or test reports into the repository. If a required GitHub check or infrastructure failure blocks a merge, complete the implementation and state the external blocker in the final response; do not bypass repository protections.

## Repository and agent policy

**This root README is the single authoritative statement of project goals. Update it when goals or architecture decisions change.**

**Do not create or retain documentation of work performed in this repository, including in feature branches.** No agent progress reports, migration diaries, implementation-status Markdown, per-branch plans, completion summaries, or files created merely to describe what a branch changed. Remove such work-documentation files from proposed branches before merging. Source code and its commit diff show what was implemented; the README states what should be implemented.

Keep only documentation necessary for the software itself (such as interfaces, build inputs, or source comments), not accounts of agent activity. See `AGENTS.md` for mandatory contributor instructions.
