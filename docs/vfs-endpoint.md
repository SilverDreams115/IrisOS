# VFS Endpoint Protocol (Fase 7.1; endpoint-only since Fase 7.5)

The VFS service serves file requests **exclusively** over KEndpoint
(`SYS_EP_CALL` + `SYS_REPLY`). The wire format is defined in
`kernel/include/iris/vfs_ep_proto.h`; the dispatcher lives in
`services/vfs/vfs_ep.c` and is unit-tested on the host
(`tests/kernel/test_vfs_ep.c`). The legacy stateful KChannel protocol
(`iris/vfs_proto.h`) was removed in Fase 7.5 together with its header.

## Design: stateless by construction

`struct IrisMsg` carries **no kernel-stamped sender identity** (no badge, no
sender_id — unlike `KChanMsg`). A stateful protocol (open/read/close with a
server-side file table) would have no safe way to bind file descriptors to
clients: any caller could read or close another client's descriptor, and a dead
client would leak table entries with no death notification to reclaim them.

The EP protocol therefore carries full addressing in every request:

- `READ_AT(path, offset, len)` instead of `OPEN` + `READ(fd)` + `CLOSE(fd)`.
- The server keeps **zero** per-client state on the EP path.
- A client crashing mid-sequence leaves nothing behind; its in-flight EP_CALL
  is woken with `IRIS_ERR_CLOSED` by KReply teardown.

The stateful open/read/close protocol was removed in Fase 7.5: `vfs_proto.h`
is deleted, the catalog marks vfs `endpoint_only = 1` (svcmgr creates no
legacy service/reply pair, bootstrap kinds 10/11 are retired) and iris_test
T032 asserts the bare `"vfs"` name no longer resolves. Requests and replies
never transfer capabilities (the kernel forbids request-side cap transfer on
`EP_CALL`; the VFS replies carry only inline data).

## Endpoint ownership and discovery

The VFS does **not** create its own endpoint. svcmgr creates one KEndpoint per
catalog service with `own_service_ep = 1` (today: vfs), keeps the master handle
across restarts, and:

- pre-start-mints the **receive side** (`RIGHT_READ`) into the service's
  root CNode at `IRIS_CPTR_OWN_EP` (slot 5; bootstrap kind 0x21 retired in
  Fase 8);
- publishes the **send side** (`RIGHT_WRITE`) under the reserved name
  `"vfs.ep"`, resolvable through both `IRIS_SVCMGR_EP_LOOKUP_NAME` and the
  legacy `SVCMGR_MSG_LOOKUP_NAME`.

Because svcmgr owns the master, client caps stay valid when the VFS is
respawned; callers blocked on a dying VFS wake with `IRIS_ERR_CLOSED`. Dynamic
registration of any name ending in `".ep"` is rejected with
`IRIS_ERR_INVALID_ARG` (endpoint spoofing prevention). This is runtime-tested
since Fase 7.2: init S4 attempts to register `"spoof.ep"` and verifies the
name stays unresolvable; iris_test T031 verifies the EP lookup of a `".ep"`
name that matches no published endpoint returns `NOT_FOUND` with no cap.

## Operations

Reply convention (see `iris/endpoint_proto.h`): `IRIS_EP_REPLY_OK` with
`words[0] = 0`, or `IRIS_EP_REPLY_ERR` with `words[0] = (uint32_t)iris_error_t`.

### VFS_EP_OP_LIST (0x0101)

Enumerate ready exports by visible index.

| Direction | Field | Meaning |
|-----------|-------|---------|
| Request | `words[0]` | index (`word_count >= 1`) |
| Reply OK | `words[1]` | export size in bytes |
| Reply OK | `words[2]` | name length (excluding NUL) |
| Reply OK | kbuf | NUL-terminated export name (`buf_len = name_len + 1`) |
| Reply ERR | `IRIS_ERR_NOT_FOUND` | index past the last ready export — the normal end-of-listing condition |

### VFS_EP_OP_STAT (0x0102)

| Direction | Field | Meaning |
|-----------|-------|---------|
| Request | kbuf | NUL-terminated path, `1 <= buf_len <= VFS_EP_PATH_MAX` (includes NUL) |
| Reply OK | `words[1]` | export size in bytes |
| Reply ERR | `NOT_FOUND` / `INVALID_ARG` | unknown name / malformed path |

### VFS_EP_OP_READ_AT (0x0103)

Stateless positional read.

| Direction | Field | Meaning |
|-----------|-------|---------|
| Request | kbuf | NUL-terminated path (as STAT) |
| Request | `words[0]` | byte offset |
| Request | `words[1]` | requested length (server clamps to `VFS_EP_DATA_MAX` = 256) |
| Reply OK | `words[1]` | bytes read; **0 = EOF** (`offset >= size` is EOF, not an error) |
| Reply OK | `words[2]` | total export size |
| Reply OK | kbuf | data (`buf_len` = bytes read) |

`EP_CALL` buffer reuse: `msg.buf_uptr` is both the request payload (path) and
the reply bulk destination (data). Clients must re-stage the path before every
call (see `sh_vfs_ep_call` in `services/sh/main.c`).

### VFS_EP_OP_STATUS (0x0104, Fase 7.5)

Service health summary, used by svcmgr's DIAG aggregation (it EP_CALLs the
master ep cap it already holds). Request: no words; a bulk payload is
rejected (`IRIS_ERR_INVALID_ARG`). Reply OK: `words[1]` = ready exports,
`words[2]` = total exported bytes. The stateless protocol has no open-file
table, so the legacy opens/capacity counters report 0 in svcmgr's DIAG reply
(init's diag invariant checks both are 0). Runtime-tested by T033.

### IRIS_EP_OP_PING (0xFF01)

Health check; replies `IRIS_EP_REPLY_OK`, no payload.

## Error semantics (all ops)

| Error | Cause |
|-------|-------|
| `IRIS_ERR_INVALID_ARG` | missing words; empty / oversized / non-NUL-terminated path; payload announced but not delivered |
| `IRIS_ERR_NOT_FOUND` | no ready export with that name; LIST index out of range |
| `IRIS_ERR_NOT_SUPPORTED` | unknown opcode |

Exactly one reply is produced for every request, including malformed ones.

## Server loop (endpoint-only, Fase 7.5)

`vfs.c` blocks on the endpoint — there is no KChannel service loop left:

```
for (;;) {
    SYS_EP_RECV(ep_h, &req);     /* blocking; bootstrap channel is closed */
    vfs_ep_dispatch(...);
    SYS_REPLY(reply_cap, &reply);
}
```

- Each EP request is answered with `SYS_REPLY` **exactly once**; the reply cap
  is closed afterwards. A request without a reply cap (plain `EP_SEND`) is
  served and dropped.
- An EP_RECV failure halts the service loudly; there is no legacy fallback to
  hide a broken endpoint.
- Marker `[VFS] ep ready` is logged before `VFS ready` and gated by
  `scripts/run_qemu_headless.sh`.

## Clients (endpoint-only since Fase 7.2)

- **sh** (Fase 8): reaches vfs through the well-known slot
  `IRIS_CPTR_VFS_EP` (2), verified with a PING at boot — no lookup at all;
  prints `[SH] vfs cptr OK` / `FAILED`. `ls` and `cat` use LIST / READ_AT
  exclusively — the legacy fallback was removed in Fase 7.2. A broken slot
  fails the `[SH] vfs cptr OK` smoke gate instead of being masked.
- **init**: the S5/S6 healthy-path probes (Fase 7.2) resolve `"svcmgr.ep"`
  over the legacy lookup once, then EP_CALL `LOOKUP_NAME("vfs.ep")` with the
  standard retry/pause loop. S5 checks LIST 0–2 + out-of-range `NOT_FOUND`;
  S6 checks STAT + full READ_AT + EOF semantics + missing-file `NOT_FOUND`.
  Fail-fast: exit codes 4 (`svcmgr.ep`), 5 (`vfs.ep`), 9 (S5), 10 (S6); no
  legacy fallback. Gated by `[USER] vfs ep list OK` / `[USER] vfs ep read OK`.
- **iris_test**: T026–T030 exercise lookup, PING, READ_AT content + EOF,
  error codes and malformed-path rejection end-to-end; T031 covers `".ep"`
  lookup anti-spoofing. They FAIL (not skip) when the endpoint is missing.

## Limits

| Constant | Value |
|----------|-------|
| `VFS_EP_PATH_MAX` | 64 bytes including NUL (= legacy `VFS_MAX_NAME`, enforced by `_Static_assert` in vfs.c) |
| `VFS_EP_DATA_MAX` | 256 bytes per READ_AT reply (= `IRIS_IPC_BUF_SIZE`) |
| Service name | `VFS_EP_SVC_NAME` = `"vfs.ep"` |
