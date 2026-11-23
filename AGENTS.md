# Agent instructions

Read the root README.md before making changes. It is the **sole authoritative statement of Plan2001 goals and architecture**. Amend that README when goals or design decisions change; never create a parallel specification.

## Mandatory rules

1. **No work documentation on GitHub, in any branch.** Do not create or commit progress notes, task journals, migration diaries, implementation reports, branch-specific plans, status documents, completion summaries, or Markdown describing what you did. Remove existing work-documentation files from a working branch before proposing it for merge. Use the code and commit diff as evidence of completed work.
2. **Preserve the frozen 9front tree.** The pinned 9front baseline in plan9-wasm32/main is the foundation. Keep the 9front directory structure. Do not pull upstream updates or recreate the obsolete custom Plan2001 source tree, GCC integration, or separate userland.
3. **Preserve working implementations.** kernel/main is authoritative for modern native UEFI boot; never revert to legacy boot. plan9-wasm32/main is authoritative for the working WASM32 system and browser/mobile UI; preserve its behavior. Consult monolith only for missing complementary implementation.
4. **Keep changes narrowly scoped.** Import only explicitly approved additions, placing them within the original 9front structure. Share BootInfo and necessary boot logic without imposing a full UEFI emulator on the browser.
5. **Keep technical information where it belongs.** Source comments, build instructions required to operate the software, and interface definitions are allowed when technically necessary. They must not turn into records of agent activity. Goals belong only in README.md.

## CI: explicit owner control

**Do not make CI policy decisions.** The project owner decides whether CI exists, which checks run, when they run, and how much compute they consume. Reading README.md does **not** authorize creation of a workflow.

1. **One central workflow at most.** If explicitly authorized, keep all permanent automated checks in `.github/workflows/ci.yml`. Do not introduce one workflow per branch, feature, architecture, or bug fix.
2. **Approval required for any CI change.** Do not create, edit, enable, or expand workflow files, schedules, triggers, matrices, required checks, third-party actions, or external CI integrations unless the owner explicitly requests that exact CI change. A failing test is not permission to change CI.
3. **Minimal automatic work.** The default proposed gate is one small, approved smoke test for relevant PR changes. Do not run full builds, lengthy browser suites, multi-architecture matrices, or unrelated tests on every push. Owner-approved heavy tests run manually or under explicitly approved conditions.
4. **No branch-CI accumulation.** Feature branches must not add CI workflows by default. Any exceptionally authorized temporary branch workflow must be removed from the branch before merge, after its feature is accepted. A useful feature-specific test may become part of the central CI **only after explicit owner approval**; otherwise it remains an ordinary manually runnable test.
5. **Trivial edits are not a reason for mass testing.** Avoid full CI for comments, README edits, print-message changes, and other cosmetic changes. Do not claim that path filters can identify print-only changes inside source files. When scope is uncertain, leave existing CI policy unchanged and ask the owner before modifying workflow behavior.
6. **Never write CI reports into the repo.** No test logs, run summaries, screenshots, hash listings, branch status notes, or other generated evidence committed as documentation. Refer to the checks and logs in GitHub Actions when needed.

**No workflow files are to be added in the specification phase.** This section is a rule for subsequent implementation, not an instruction to implement CI now.

The repository must contain **software and its current requirements**, not a history of how agents worked on it.