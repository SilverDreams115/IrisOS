# `vfs` contract

## Purpose

Defines the current userland VFS contract: an **endpoint-only, stateless**
read-only namespace service (since Fase 7.2/7.5).

> **Historical note (retired ABI).** Until Fase 7.5 the VFS spoke a stateful
> KChannel protocol (`vfs_proto.h`: `VFS_MSG_OPEN/READ/CLOSE/STATUS/LIST`
> with per-client `file_id` state and process-watch dead-client reclaim).
> That protocol and its header were **removed in Fase 7.5** and are no longer
> ABI. This document used to describe it; see git history if you need the old
> contract. The wire protocol of record is now `iris/vfs_ep_proto.h`,
> documented in `docs/vfs-endpoint.md`.

## Ownership

`vfs` owns:

- the exported boot namespace (static boot files + initrd-backed exports)
- export metadata (name, size, readiness)

`vfs` deliberately owns **no per-client state**: the endpoint protocol is
stateless (full addressing in every request), because `IrisMsg` carries no
sender identity. There are no `file_id`s, no open-file table, and therefore
no dead-client reclaim — nothing to reclaim.

## Bootstrap contract

`vfs` is spawned by `svcmgr` as a catalog service with `own_service_ep = 1`
and `endpoint_only = 1`:

- it receives in `RBX` a private bootstrap channel handle;
- over that channel it receives the **receive side of its KEndpoint**
  (kind `SVCMGR_BOOTSTRAP_KIND_SERVICE_EP` = 0x21, `RIGHT_READ`, sent before
  `INITRD_CAP`), the svcmgr discovery endpoint (kind 0x20) and one
  `KBootstrapCap` (INITRD_CAP kind, `RIGHT_READ`) for initrd VMO access;
- **no legacy service/reply channel pair is created** (`endpoint_only`).

The endpoint is mandatory: without it the service has no request surface and
the smoke gate (`[VFS] ep ready`) fails.

## Request/response surface (current ABI)

Wire format `struct IrisMsg` over `SYS_EP_CALL` / `SYS_REPLY`; opcodes in
`iris/vfs_ep_proto.h`; full semantics in `docs/vfs-endpoint.md`:

- `VFS_EP_OP_LIST` (0x0101) — enumerate exports by visible index
- `VFS_EP_OP_STAT` (0x0102) — size of a named export
- `VFS_EP_OP_READ_AT` (0x0103) — stateless read: path + offset + length
- `VFS_EP_OP_STATUS` (0x0104) — bounded service summary (Fase 7.5)
- `IRIS_EP_OP_PING` (0xFF01) — health check

Replies: `IRIS_EP_REPLY_OK`, or `IRIS_EP_REPLY_ERR` with
`words[0] = (uint32_t)iris_error_t`. Exactly one reply per request; malformed
requests fail cleanly with `IRIS_ERR_INVALID_ARG`.

## Boot export invariants

- 4 static boot files: `iris.txt`, `services.txt`, `readme.txt`, `catalog.txt`.
- At runtime `vfs` seeds 8 additional initrd-backed exports (one per initrd
  image: userboot, init, svcmgr, kbd, vfs, console, fb, sh) — total 12 of a
  capacity of `VFS_SERVICE_EXPORTS` (16).
- Initrd-backed exports are read through eagerly-established VMO mappings
  (`SYS_INITRD_VMO` + `SYS_VMO_MAP` at seed time; `is_mapped`/`virt_base` in
  `struct vfs_export`). There is **no** fault-driven mapping involved.
- Total exported bytes are not a fixed invariant; initrd ELF sizes vary by
  build.

## Clients

- `sh`: `ls` / `cat` via EP_CALL on `"vfs.ep"` (endpoint-only since 7.2).
- `init`: S5/S6 healthy-path probes (LIST / STAT / READ_AT), fail-fast.
- `svcmgr`: diagnostics via `VFS_EP_OP_STATUS` (Fase 7.5).
- `iris_test`: T026–T030 protocol conformance, T031 `.ep` anti-spoof.
