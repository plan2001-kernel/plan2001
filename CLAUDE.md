# Plan2001 agent instructions

@AGENTS.md

Read and follow the root AGENTS.md at the start of every task. It is the shared agent policy; do not maintain a separate copy here.

## Do not remove tools

Never uninstall, delete, move or overwrite development tools on this host: system packages, toolchains, plan9port (wherever it is built, including a worktree's `build/host/`), QEMU images, `~/.cache/plan2001`, or other agents' worktrees and build directories. Clean only files you created yourself in the current task, and never with `rm -rf build` or `rm -rf build/host` where a toolchain may live. If a required tool is missing or broken, report it to the owner and ask before installing, replacing or working around it.

## cwfs crashes on Oracle Cloud (#56): no further investigation

The owner has closed the investigation of plan2001.com's cwfs crashes (#56) as an Oracle Cloud platform problem. Do not continue hunting that bug: no new diagnostics, load tests or kernel/cwfs changes for it. If plan2001.com hangs, recover it (OCI reset) and report the crash to the owner; if the crashes continue, the server moves away from Oracle Cloud. Real cwfs or kernel defects found in other work are still fixed as usual.
