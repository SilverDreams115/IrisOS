# `svcmgr` contract

## Purpose

Defines the current service-manager contract for discovery, bootstrap delegation, supervision, and global status aggregation.

## Responsibilities

`svcmgr` currently owns:

- autostart of built-in userland services from the service catalog
- runtime service discovery for normal clients
- service endpoint rights reduction for lookup replies
- service lifecycle supervision through `SYS_PROCESS_WATCH`
- bounded restart policy for autostart services
- global aggregated diagnostics over kernel and service-local status surfaces

`svcmgr` does not own:

- bootloader handoff
- ELF loading implementation
- low-level IRQ delivery implementation
- kernel object creation policy

## Bootstrap prerequisites

`svcmgr` must receive at minimum:

- one private bootstrap channel handle in `RBX`
- one spawn capability message (`SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP`)

Optional bootstrap inputs:

- one `KIrqCap` per declared IRQ-routed service
- one `KIoPort` capability per declared hardware-I/O service

Without the spawn capability, `svcmgr` exits immediately after logging a fatal bootstrap error.

## Runtime endpoint model

Discovery is endpoint-based, not name-string-based at runtime.

Current endpoints:

- `SVCMGR_ENDPOINT_KBD`
- `SVCMGR_ENDPOINT_KBD_REPLY`
- `SVCMGR_ENDPOINT_VFS`
- `SVCMGR_ENDPOINT_VFS_REPLY`

Each endpoint resolves through the service catalog to one service master handle and one allowed-rights mask.

## Lookup contract

Client request:

- message type: `SVCMGR_MSG_LOOKUP`
- payload:
  - endpoint id
  - requested rights mask
- attached handle:
  - reply channel moved into `svcmgr`
  - rights must include `RIGHT_WRITE`

Lookup behavior:

- unknown endpoint -> `IRIS_ERR_NOT_FOUND`
- known endpoint but unavailable master handle -> `IRIS_ERR_INVALID_ARG`
- granted rights = `requested & allowed`, except `RIGHT_SAME_RIGHTS` maps to full allowed set
- `svcmgr` duplicates the selected master handle with `granted | RIGHT_TRANSFER`
- the duplicate is returned via `SVCMGR_MSG_LOOKUP_REPLY`

Client-facing allowed rights currently come from the service catalog:

- `kbd`
  - service endpoint: `RIGHT_WRITE`
  - reply endpoint: `RIGHT_READ`
- `vfs`
  - service endpoint: `RIGHT_WRITE | RIGHT_DUPLICATE`
  - reply endpoint: `RIGHT_READ | RIGHT_DUPLICATE`

## Status contract

`SVCMGR_MSG_STATUS` returns manager-local health, not full system health.

Current reply fields:

- protocol version
- manifest entry count
- ready service count
- active supervision slot count
- service catalog version

`ready_services` currently means the service has both a public master handle and a reply master handle installed in `svcmgr` state.

## Diagnostics contract

`SVCMGR_MSG_DIAG` is the current global health entry point.

To satisfy it, `svcmgr` must:

1. call `SYS_DIAG_SNAPSHOT`
2. query `vfs` with `VFS_MSG_STATUS`
3. query `kbd` with `KBD_MSG_STATUS`
4. combine those results with its own live state

The reply is a compact aggregate view. It does not replace service-local status as source of truth.

## Supervision and restart contract

For each tracked service slot, `svcmgr` stores:

- `proc_h`
- `irq_num`
- `service_id`
- short service name

On `PROC_EVENT_MSG_EXIT`:

- the exiting slot is matched by watched `proc_h`
- current master service handles for that service are sealed and closed
- the tracked `proc_h` is released
- if the service is autostarted and restart budget remains, `svcmgr` respawns it

Restart policy is declarative:

- controlled by `autostart`
- controlled by `restart_on_exit`
- bounded by `restart_limit`

Current built-in services both restart up to 3 times.

## Current invariants

- `svcmgr` is the healthy-path discovery authority for `kbd` and `vfs`.
- `svcmgr` supervises service exit by watch events, not by polling.
- Stale master endpoints are sealed before replacement so blocked clients fail fast.
- `svcmgr` can aggregate health only if both kernel diagnostics and service-local status paths are functioning.
