# `vfs` contract

## Purpose

Defines the current userland VFS contract for boot exports, client-visible file state, and dead-client cleanup.

## Ownership

`vfs` owns:

- client-visible `file_id` namespace
- per-client open-file state
- exported boot namespace
- offsets and stale-id rejection
- dead-client reclaim through process-watch events

The kernel no longer owns the healthy-path `OPEN/READ/CLOSE` semantics for normal clients.

## Bootstrap contract

`vfs` is spawned by `svcmgr` as an ELF service and receives in `RBX`:

- a private bootstrap channel handle

From `svcmgr`, `vfs` receives over that bootstrap channel:

- one public request handle for the VFS service endpoint
- one reply handle for VFS replies

Current child rights from the service catalog:

- service handle: `RIGHT_READ | RIGHT_WRITE`
- reply handle: `RIGHT_WRITE`

## Request/response surface

Current request opcodes:

- `VFS_MSG_OPEN`
- `VFS_MSG_READ`
- `VFS_MSG_CLOSE`
- `VFS_MSG_STATUS`
- `VFS_MSG_LIST`

Current reply opcodes:

- `VFS_MSG_OPEN_REPLY`
- `VFS_MSG_READ_REPLY`
- `VFS_MSG_CLOSE_REPLY`
- `VFS_MSG_STATUS_REPLY`
- `VFS_MSG_LIST_REPLY`

## `OPEN` contract

`VFS_MSG_OPEN` requires:

- a NUL-terminated path
- path length within `VFS_MSG_OPEN_PATH_MAX`
- an attached caller `KProcess` handle

The attached caller handle must carry `RIGHT_READ` so `vfs` can arm a process-exit watch.

Current healthy-path client behavior:

- `init` opens `iris.txt`
- `init` duplicates `SYS_PROCESS_SELF` with `RIGHT_READ | RIGHT_TRANSFER`
- `vfs` uses that proc handle to reclaim state if the client dies

## `READ` contract

`VFS_MSG_READ` addresses an existing `file_id` and requested byte length.

Current reply contract:

- reply echoes `file_id`
- carries `err`
- carries actual returned byte count
- payload bytes are bounded by `VFS_MSG_READ_REPLY_DATA_MAX`

## `CLOSE` contract

`VFS_MSG_CLOSE` retires a client-visible `file_id`.

After close:

- that `file_id` must not remain usable by the client
- stale-id access is treated as invalid

## `LIST` contract

`VFS_MSG_LIST` enumerates the boot export namespace by index.

Current healthy-path expectations checked by `init`:

- index `0` succeeds
- index `1` succeeds
- index `2` fails as out-of-bounds

Current boot export invariants:

- `VFS_BOOT_EXPORT_COUNT == 2`
- `VFS_BOOT_EXPORT_TOTAL_BYTES == 31`

## `STATUS` contract

`VFS_MSG_STATUS` returns bounded service-local summary fields:

- protocol version
- ready export count
- live open file count
- open file capacity
- total exported bytes

If a one-shot reply handle is attached, it must grant `RIGHT_WRITE`. If not attached, the service may use the bootstrapped shared reply channel for compatibility.

## Current invariants

- `vfs` is the source of truth for boot-file export metadata.
- `vfs` capacity is bounded by `VFS_SERVICE_OPEN_FILES`.
- `vfs` is responsible for reclaiming dead-client state rather than delegating that ownership back to the kernel.
- Healthy boot currently expects `vfs` status values to match the constants used by `init` and `svcmgr` diagnostics.
