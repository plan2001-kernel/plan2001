# Agent instructions

Read the root README.md before making changes. It is the **sole authoritative statement of Plan2001 goals and architecture**. Amend that README when goals or design decisions change; never create a parallel specification.

## Mandatory rules

1. **No work documentation on GitHub, in any branch.** Do not create or commit progress notes, task journals, migration diaries, implementation reports, branch-specific plans, status documents, completion summaries, or Markdown describing what you did. Remove existing work-documentation files from a working branch before proposing it for merge. Use the code and commit diff as evidence of completed work.
2. **Preserve the frozen 9front tree.** The pinned 9front baseline in plan9-wasm32/main is the foundation. Keep the 9front directory structure. Do not pull upstream updates or recreate the obsolete custom Plan2001 source tree, GCC integration, or separate userland.
3. **Preserve working implementations.** kernel/main is authoritative for modern native UEFI boot; never revert to legacy boot. plan9-wasm32/main is authoritative for the working WASM32 system and browser/mobile UI; preserve its behavior. Consult monolith only for missing complementary implementation.
4. **Keep changes narrowly scoped.** Import only explicitly approved additions, placing them within the original 9front structure. Share BootInfo and necessary boot logic without imposing a full UEFI emulator on the browser.
5. **Keep technical information where it belongs.** Source comments, build instructions required to operate the software, and interface definitions are allowed when technically necessary. They must not turn into records of agent activity. Goals belong only in README.md.

## CI: fixed policy, autonomous task completion

**Finish the assigned software task without requesting CI decisions from the user.** CI configuration is outside the scope of ordinary development tasks. Do not interrupt, stop, or defer coding, testing, or delivery because a CI workflow could be added or changed.

1. **Never alter CI as a side effect.** Unless the user's actual task explicitly requests CI administration, do not create, edit, enable, delete or otherwise change workflows, jobs, triggers, matrices, required checks, schedules, or external CI services. Do not ask permission during an unrelated task; simply leave CI unchanged.
2. **Tests are part of implementation.** Create or modify relevant source-level tests where needed, and run them locally with available tools. Run existing CI checks when available, but do not turn every test into a GitHub Actions workflow. Fix code defects exposed by tests without altering CI.
3. **At most one permanent workflow.** When the user explicitly tasks an agent with CI changes, prefer one small central workflow at `.github/workflows/ci.yml`. Do not accumulate feature- or branch-specific CI. Remove any temporary CI workflow from the branch before merging it; tests worth retaining should remain ordinary test scripts unless the user has explicitly requested central CI coverage.
4. **Keep CI cheap.** Do not run entire multi-architecture, browser, or long-running suites on every small change. Cosmetic changes (including print strings) must not motivate new automatic checks. Path filters alone cannot identify cosmetic edits within source files.
5. **Never weaken checks or bypass branch protection to force a merge.** Resolve code failures. If an external infrastructure failure or repository rule prevents the final GitHub operation, finish all work that can be completed and report that specific blocker in the response, not a repository document.
6. **No work documentation or CI artifacts in Git.** Do not commit test output, screenshots, logbooks, workflow run reports, hashes, or branch progress notes.

**CI is not a decision point in normal agent work. Complete the requested implementation without user intervention and without changing CI.**

The repository must contain **software and its current requirements**, not a history of how agents worked on it.