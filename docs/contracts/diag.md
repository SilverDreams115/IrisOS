# Diagnostics contract

## Purpose

Defines the current observability split between kernel-owned diagnostics and service-aggregated diagnostics.

## Diagnostics model

IRIS diagnostics are surfaced entirely through the `svcmgr` IPC layer.

`SYS_DIAG_SNAPSHOT` (syscall 30) was retired in Phase 51 and returns
`IRIS_ERR_NOT_SUPPORTED`.  Kernel-side counters are no longer exposed via a
direct user-buffer snapshot path.

The current single-layer model:

- `SVCMGR_MSG_DIAG` — service-aggregated summary over IPC
  - wire contract in `kernel/include/iris/svcmgr_proto.h`

## `svcmgr` aggregated diagnostics contract

`SVCMGR_MSG_DIAG` requires the caller to attach a writable reply channel.

`svcmgr` then composes from internal state and live service queries:

- manager-internal values:
  - manifest count
  - ready service count
  - active tracked slots
  - catalog version
  - live task and process counts (maintained internally)
  - active IRQ route count (maintained internally)
  - scheduler tick snapshot (maintained internally)
- `vfs` status values from `VFS_EP_OP_STATUS` (EP_CALL on `"vfs.ep"`; legacy `VFS_MSG_STATUS` retired in Fase 7.5)
- `kbd` status values from `KBD_MSG_STATUS`

The resulting `SVCMGR_MSG_DIAG_REPLY` is the current system-health gate used by `init`.

## Current healthy-path expectations

`init` currently treats the boot as unhealthy unless all of these hold:

- `err == 0`
- `manifest == 3`
- `ready == 3`
- `slots == 3`
- `tasks > 0`
- `kproc > 0`
- `irq > 0`
- `catalog == IRIS_SERVICE_CATALOG_VERSION`
- `vfs_exports >= VFS_BOOT_EXPORT_COUNT` (at least the static boot files are present)
- `vfs_opens == 0`
- `vfs_capacity == VFS_SERVICE_OPEN_FILES`
- `kbd_flags == KBD_STATUS_NORMAL`

## Current invariants

- `SVCMGR_MSG_DIAG` is the source of truth for the current global healthy-boot gate.
- Kernel-owned pool counters are surfaced only through `svcmgr` internal state, not through a direct kernel snapshot syscall.
- Service-local status remains authoritative for service-owned state.
- A passing diagnostics response today is necessary but not sufficient for full runtime correctness; it is a bounded health summary.
- `vfs_bytes` is no longer a fixed invariant: the total exported bytes include initrd ELF file sizes which vary by build.
