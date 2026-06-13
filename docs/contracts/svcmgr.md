# `svcmgr` contract

## Purpose

Defines the current service-manager contract for discovery, runtime publication, bootstrap delegation, supervision, and global status aggregation.

## Responsibilities

`svcmgr` currently owns:

- autostart of built-in userland services from the service catalog
- runtime service discovery for normal clients
- first-cut dynamic runtime publication of extra service endpoints
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

Discovery now supports both:

- fixed endpoint lookup (`SVCMGR_MSG_LOOKUP`)
- fixed/dynamic name lookup (`SVCMGR_MSG_LOOKUP_NAME`)

Current endpoints:

- `SVCMGR_ENDPOINT_KBD`
- `SVCMGR_ENDPOINT_KBD_REPLY`
- `SVCMGR_ENDPOINT_VFS`
- `SVCMGR_ENDPOINT_VFS_REPLY`
- `SVCMGR_ENDPOINT_SH`
- `SVCMGR_ENDPOINT_SH_REPLY`

Built-in endpoints resolve through the service catalog to one service master handle and one allowed-rights mask.
Runtime-published endpoints are stored in `svcmgr` state as `(endpoint, name, public_h, client_rights)` entries.

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
- `sh`
  - service endpoint: `RIGHT_WRITE`
  - reply endpoint: `RIGHT_READ`

## Dynamic publication contract

`SVCMGR_MSG_REGISTER` / `SVCMGR_MSG_UNREGISTER` are the first cut of a general runtime registry.

Publisher request:

- message type: `SVCMGR_MSG_REGISTER`
- payload:
  - runtime endpoint id
  - allowed client-rights mask
  - fixed-size service name
- attached handle:
  - one service master handle moved into `svcmgr`
  - attached rights must include `RIGHT_DUPLICATE`

Registration behavior:

- collisions with built-in endpoint ids are rejected
- collisions with built-in names (`kbd`, `vfs`) are rejected
- collisions with existing runtime endpoint ids or names are rejected
- only `KChannel` publications are accepted in this cut

Current first-cut semantics:

- publication is possession-based: if the caller can move a duplicable handle into `svcmgr`, it can publish it
- lookup by endpoint and lookup by name both work for dynamic entries
- unregister is proof-of-possession based: the caller must move a handle for the same published channel object
- successful unregister seals the published channel before dropping the registry entry
- sealing invalidates already-distributed channel duplicates with `IRIS_ERR_CLOSED`
- repeated unregister of an already-removed entry is a no-op
- automatic cleanup on publisher death is not implemented in this cut

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

1. query `vfs` with `VFS_EP_OP_STATUS` over `"vfs.ep"` (EP_CALL; the legacy `VFS_MSG_STATUS` was retired with `vfs_proto.h` in Fase 7.5)
2. query `kbd` with `KBD_MSG_STATUS`
3. combine those results with its own internal counters (task count, process count, IRQ routes, tick snapshot)

`SYS_DIAG_SNAPSHOT` is not called; it was retired in Phase 51 and returns `IRIS_ERR_NOT_SUPPORTED`.
The reply is a compact aggregate view. It does not replace service-local status as source of truth.

## Supervision and restart contract

For each tracked service slot, `svcmgr` stores:

- `proc_h`
- `irq_num`
- `service_id`
- short service name

On the death KNotification signal (Fase 13 / Track B — the kernel signals
bit `1<<service_id` on `svcmgr`'s death notification; the bit index names
the exiting slot directly):

- current master service handles for that service are sealed and closed
- the tracked `proc_h` is released
- if the service is autostarted and restart budget remains, `svcmgr` respawns it

Restart policy is declarative:

- controlled by `autostart`
- controlled by `restart_on_exit`
- bounded by `restart_limit`

Current built-in services:

- `kbd`: restart up to 3 times
- `vfs`: restart up to 3 times
- `sh`: autostarted, no restart budget in the current catalog

## Current invariants

- `svcmgr` is the healthy-path discovery authority for `kbd`, `vfs`, and `sh`.
- `svcmgr` supervises service exit by watch events, not by polling.
- Stale master endpoints are sealed before replacement so blocked clients fail fast.
- `svcmgr` can aggregate health only if both kernel diagnostics and service-local status paths are functioning.

## Fase 10 — lifecycle & badge policy

- EP opcodes added: `IRIS_SVCMGR_EP_STATUS` (0xF005, open: name → {alive,
  generation}), `IRIS_SVCMGR_EP_RESTART` (0xF006, **supervisor-only**: kill +
  watch-driven respawn, bumps generation), badge-authenticated
  `IRIS_SVCMGR_EP_REGISTER`/`UNREGISTER` (name claim + `owner_badge`).
- `.ep` lookups grant `RIGHT_WRITE` only to ordinary clients; `DUPLICATE`/
  `TRANSFER` requires `iris_badge_is_supervisor()` (init/svcmgr/unbadged).
- Reserved names (`*.ep`, catalog names) are never runtime-registrable on
  either transport. The legacy KChannel loop is a compatibility boundary
  (`owner_badge = 0`). See [service-lifecycle.md](../service-lifecycle.md).

## Fase 11 — endpoint cap-transfer & REGISTER over EP

- `IrisMsg.attached_cap` (offset 72, ABI 80 B) carries a capability transferred
  by an `EP_CALL` (the reply cap keeps `attached_handle`). Kernel-staged,
  anti-spoof, KReply-compatible.
- `IRIS_SVCMGR_EP_REGISTER` now consumes a real transferred **endpoint** cap
  (validated `KOBJ_ENDPOINT`); LOOKUP returns a same-object cap. Reject paths:
  reserved name → ACCESS_DENIED, no cap / wrong type → INVALID_ARG, name taken
  → BUSY (transferred cap closed on every reject, no leak).
- `UNREGISTER` over EP requires the owner badge (or a supervisor); the stored
  cap is closed on unregister. Legacy KChannel REGISTER/UNREGISTER is a
  compatibility boundary (`owner_badge = 0`).

## Fase 12 — endpoint-first svcmgr

- `IRIS_SVCMGR_EP_DIAG` (0xF007) is the productive snapshot path (replaces
  legacy `SVCMGR_MSG_DIAG`): words[0]=catalog count, [1]=ready, [2]=active
  dynamic, [3]=catalog version. No KChannel.
- Unknown/malformed EP opcodes fail with `INVALID_ARG` — never a silent
  fallback to the legacy loop (T068).
- Legacy KChannel REGISTER/UNREGISTER/LOOKUP/DIAG are a compatibility/test
  boundary (init self-tests + T046); `SVCMGR_MSG_STATUS` retired.
