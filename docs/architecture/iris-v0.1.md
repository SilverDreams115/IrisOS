# IRIS architecture notes (historical)

This file is no longer an accurate description of the current system architecture.

IRIS has moved beyond the original `v0.1` snapshot. The live architecture is now defined by:

- the repository root [README.md](../../README.md)
- the executable contracts under [docs/contracts](../contracts/README.md)

Current architectural baseline:

- target: `x86_64`
- boot: `UEFI`
- kernel: ring 0
- userland services: ring 3 ELF processes loaded from embedded initrd
- healthy-path bootstrap:
  - kernel boots `svcmgr`
  - `svcmgr` boots `kbd` and `vfs`
  - kernel boots `init` with a narrow `svcmgr` bootstrap handle
- diagnostics:
  - kernel snapshot via `SYS_DIAG_SNAPSHOT`
  - aggregated health via `svcmgr`

This file should be kept only as a historical pointer unless it is rewritten to reflect the current code.
