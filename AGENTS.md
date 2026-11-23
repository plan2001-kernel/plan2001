# Agent instructions

Read the root README.md before making changes. It is the **sole authoritative statement of Plan2001 goals and architecture**. Amend that README when goals or design decisions change; never create a parallel specification.

## Mandatory rules

1. **No work documentation on GitHub, in any branch.** Do not create or commit progress notes, task journals, migration diaries, implementation reports, branch-specific plans, status documents, completion summaries, or Markdown describing what you did. Remove existing work-documentation files from a working branch before proposing it for merge. Use the code and commit diff as evidence of completed work.
2. **Preserve the frozen 9front tree.** The pinned 9front baseline in plan9-wasm32/main is the foundation. Keep the 9front directory structure. Do not pull upstream updates or recreate the obsolete custom Plan2001 source tree, GCC integration, or separate userland.
3. **Preserve working implementations.** kernel/main is authoritative for modern native UEFI boot; never revert to legacy boot. plan9-wasm32/main is authoritative for the working WASM32 system and browser/mobile UI; preserve its behavior. Consult monolith only for missing complementary implementation.
4. **Keep changes narrowly scoped.** Import only explicitly approved additions, placing them within the original 9front structure. Share BootInfo and necessary boot logic without imposing a full UEFI emulator on the browser.
5. **Keep technical information where it belongs.** Source comments, build instructions required to operate the software, and interface definitions are allowed when technically necessary. They must not turn into records of agent activity. Goals belong only in README.md.

The repository must contain **software and its current requirements**, not a history of how agents worked on it.