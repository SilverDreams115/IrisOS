# Diagnostics contract

## Purpose

Defines the current observability split between kernel-owned diagnostics and service-aggregated diagnostics.

## Two-layer model

IRIS diagnostics are intentionally split:

1. Kernel-owned snapshot:
   - `SYS_DIAG_SNAPSHOT`
   - wire contract in `kernel/include/iris/diag.h`
2. Service-aggregated summary:
   - `SVCMGR_MSG_DIAG`
   - wire contract in `kernel/include/iris/svcmgr_proto.h`

The full healthy-path picture requires both layers.

## Kernel diagnostics contract

`SYS_DIAG_SNAPSHOT` writes one atomic `iris_diag_snapshot` to a caller-supplied user buffer.

Current fields:

- `magic`
- `version`
- `tasks_live`
- `tasks_max`
- `kproc_live`
- `kproc_max`
- `irq_routes_active`
- `irq_routes_max`
- `ticks_lo`
- `ticks_hi`
- `kchan_live`
- `kchan_max`
- `knotif_live`
- `knotif_max`
- `kvmo_live`
- `kvmo_max`

Current access model:

- unrestricted read access
- compact bounded counters only
- no raw internal dumps

Clients must validate:

- `magic == IRIS_DIAG_MAGIC`
- `version == IRIS_DIAG_VERSION`

before trusting other fields.

## `svcmgr` aggregated diagnostics contract

`SVCMGR_MSG_DIAG` requires the caller to attach a writable reply channel.

`svcmgr` then composes:

- kernel snapshot values from `SYS_DIAG_SNAPSHOT`
- local manager values:
  - manifest count
  - ready service count
  - active tracked slots
  - catalog version
- `vfs` status values
- `kbd` status values

The resulting `SVCMGR_MSG_DIAG_REPLY` is the current system-health gate used by `init`.

## Current healthy-path expectations

`init` currently treats the boot as unhealthy unless all of these hold:

- `err == 0`
- `manifest == 2`
- `ready == 2`
- `slots == 2`
- `tasks > 0`
- `kproc > 0`
- `irq > 0`
- `catalog == IRIS_SERVICE_CATALOG_VERSION`
- `vfs_exports == VFS_BOOT_EXPORT_COUNT`
- `vfs_opens == 0`
- `vfs_capacity == VFS_SERVICE_OPEN_FILES`
- `vfs_bytes == VFS_BOOT_EXPORT_TOTAL_BYTES`
- `kbd_flags == KBD_STATUS_NORMAL`

## Current invariants

- `SYS_DIAG_SNAPSHOT` is the source of truth for kernel-owned pool counters.
- `SVCMGR_MSG_DIAG` is the source of truth for the current global healthy-boot gate.
- Service-local status remains authoritative for service-owned state.
- A passing diagnostics response today is necessary but not sufficient for full runtime correctness; it is a bounded health summary.
