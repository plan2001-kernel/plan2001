# Agent instructions

Read the root README.md before making changes. It is the **sole authoritative statement of Plan2001 goals and architecture**. Amend that README when goals or design decisions change; never create a parallel specification.

## Mandatory rules

1. **No work documentation on GitHub, in any branch.** Do not create or commit progress notes, task journals, migration diaries, implementation reports, branch-specific plans, status documents, completion summaries, or Markdown describing what you did. Remove existing work-documentation files from a working branch before proposing it for merge. Use the code and commit diff as evidence of completed work.
2. **Preserve the frozen 9front tree.** The pinned 9front baseline in plan9-wasm32/main is the foundation. Keep the 9front directory structure. Do not pull upstream updates or recreate the obsolete custom Plan2001 source tree, GCC integration, or separate userland.
3. **Preserve working implementations.** kernel/main is authoritative for modern native UEFI boot; never revert to legacy boot. plan9-wasm32/main is authoritative for the working WASM32 system and browser/mobile UI; preserve its behavior. Consult monolith only for missing complementary implementation.
4. **Keep changes narrowly scoped.** Import only explicitly approved additions, placing them within the original 9front structure. Share BootInfo and necessary boot logic without imposing a full UEFI emulator on the browser.
5. **Keep technical information where it belongs.** Source comments, build instructions required to operate the software, and interface definitions are allowed when technically necessary. They must not turn into records of agent activity. Goals belong only in README.md.

## CI: disabled

**CI IS DISABLED. Do not create, enable, modify, import or run GitHub Actions workflows or any other automated CI pipelines.** This prohibition applies to `main`, feature branches, and migrations from other repositories. Do not copy existing source-repository workflow directories.

1. **Complete the assigned task autonomously.** Never stop work to request CI approval; simply leave CI disabled and finish the coding task.
2. **Test locally.** Create and run relevant tests as ordinary source files or scripts when needed. Do not convert them to automatic CI jobs.
3. **Do not create temporary or branch-specific workflows.** No `.github/workflows/` files are permitted while CI remains disabled.
4. **Only an explicit project-owner decision to enable CI may change this policy.** If CI is enabled later, the default design is one small centrally controlled workflow, not feature-by-feature CI.
5. **Do not bypass repository protections or weaken tests to finish a task.** Report an external blocker in the final response if it cannot be resolved without changing policy.
6. **No CI artifacts or work documentation in Git.** Do not commit test output, screenshots, hashes, logs or task reports.

**CI is not a dependency for completing software tasks.**

The repository must contain **software and its current requirements**, not a history of how agents worked on it.