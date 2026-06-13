# IRIS Endpoint Protocol

The endpoint protocol (`kernel/include/iris/endpoint_proto.h`) defines the IrisMsg-based wire format for services that communicate over KEndpoint rather than KChannel.

## Protocol model

All requests use `SYS_EP_CALL` (client) + `SYS_REPLY` (server):

```
Client                    Server
  |                         |
  |--- EP_CALL(ep, req) --> |   (rendezvous; client blocks)
  |                         |   server processes req
  |<-- SYS_REPLY(rh, res)-- |   (client unblocks)
  |                         |
```

- `msg.label` identifies the operation.
- `msg.words[0..3]` carry fixed-size arguments (up to 4 × uint64_t).
- Variable-length data goes in the bulk kbuf (`msg.buf_uptr` / `msg.buf_len`, ≤256 bytes).

## Standard reply labels

| Label | Value | Meaning |
|-------|-------|---------|
| `IRIS_EP_REPLY_OK` | 0 | Success; operation-specific payload in `words[]` and `attached_handle` |
| `IRIS_EP_REPLY_ERR` | 1 | Failure; `words[0]` = `iris_error_t` error code |

## Generic service opcodes

| Opcode | Value | Description |
|--------|-------|-------------|
| `IRIS_EP_OP_PING` | 0xFF01 | Health check; server replies `REPLY_OK` |
| `IRIS_EP_OP_SHUTDOWN` | 0xFF02 | Request graceful shutdown |

## Svcmgr endpoint protocol

The svcmgr creates a KEndpoint (`state->ep_h`) at startup and distributes it to all catalog services via bootstrap (kind `SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP = 0x20`). Services can use EP_CALL on this endpoint instead of the KChannel-based SVCMGR_MSG_LOOKUP_NAME.

### IRIS_SVCMGR_EP_LOOKUP_NAME (0xF001)

Look up a registered service by name.

**Request:**
```
msg.label   = IRIS_SVCMGR_EP_LOOKUP_NAME
msg.buf_uptr → NUL-terminated service name
msg.buf_len  = length including NUL (≤ IRIS_EP_SVCNAME_MAX = 128)
```

**Reply (success):**
```
reply.label              = IRIS_EP_REPLY_OK
reply.words[0]           = 0 (IRIS_OK)
reply.attached_handle    = service cap (caller-owned; KChannel for plain names,
                           KEndpoint for ".ep" names)
reply.attached_rights    = granted rights
```

The cap delivery relies on the `SYS_REPLY` reply-cap transfer extension
(Fase 7.1, see `docs/ipc.md`). If the caller's handle table is full the reply
arrives with `attached_handle = IRIS_MSG_NO_CAP`.

**Reply (failure):**
```
reply.label    = IRIS_EP_REPLY_ERR
reply.words[0] = error code (IRIS_ERR_NOT_FOUND, IRIS_ERR_NO_MEMORY, etc.)
```

### Reserved ".ep" names (Fase 7.1)

Names ending in `".ep"` resolve to KEndpoint send caps instead of KChannels,
through **both** `IRIS_SVCMGR_EP_LOOKUP_NAME` and the legacy
`SVCMGR_MSG_LOOKUP_NAME`:

| Name | Resolves to | Granted rights |
|------|------------|----------------|
| `"svcmgr.ep"` | svcmgr's own discovery endpoint | `RIGHT_WRITE \| RIGHT_TRANSFER \| RIGHT_DUPLICATE` (distributable discovery cap — TRANSFER for KChannel attach, DUPLICATE for CSpace mint; only grants EP_CALL, never recv) |
| `"<image_name>.ep"` | the service's endpoint, if its catalog entry has `own_service_ep = 1` (today: `"vfs.ep"`, `"kbd.ep"`) | `RIGHT_WRITE` |
| `"console.ep"` | the console endpoint (Fase 7.3) — console is init-spawned, not catalog, so **init** creates the endpoint and delivers the send side to svcmgr at bootstrap (kind 0x22); see `docs/console-endpoint.md` | `RIGHT_WRITE` |

`SVCMGR_MSG_REGISTER` rejects dynamic names ending in `".ep"` with
`IRIS_ERR_INVALID_ARG` so a rogue service cannot shadow an endpoint name.
Runtime coverage (Fase 7.2): init S4 attempts to register `"spoof.ep"` and
verifies the name stays unresolvable; iris_test T031 verifies the EP lookup
of an unpublished `".ep"` name returns `NOT_FOUND` with no cap attached.

### IRIS_SVCMGR_EP_REGISTER (0xF002)

Register a service endpoint with svcmgr.

> **Status: unimplementable as specified.** The kernel forbids request-side
> capability transfer on `EP_CALL`, so a service cannot attach its endpoint
> cap to a REGISTER request. Fase 7.1 inverts the ownership instead: svcmgr
> **creates** the endpoint for catalog services flagged `own_service_ep = 1`,
> sends the receive side at bootstrap (kind `SVCMGR_BOOTSTRAP_KIND_SERVICE_EP`
> = 0x21) and publishes the send side as `"<image_name>.ep"`. This also keeps
> the endpoint master alive across service restarts, so client caps stay
> valid. The opcode is kept reserved for a future CPtr/badge-based transfer
> mechanism.

**Request (reserved):**
```
msg.label              = IRIS_SVCMGR_EP_REGISTER
msg.attached_handle    = service endpoint cap (transferred to svcmgr)
msg.buf_uptr → NUL-terminated service name
msg.buf_len  = length including NUL
```

**Reply (success):**
```
reply.label    = IRIS_EP_REPLY_OK
reply.words[0] = assigned service_id (uint32_t)
```

### IRIS_SVCMGR_EP_UNREGISTER (0xF003)

Unregister a previously registered service.

**Request:**
```
msg.label    = IRIS_SVCMGR_EP_UNREGISTER
msg.words[0] = service_id from REGISTER reply
```

**Reply:** `IRIS_EP_REPLY_OK` or `IRIS_EP_REPLY_ERR`.

### IRIS_SVCMGR_EP_LOOKUP_ID (0xF004)

Resolve a service by numeric ID.

**Request:**
```
msg.label    = IRIS_SVCMGR_EP_LOOKUP_ID
msg.words[0] = service_id (uint32_t)
```

**Reply (success):** `attached_handle` = service cap (same format as LOOKUP_NAME).

## Bootstrap kinds

| Kind | Value | Carries |
|------|-------|---------|
| `SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP` | 0x20 | svcmgr discovery endpoint (send side) — every catalog service receives it; init forwards it to iris_test |
| `SVCMGR_BOOTSTRAP_KIND_SERVICE_EP` | 0x21 | the service's **own** endpoint (receive side, `RIGHT_READ`) for catalog entries with `own_service_ep = 1`; sent **before** `INITRD_CAP` so bootstrap loops that exit on the initrd cap still see it. Also reused by init→console for the console endpoint's receive side (Fase 7.3) |
| `SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP` | 0x22 | send side of the console endpoint (`RIGHT_WRITE \| RIGHT_DUPLICATE \| RIGHT_TRANSFER`), init → svcmgr; svcmgr publishes it as `"console.ep"` (Fase 7.3) |
| `SVCMGR_BOOTSTRAP_KIND_IRQ_NOTIFY` | 0x23 | WAIT side of the IRQ KNotification for catalog entries with `irq_notify = 1` (Fase 7.6; today: kbd). The kernel signals bit `1 << irq` on each routed IRQ; the service drains device state via its KIoPort cap and re-arms with `SYS_IRQ_ACK` |

## Well-known CPtr slots (Fase 8)

CPtr-first bootstrap handoff: the spawner mints capabilities directly into
the child's root CNode with `SYS_PROC_CSPACE_MINT(proc_h, slot, src_h,
rights)` (syscall 104; needs `RIGHT_WRITE` on the child process cap and
`RIGHT_DUPLICATE` on the source cap; rights can only be reduced; an occupied
destination slot fails `ALREADY_EXISTS`). Minting happens **pre-start**
(`svc_load_minted`, between process creation and `SYS_THREAD_START`), so the
child sees its slots populated from its first instruction — no bootstrap
barrier, no races. The child invokes the cap **by CPtr** — e.g.
`SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg)` — with no KChannel handle transfer.

CPtrs and handle_ids share one argument namespace; since Fase 8 the dual
resolvers **enforce** it: values < 1024 resolve through the CSpace only
(missing slot fails cleanly, `ACCESS_DENIED` is a hard stop, no
handle-table fallback) and values ≥ 1024 (`slot | generation << 10`,
generation ≥ 1) resolve through the handle table only — they never walk the
CSpace, so populated low slots cannot be aliased by handle bit patterns.

Full slot layout, per-service bootstrap flows and the remaining handle
boundary: **`docs/cptr-first-services.md`**. Summary:

| Slot | Name | Carries |
|------|------|---------|
| 0 | `CPTR_NULL` | always invalid |
| 1 | `IRIS_CPTR_SVCMGR_EP` | svcmgr discovery endpoint (call side, WRITE) |
| 2 | `IRIS_CPTR_VFS_EP` | `"vfs.ep"` (call side, WRITE) |
| 3 | `IRIS_CPTR_CONSOLE_EP` | `"console.ep"` (call side, WRITE; svcmgr's copy adds DUPLICATE\|TRANSFER) |
| 4 | `IRIS_CPTR_KBD_EP` | `"kbd.ep"` (call side, WRITE) |
| 5 | `IRIS_CPTR_OWN_EP` | the service's own endpoint (recv side, READ) |
| 7 | `IRIS_CPTR_IRQ_NOTIFY` | IRQ KNotification (WAIT side) |
| 30/31 | `IRIS_CPTR_TEST_FIX_A/B` | iris_test failure fixtures |

Bootstrap kinds 0x20/0x21/0x22/0x23 are **retired** (replaced by these
mints); their values stay reserved.

Runtime coverage: gated markers `[SH] svcmgr/vfs/console/kbd cptr OK`,
`[VFS] console cptr OK`, `[IRIS][TEST] console cptr write OK`; tests
T039–T046 (positive paths per service, null/wrong-type/denied without
fallback, slot typing, client-slot rights reduction, legacy lookup interop).

## Opcode ranges

| Range | Owner |
|-------|-------|
| 0x0000–0x00FF | Standard protocol (reserved) |
| 0x0100–0xEFFF | Individual services (VFS owns 0x01xx — `docs/vfs-endpoint.md`; KBD owns 0x02xx — `docs/kbd-endpoint.md`; console owns 0x03xx — `docs/console-endpoint.md`) |
| 0xF000–0xFEFF | Svcmgr endpoint protocol |
| 0xFF00–0xFFFF | Generic service management (ping, shutdown) |

## Example: EP_CALL-based service lookup (C)

```c
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/syscall.h>

static handle_id_t ep_lookup_name(handle_id_t svcmgr_ep, const char *name) {
    static char reply_buf[64];
    struct IrisMsg msg = {0};
    uint32_t namelen = 0;
    while (name[namelen]) namelen++;
    namelen++;  /* include NUL */

    msg.label   = IRIS_SVCMGR_EP_LOOKUP_NAME;
    msg.buf_uptr = (uint64_t)(uintptr_t)name;   /* send buffer = name */
    msg.buf_len  = namelen;

    long r = syscall(SYS_EP_CALL, svcmgr_ep, (long)&msg);
    if (r != 0 || msg.label != IRIS_EP_REPLY_OK)
        return HANDLE_INVALID;
    return (handle_id_t)msg.attached_handle;
}
```

Note: `EP_CALL` reuses `msg.buf_uptr` as both the send buffer and the reply receive buffer. If the lookup reply includes bulk data, it overwrites the name buffer.

## Fase 11 — endpoint capability transfer

`struct IrisMsg` grew to **80 bytes** with a second cap slot
`attached_cap` (+`attached_cap_rights`) at offset 72 (all prior offsets
unchanged; `_Static_assert`s in `iris/ipc_msg.h`; kbd asm buffer bumped to 80).

- On **EP_CALL** the reply cap occupies `attached_handle`, so the client puts
  the capability it wants to transfer in `attached_cap`. The kernel **stages**
  it (the caller must really hold it; rights are reduced to
  `attached_cap_rights`; badge preserved) and clears the raw `attached_cap`
  number — it is never delivered as written (anti-spoof, same contract as
  `attached_handle`).
- The **server** receives the transferred cap as a real handle in
  `attached_cap` and the reply cap in `attached_handle` — the two never
  collide. Move vs duplicate is governed by the rights the sender grants
  (TRANSFER moves authority onward; DUPLICATE lets svcmgr re-hand it).
- **EP_SEND/EP_NB_SEND/EP_REPLY** are unchanged — they keep using
  `attached_handle` for their single transferred cap.
- KReply is unchanged: still one-shot, reply still forces `sender_badge = 0`.

This unblocks **cap-backed `IRIS_SVCMGR_EP_REGISTER`**: a client transfers its
service endpoint over the EP and svcmgr stores the real cap (validated as
`KOBJ_ENDPOINT`), so a later `LOOKUP_NAME` returns a usable cap to the same
object. See [service-lifecycle.md](service-lifecycle.md).
