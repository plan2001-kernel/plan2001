# Agent instructions

Read the root README.md before making changes. It is the **sole authoritative statement of Plan2001 goals and architecture**. Amend that README when goals or design decisions change; never create a parallel specification.

## Mandatory rules

1. **No work documentation on GitHub, in any branch.** Do not create or commit progress notes, task journals, migration diaries, implementation reports, branch-specific plans, status documents, completion summaries, or Markdown describing what you did. Remove existing work-documentation files from a working branch before proposing it for merge. Use the code and commit diff as evidence of completed work.
2. **Keep 9front current through the locked upstream workflow, and the approved target set.** `upstream/9front.lock` is the foundation: change it only with `tools/upstream update` and review, never let a build follow a moving branch, and keep every change to an upstream file a patch in `patches/9front/series` (no full copies of upstream files). Keep the verified upstream cache and isolated bootstrap VM intact and retain the 9front directory structure. New Plan2001 features are frozen; do not selectively undo upstream improvements. Deterministically remove Ghostscript, APE and unsupported CPU targets from the assembled Plan2001 OS source, builds and distribution. Current targets are AMD64, ARM64 (including bcm64/Pi4) and WASM32; RISC-V64 is future work. Do not restore removed components for compatibility. Do not recreate the obsolete custom Plan2001 source tree, GCC integration, or separate userland. Do not submit changes upstream without the owner's authorisation.
3. **Use the current repository as the sole implementation authority.** `plan2001-kernel/plan2001` on `main`, its locked 9front baseline, patches and overlays define the current OS, native UEFI boot, BootInfo and WASM32 runtime and browser/mobile UI. Preserve working implementations; never revert to legacy boot. `plan2001-kernel/room` on `main` owns the separate Room application. The archived `plan2001-kernel/kernel` is only a historical GE/O object-system reference, not an implementation or build dependency. The former `userland`, `plan9-wasm32` and `monolith` repositories are obsolete and may be absent. Never fetch, import, build from or rely on those repositories for current development. Use the current repository and its Git history; report a missing implementation rather than reviving an obsolete source tree.
4. **Keep changes narrowly scoped.** Ghostscript-dependent PDF/PostScript functions and programs that require APE may be removed; retain APE-independent programs from the same directories and other existing functionality. Import only explicitly approved additions, placing them within the original 9front structure. Share BootInfo and necessary boot logic without imposing a full UEFI emulator on the browser.
5. **Keep technical information where it belongs.** Source comments, build instructions required to operate the software, and interface definitions are allowed when technically necessary. They must not turn into records of agent activity. Goals belong only in README.md.

## Mandatory: fix Plan2001 defects immediately; never work around them

- Use Plan2001's own applications and services for their intended tasks. If a Plan2001 component fails during real work, stop the affected task, diagnose the failure, fix its root cause immediately, and verify the fix before resuming the original task.
- Never silently bypass a broken Plan2001 application or substitute another tool to complete the task. In particular, do not substitute Ubuntu/Linux, SSH, SCP, SFTP, a shared partition, a serial console, or an alternate transfer path for broken Drawterm, 9pterm, or other native Plan2001 functionality.
- Diagnostic and development tools may be used to investigate and repair the defect, but not to bypass it and claim the original task is complete. If repair is blocked by missing access, hardware, credentials, or another external dependency, report the precise blocker to the project owner. A workaround or emergency recovery path requires explicit owner approval.
- Verify the repair by repeating the actual failed operation in the real target environment. For remote administration, this means executing the actual remote command or transferring the actual file using the repaired Plan2001 application, not merely passing a mock, local, build, or regression test.
- Prefer the shortest relevant verification that proves the defect is fixed. Do not spend hours on unrelated regression suites, stress tests, or new test infrastructure. Broader tests require a concrete technical reason or explicit owner instruction. Never weaken or falsify tests.
- Once repaired, resume the original task using the repaired application. A successful workaround, a passing test suite, or a successful build is not evidence that the defective functionality works.

This policy applies to every agent, every task, every branch, and all Plan2001 components. Read and follow it at the start of every task.

## CI: enabled

**CI and GitHub Actions are allowed and enabled.** Use focused automated builds and functional tests to reduce agent time and usage spent on repetitive testing.

1. Prefer the existing small, centrally maintained workflow per repository. Create or modify workflows when necessary for the assigned task; avoid temporary or redundant pipelines.
2. Use CI for repeatable checks instead of duplicating them locally. Run only the local or real-target verification needed to establish actual functionality.
3. Do not build unrelated targets, run broad regression campaigns without a concrete reason, weaken tests or bypass repository protections.
4. CI success does not authorize merging or production deployment; obtain owner approval as required.
5. Do not commit test output, screenshots, logs or work reports.

The repository must contain **software and its current requirements**, not a history of how agents worked on it.
