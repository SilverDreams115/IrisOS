# IRIS Endpoint Protocol

The endpoint protocol (`kernel/include/iris/endpoint_proto.h`) is the wire format
services speak over a KEndpoint.

**Ledger A-32/A-33 changed how a request is carried, not what it says.** An
operation is an INVOCATION — a label sent to a capability — and a message is a
MessageInfo word plus message registers (`kernel/include/iris/ipc_msg.h`).
There is no message struct in the ABI and no pointer to one: `struct iris_msg`
in `services/common/iris_msg.h` is ring-3 marshalling, the way seL4's
`seL4_MessageInfo_t` and `seL4_SetMR` are, and the kernel has never seen it.

## Protocol model

All requests use `SYS_EP_CALL` (client) + `SYS_REPLY` (server):

```
Client                        Server
  |                             |
  |--- EP_Call(ep, req) ------> |   (rendezvous; client blocks)
  |                             |   server processes req
  |<-- Reply(reply_obj, res) -- |   (client unblocks)
  |                             |
```

- `msg.label` identifies the operation.  It rides in the MessageInfo's label
  field, which is 39 bits wide.
- `msg.words[0..3]` carry fixed-size arguments — four machine words, all of
  them in registers.
- Variable-length data goes in the sending thread's **registered IPC buffer**
  (`seL4_TCB_SetIPCBuffer`, ledger D-4), and `msg.buf_len` says how many bytes.
  There is no address: the bytes are in the page the thread registered because
  there is nowhere else they could be.  **Both ends need a buffer** — a payload
  lives in the sender's page and is copied to the receiver's, so two threads of
  one process each need their own.
- A capability travels in `msg.cap` (+ `msg.cap_rights`); a receiver says where
  it should land in `msg.recv_slot`, and reads back in `msg.got_caps` the
  RIGHTS it landed with — zero when nothing arrived, which is unambiguous
  because a capability with no rights cannot be transferred at all.

## Standard reply labels

| Label | Value | Meaning |
|-------|-------|---------|
| `IRIS_EP_REPLY_OK` | 0 | Success; operation-specific payload in `words[]`, and a capability in the slot the caller declared |
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
the NUL-terminated service name is in the caller's registered IPC buffer
msg.buf_len  = length including NUL (≤ IRIS_EP_SVCNAME_MAX = 128)
```

**Reply (success):**
```
reply.label   = IRIS_EP_REPLY_OK
reply.words[0] = 0 (IRIS_OK)
the service capability lands in the slot the caller declared in msg.recv_slot,
and msg.got_caps comes back holding the RIGHTS it was granted
```

A caller that declares no receive slot is handed nothing: since Stage 4 there
is no handle namespace to fall back to, and since A-33 the reply reports what
it delivered rather than what the server asked to deliver.  `msg.got_caps == 0`
means nothing arrived.

**Reply (failure):**
```
reply.label    = IRIS_EP_REPLY_ERR
reply.words[0] = error code (IRIS_ERR_NOT_FOUND, IRIS_ERR_NO_MEMORY, etc.)
```

### Reserved ".ep" names

Names ending in `".ep"` resolve to endpoint capabilities.  The suffix is a
leftover of the migration that retired KChannel: for a while a plain name and
an `".ep"` name resolved to different KINDS of object, and the suffix said
which.  There is one kind now, and the names are kept because services are
registered under them.

| Name | Resolves to | Granted rights |
|------|------------|----------------|
| `"svcmgr.ep"` | svcmgr's own discovery endpoint | `RIGHT_WRITE \| RIGHT_TRANSFER \| RIGHT_DUPLICATE` (distributable discovery cap — TRANSFER for KChannel attach, DUPLICATE for CSpace mint; only grants EP_CALL, never recv) |
| `"<image_name>.ep"` | the service's endpoint, if its catalog entry has `own_service_ep = 1` (today: `"vfs.ep"`, `"kbd.ep"`) | `RIGHT_WRITE` |
| `"console.ep"` | the console endpoint (Phase 7.3) — console is init-spawned, not catalog, so **init** creates the endpoint and delivers the send side to svcmgr at bootstrap (kind 0x22); see `docs/console-endpoint.md` | `RIGHT_WRITE` |

`SVCMGR_MSG_REGISTER` rejects dynamic names ending in `".ep"` with
`IRIS_ERR_INVALID_ARG` so a rogue service cannot shadow an endpoint name.
Runtime coverage (Phase 7.2): init S4 attempts to register `"spoof.ep"` and
verifies the name stays unresolvable; iris_test T031 verifies the EP lookup
of an unpublished `".ep"` name returns `NOT_FOUND` with no cap attached.

### IRIS_SVCMGR_EP_REGISTER (0xF002)

Register a service endpoint with svcmgr.

> **Status: unimplementable as specified.** The kernel forbids request-side
> capability transfer on `EP_CALL`, so a service cannot attach its endpoint
> cap to a REGISTER request. Phase 7.1 inverts the ownership instead: svcmgr
> **creates** the endpoint for catalog services flagged `own_service_ep = 1`,
> sends the receive side at bootstrap (kind `SVCMGR_BOOTSTRAP_KIND_SERVICE_EP`
> = 0x21) and publishes the send side as `"<image_name>.ep"`. This also keeps
> the endpoint master alive across service restarts, so client caps stay
> valid. The opcode is kept reserved for a future CPtr/badge-based transfer
> mechanism.

**Request (reserved):**
```
msg.label      = IRIS_SVCMGR_EP_REGISTER
msg.cap        = the service endpoint capability to hand over
msg.cap_rights = the rights to hand it over with
the NUL-terminated service name is in the caller's registered IPC buffer
msg.buf_len    = length including NUL
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

**Reply (success):** the service capability lands in the declared receive slot
(same shape as LOOKUP_NAME).

## Bootstrap kinds

| Kind | Value | Carries |
|------|-------|---------|
| `SVCMGR_BOOTSTRAP_KIND_SVCMGR_EP` | 0x20 | svcmgr discovery endpoint (send side) — every catalog service receives it; init forwards it to iris_test |
| `SVCMGR_BOOTSTRAP_KIND_SERVICE_EP` | 0x21 | the service's **own** endpoint (receive side, `RIGHT_READ`) for catalog entries with `own_service_ep = 1`; sent **before** `INITRD_CAP` so bootstrap loops that exit on the initrd cap still see it. Also reused by init→console for the console endpoint's receive side (Phase 7.3) |
| `SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP` | 0x22 | send side of the console endpoint (`RIGHT_WRITE \| RIGHT_DUPLICATE \| RIGHT_TRANSFER`), init → svcmgr; svcmgr publishes it as `"console.ep"` (Phase 7.3) |
| `SVCMGR_BOOTSTRAP_KIND_IRQ_NOTIFY` | 0x23 | WAIT side of the IRQ KNotification for catalog entries with `irq_notify = 1` (Phase 7.6; today: kbd). The kernel signals bit `1 << irq` on each routed IRQ; the service drains device state via its KIoPort cap and re-arms with `SYS_IRQ_ACK` |

## Well-known CPtr slots (Phase 8)

CPtr-first bootstrap handoff: the spawner mints capabilities directly into
the child's root CNode with `SYS_CSPACE_MINT(src, dest_slot, rights|badge<<32,
dest_cnode)`, where `dest_cnode` is the child's root CNode — the spawner holds
it because it retyped it (Stage 6-pure).  It needs `RIGHT_DUPLICATE` on the
source cap, rights can only be reduced, and an occupied destination slot fails
`ALREADY_EXISTS`.  Minting happens **pre-start** — after the child's objects
are retyped and before `SYS_TCB_RESUME` — so the child sees its slots populated
from its first instruction: no bootstrap barrier, no races.  The child invokes
the cap **by CPtr** — e.g. `SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg)`.

(Until Stage 7 Step 9 this was `SYS_PROC_CSPACE_MINT(proc_h, slot, src_h,
rights)`, syscall 104, which named the PROCESS owning the destination CSpace.
It is retired: naming a process to reach a namespace you were never handed was
the last place a process capability granted access to an object its holder did
not hold.)

There is ONE argument namespace.  Stage 4 deleted the handle table, so a
syscall argument is a CPtr or it is `INVALID_ARG`.  Historically CPtrs and
handle_ids shared the namespace and, since Phase 8, the dual
resolvers **enforced** the split: values < 1024 resolved through the CSpace only
(missing slot fails cleanly, `ACCESS_DENIED` is a hard stop, no
handle-table fallback) and values ≥ 1024 (`slot | generation << 10`,
generation ≥ 1) resolved through the handle table only — they never walked the
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

## Example: a service lookup (C)

```c
#include "../common/iris_msg.h"          /* ring-3 marshalling */
#include <iris/endpoint_proto.h>
#include <iris/invoke.h>

/* `slot` is an empty slot of the caller's own CSpace: where the service
 * capability should land.  Nothing arrives without one. */
static long ep_lookup_name(long svcmgr_ep, const char *name, long slot,
                           uint8_t *ipc_buf)
{
    uint32_t n = 0;
    while (name[n]) { ipc_buf[n] = (uint8_t)name[n]; n++; }
    ipc_buf[n++] = 0;                     /* the payload is in OUR buffer */

    struct iris_msg m;
    iris_msg_zero(&m);
    m.label     = IRIS_SVCMGR_EP_LOOKUP_NAME;
    m.buf_len   = n;                      /* a length, not an address */
    m.recv_slot = slot;

    if (iris_msg_call(svcmgr_ep, &m) != 0)   return -1;
    if (m.label != IRIS_EP_REPLY_OK)         return -1;
    if (m.got_caps == 0u)                    return -1;   /* nothing arrived */
    return slot;                          /* ...and m.got_caps are its rights */
}
```

`struct iris_msg` is a marshalling record, not the ABI: the kernel holds no
pointer to it.  The words go out in registers and come back in registers.

## Capability transfer

- A message carries at most ONE capability, in `msg.cap` with the rights to
  hand it over in `msg.cap_rights`.  The kernel STAGES it — the sender must
  really hold it, the rights are reduced to what was asked for, and the badge
  is preserved — so the raw number a sender writes is never delivered as
  written.
- Since **A-29** the transfer is a COPY: the sender keeps what it sent and the
  receiver's capability is a revocable derivation CHILD of the sender's slot.
  Giving something away is send-then-delete, two steps that both belong to the
  sender.
- A receiver says where a capability should land in `msg.recv_slot` and learns
  what it got in `msg.got_caps` — the RIGHTS, or zero for nothing.  It does not
  need to be told WHERE: it declared the slot.
- A **Call** is handed two capabilities on the receiving side — the caller's
  gift, in the declared slot, and the reply object it is now owed, in
  `msg.got_cap`.  They used to share one field; **A-33** gave each its own.
- `Reply` carries a capability the same way.  `ReplyRecv` does not, and the
  syscall enforces it: a server's loop hands back the words its receive
  delivered, and those include the reply object it must not give away.
- KReply is one-shot, and a reply always delivers `sender_badge = 0`: reply
  identity is implied by the object, never by a badge a server could write.

Registration is cap-backed: a client hands its service endpoint over the
endpoint and svcmgr stores the real capability (validated as `KOBJ_ENDPOINT`),
so a later `LOOKUP_NAME` mints from it and the caller gets a usable capability
to the same object — a derivation child of svcmgr's, and revocable from it.
See [service-lifecycle.md](service-lifecycle.md).
