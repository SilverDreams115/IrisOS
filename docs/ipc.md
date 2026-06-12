# IRIS IPC Architecture

IRIS supports two IPC mechanisms: **KChannel** (legacy ring-buffer) and **KEndpoint** (seL4-style synchronous rendezvous). New code should use KEndpoint.

## KEndpoint (seL4-style, preferred)

KEndpoint provides synchronous rendezvous IPC. The sender blocks until a receiver is ready and vice versa — there is no message queue.

### State machine

```
        EP_SEND (sender(s) queued)
       /                          \
IDLE --                             -- IDLE (after rendezvous)
       \                          /
        EP_RECV (receiver(s) queued)
```

- `EP_STATE_IDLE`: no waiters.
- `EP_STATE_SEND`: one or more senders blocked waiting for a receiver.
- `EP_STATE_RECV`: one or more receivers blocked waiting for a sender.

### Syscalls

| Syscall | Number | Description |
|---------|--------|-------------|
| `SYS_ENDPOINT_CREATE` | 73 | Create a new endpoint (returns handle with all rights) |
| `SYS_EP_SEND` | 74 | Blocking send (blocks until rendezvous) |
| `SYS_EP_RECV` | 75 | Blocking receive (blocks until rendezvous) |
| `SYS_EP_NB_SEND` | 76 | Non-blocking send (returns `WOULD_BLOCK` if no receiver) |
| `SYS_EP_NB_RECV` | 77 | Non-blocking receive (returns `WOULD_BLOCK` if no sender) |
| `SYS_EP_CALL` | 93 | Send + block for reply (creates KReply, delivers to server) |
| `SYS_REPLY` | 94 | Invoke KReply to unblock the EP_CALL caller |

### Message format (`struct IrisMsg`, 64 bytes)

```
label        (8 bytes): operation identifier
words[4]     (32 bytes): fixed-size arguments
word_count   (4 bytes): number of valid words
buf_len      (4 bytes): bulk data length (0 = no bulk)
buf_uptr     (8 bytes): user address of bulk data buffer
attached_handle (4 bytes): handle to transfer (IRIS_MSG_NO_CAP = none)
attached_rights (4 bytes): rights to grant on attached handle
```

### Bulk data (kbuf)

Up to `IRIS_IPC_BUF_SIZE` (256) bytes of bulk data can be transferred per message. The sender sets `msg.buf_uptr` to point to the source buffer and `msg.buf_len` to the byte count. The receiver sets `msg.buf_uptr` to point to the destination buffer before calling `EP_RECV` or `EP_NB_RECV`.

For `EP_CALL`, `msg.buf_uptr` is used for both the send buffer (outbound) and the reply receive buffer (inbound). After `EP_CALL` returns, the same buffer contains the server's reply data.

### Capability transfer

A single capability can be transferred per message by setting `msg.attached_handle` and `msg.attached_rights`. The sender's handle is consumed at staging time (it must carry `RIGHT_TRANSFER`; the transferred rights are reduced to `attached_rights`). The receiver's message carries the newly installed handle in `msg.attached_handle`.

`EP_CALL` does **not** support request-side capability transfer; use `EP_SEND`/`EP_RECV` for that. The **reply** direction does support it — see below.

### Reply caps (KReply)

When `EP_CALL` is used:
1. Caller sends message and transitions to `TASK_BLOCKED_REPLY`.
2. Server receives via `EP_RECV` and gets a `KReply` handle in `msg.attached_handle`.
3. Server calls `SYS_REPLY(reply_h, reply_msg)` exactly once to unblock the caller.
4. If the server closes the reply handle without replying, the caller wakes with `IRIS_ERR_CLOSED`.
5. A second `SYS_REPLY` on the same handle returns `IRIS_ERR_NOT_FOUND` (one-shot guarantee).

### Reply-cap transfer (Fase 7.1 ABI extension)

`SYS_REPLY` can transfer one capability to the EP_CALL caller via
`reply_msg.attached_handle` / `attached_rights`, with the same staging
semantics as `EP_SEND` (the server's handle needs `RIGHT_TRANSFER`, is
consumed, and rights are reduced). Consumption contract:

| Outcome | Server handle | Caller sees |
|---------|--------------|-------------|
| Success | consumed | new handle in `msg.attached_handle` |
| Staging error (`BAD_HANDLE`, `ACCESS_DENIED`, …) | **not** consumed; KReply still usable | still blocked |
| Caller already gone (`NOT_FOUND`) | consumed (cap destroyed) | — |
| Caller handle table full | consumed (cap destroyed) | `IRIS_MSG_NO_CAP` |

This is what allows `IRIS_SVCMGR_EP_LOOKUP_NAME` to return service endpoint
caps over the EP path (used by sh, init and iris_test for `"vfs.ep"` —
endpoint-only for VFS operations since Fase 7.2 — and `"kbd.ep"`, Fase 7.4).
Covered by runtime tests T024/T025.

Reply caps may also be answered **deferred**: the server stashes the KReply
handle and replies later. kbd uses this for blocking key pulls
(`KBD_EP_OP_READ` parks the reply and answers it from the next IRQ scancode
— see `docs/kbd-endpoint.md`). The caller stays in `TASK_BLOCKED_REPLY`
until the deferred `SYS_REPLY` (or wakes with `IRIS_ERR_CLOSED` on KReply
teardown).

### Close semantics

When all handles to an endpoint are closed (`active_refs → 0`), `kendpoint_obj_close` fires:
- All blocked senders and receivers wake with `IRIS_ERR_CLOSED`.
- Staged caps are released.
- The endpoint queue is cleared.

### Rights

| Right | Effect |
|-------|--------|
| `RIGHT_WRITE` | Required for `EP_SEND`, `EP_NB_SEND`, `EP_CALL` |
| `RIGHT_READ` | Required for `EP_RECV`, `EP_NB_RECV` |

---

## KChannel (legacy, scheduled for migration)

KChannel provides asynchronous buffered IPC via a ring buffer (128 messages × 84 bytes). It uses `SYS_CHAN_RECV` / `SYS_CHAN_SEND` with a blocking waiter set.

KChannel remains fully supported. See `docs/kchannel-migration.md` for the migration plan.

---

## KNotification

KNotification provides signal-bit signaling (bitmask OR semantics). Used for IRQ delivery and async events. See `kernel/new_core/include/iris/nc/knotification.h`.

Since Fase 7.6, `SYS_IRQ_ROUTE_REGISTER` accepts a KNotification destination
(`RIGHT_WRITE`) in addition to the legacy KChannel: the kernel then signals
bit `1 << irq` from IRQ context (signal-only — no allocation, no blocking)
instead of enqueuing a message. The service blocks on
`SYS_NOTIFY_WAIT_TIMEOUT`, drains device state through its KIoPort cap and
re-arms with `SYS_IRQ_ACK`. A route holds either a channel or a
notification, never both (registering one replaces the other). First user:
kbd (catalog flag `irq_notify = 1`, WAIT side delivered at bootstrap kind
0x23).

---

## CPtr-first invocation (Fase 8)

The IPC, CNode, Untyped and Frame syscalls (via `cspace_or_handle_resolve_*`)
take one capability argument with a kernel-enforced namespace split (Fase 8):
values below 1024 are root-CNode slot indices (CPtrs) and resolve through
the CSpace **only** — a missing slot fails cleanly and `ACCESS_DENIED` is a
hard stop, with no handle-table fallback; values ≥ 1024 are handle ids
(`slot | generation << 10`, generation ≥ 1) and resolve through the handle
table **only** — they never walk the CSpace, so handle bit patterns cannot
alias populated slots. (Before the split, the radix walker masked the index
and a handle like 1027 could alias slot 3 — found and fixed in Fase 8;
regression-tested in `test_ipc_cspace.c`.)

A spawner mints caps directly into a child's root CNode with
`SYS_PROC_CSPACE_MINT` (syscall 104, exclusive: occupied slot →
`ALREADY_EXISTS`), normally **pre-start** via `svc_load_minted` so the child
sees its slots from its first instruction. The child invokes them by CPtr —
e.g. `SYS_EP_CALL(IRIS_CPTR_SVCMGR_EP, &msg)` — with no KChannel handle
transfer. See `docs/cptr-first-services.md` for the slot map, per-service
bootstrap flows and runtime coverage (T039–T046).

---

## Choosing between KEndpoint and KChannel

| Property | KEndpoint | KChannel |
|----------|-----------|----------|
| Semantics | Synchronous rendezvous | Async ring buffer |
| Capacity | No queue (O(1) space) | 128 messages |
| Bulk data | ≤256 bytes per call | ≤64 bytes per message |
| Reply pattern | Built-in (KReply) | Manual second channel |
| Multi-client | Yes (queue of callers) | Yes (multiple senders) |
| Close behavior | Wakes all blocked tasks | Seals: receivers get CLOSED |
| IRQ delivery | Not supported (cannot block in IRQ context) | Legacy route; new routes use KNotification (Fase 7.6) |
| Status | **Preferred for new code** | **Legacy, use in existing code** |
