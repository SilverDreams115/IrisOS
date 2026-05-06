# IRIS technical contracts

This directory defines the current executable contracts for the healthy boot path and its core bootstrap services.

These documents describe what the code does today, not an aspirational future design.

## Contracts

- [boot.md](./boot.md): kernel boot contract from UEFI handoff to scheduler start
- [bootstrap.md](./bootstrap.md): ring-3 bootstrap handle and service-spawn contract
- [svcmgr.md](./svcmgr.md): service manager discovery, supervision, and aggregation contract
- [vfs.md](./vfs.md): VFS protocol and ownership contract
- [kbd.md](./kbd.md): keyboard service protocol and IRQ-facing contract
- [diag.md](./diag.md): kernel and aggregated diagnostics contract
- [hardening.md](./hardening.md): rights, transfer, IRQ ownership, and critical capability invariants

## Scope rules

- If code and docs disagree, code wins until the docs are corrected.
- Protocol layouts are sourced from headers under `kernel/include/iris/`.
- Bootstrap ownership and sequencing are sourced from `kernel/kernel_main.c`, `kernel/core/initrd/initrd.c`, `kernel/core/syscall/syscall.c`, and the service entrypoints.
- These contracts intentionally describe the current internal compatibility surface. They are not a promise of external ABI stability yet.
