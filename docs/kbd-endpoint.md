# KBD Endpoint Protocol (Phase 7.4)

The keyboard service delivers key events to sh **exclusively** over KEndpoint
(`"kbd.ep"`). The wire format is `kernel/include/iris/kbd_ep_proto.h`; the
server is `services/kbd/main.S` (ring-3 assembly). It replaced the Class D
KChannel debt `kbd event channel (sh ← kbd)` from `docs/kchannel-migration.md`,
and since Phase 13 there is nothing else left: kbd is endpoint-only.

## Design: pull with parked reply (seL4-style deferred reply)

Design questions and answers that selected this shape:

1. **Does a key event need an immediate payload?** Yes — the raw scancode
   (1 byte). A bare KNotification signal cannot carry it (IRIS KNotification
   is a bit-OR wakeup, no payload queue), so notification-only was rejected.
2. **Does the payload fit an endpoint message?** Trivially: `words[1]`.
3. **Push or pull?** Pull. Push (`EP_SEND` from kbd) can block the driver
   when the consumer is slow — unacceptable for the IRQ service path. With
   pull, kbd never blocks on delivery and backpressure is exactly one
   in-flight event per consumer call.
4. **How does the consumer block without polling?** Every `EP_Call` binds a
   **KReply**, which the receiver may answer *later* (Phase S1: the object is
   the server's own, passed to the receive and handed back in `got_cap`). kbd
   parks the reply object when no event is buffered and answers it from the
   next IRQ scancode. The consumer's `EP_Call(KBD_EP_OP_READ)` therefore
   doubles as the blocking wait — no busy-poll, no sleep loops, no extra
   notification object.
5. **Bursts?** A 16-deep scancode ring absorbs typing while the consumer is
   processing; on overflow the OLDEST event is dropped (newest kept).
6. **Consumer dies?** the binding is dropped, so kbd's deferred `Reply`
   answers `NOT_FOUND`; the object returns to free and is reusable, nothing
   leaks, and no event is delivered twice.
7. **kbd dies?** KReply teardown wakes the parked caller with an error; sh
   retries (yield + re-call) and svcmgr's restart policy respawns kbd. The
   endpoint cap stays valid across restarts (svcmgr keeps the master).

## Operations

Requests carry no bulk payload; `buf_len > 0` → `IRIS_ERR_INVALID_ARG`.
Unknown opcodes → `IRIS_ERR_NOT_SUPPORTED`. Exactly one reply per request.

### KBD_EP_OP_POLL (0x0201)

Non-blocking fetch. Reply OK with `words[1]` = oldest buffered scancode, or
`IRIS_ERR_WOULD_BLOCK` when the ring is empty. Never parks.

### KBD_EP_OP_READ (0x0202)

Blocking pull. Ring non-empty → immediate reply. Ring empty → the reply
object is **parked** (at most one; a second concurrent READ gets
`IRIS_ERR_WOULD_BLOCK`) and answered from the next IRQ scancode. kbd owns TWO
reply objects (slots 13/14) and receives with whichever is not parked, which
is what lets it keep serving while one caller waits. Single interactive
consumer (sh) by design.

### IRIS_EP_OP_PING (0xFF01)

Health check; replies `IRIS_EP_REPLY_OK`. Served even while a READ is parked.

## Server loop (services/kbd/main.S)

```
TCB_BindNotification(OWN_TCB, irq_notification)   /* once, at startup */
for (;;) {
    EP_Recv(kbd_ep, no_recv_slot, free_reply_object)   /* BLOCKING */
    label == IRIS_MSG_LABEL_NOTIFICATION ?             /* a keystroke */
        read port 0x60 via the KIoPort cap; IRQ_Ack;
        parked reply? answer it : push ring (drop-oldest)
      : dispatch POLL / READ / PING → Reply (or park)
}
```

**One blocking point, and idle costs nothing.** This is ledger A-23's whole
point. The loop used to drain the endpoint non-blockingly and then sleep 10 ms
on the IRQ notification, because a thread blocked receiving on an endpoint was
deaf to signals and serving both meant waking up to check: a hundred wakeups a
second, to find nothing, forever. The notification is BOUND to the thread now,
so a keystroke wakes it out of the endpoint receive and arrives as a message
labelled `IRIS_MSG_LABEL_NOTIFICATION` — which is how a server tells "somebody
called me" from "somebody signalled me" on one receive.

kbd reads the ABI the same way C does: `iris/invoke.h` and `iris/ipc_msg.h`
are both assembler-safe on purpose (plain `#define`s, no `u` suffixes, the
C-only parts behind `#ifndef __ASSEMBLER__`), so the driver shifts by
`IRIS_MI_LABEL_SHIFT` and invokes `INV_EP_RECV` rather than mirroring hex
constants that could drift. The remaining service-protocol mirrors
(`KBD_EP_PING_OP`, `KBD_EP_E_*`) are sync-checked against
`endpoint_proto.h`/`nc/error.h` with `_Static_assert` when the header is
included from C (iris_test does).

## Discovery and bootstrap

- Catalog: kbd has `own_service_ep = 1`; svcmgr creates the endpoint,
  pre-start-mints the recv side at `IRIS_CPTR_OWN_EP` (slot 5; bootstrap
  kind 0x21 retired in Phase 8) and publishes `"kbd.ep"`.
- sh (Phase 8) reaches kbd through the well-known slot `IRIS_CPTR_KBD_EP`
  (4), verified with a PING; prints `[SH] kbd cptr OK` / `FAILED` (gated by
  `scripts/run_qemu_headless.sh`; no silent fallback, no lookup).
- `SVCMGR_BOOTSTRAP_KIND_KBD_CAP` (9) and the `give_kbd` catalog flag are
  retired; svcmgr no longer forwards the kbd write-end to sh.
- kbd's own TCB is `IRIS_CPTR_OWN_TCB` (slot 19) — it has to name itself to
  bind its notification, which is what a thread capability is for.

## IRQ delivery (Phase 7.6: KNotification)

IRQ1 no longer arrives as a `KBD_MSG_IRQ_SCANCODE` KChannel message. The
catalog flags kbd `irq_notify = 1`: svcmgr creates a KNotification master
(kept across restarts), registers it as the kernel IRQ route
(`SYS_IRQ_ROUTE_REGISTER` accepts a KNotification with `RIGHT_WRITE` since
Phase 7.6) and pre-start-mints the WAIT side at `IRIS_CPTR_IRQ_NOTIFY`
(slot 7; bootstrap kind 0x23 retired in Phase 8 — kbd uses the slot as a
constant). On each IRQ the kernel masks the line, signals bit `1 << irq`
(signal-only — safe from IRQ context, no allocation) and EOIs; kbd wakes out
of its endpoint receive (A-23), reads port 0x60 via its KIoPort cap and
re-arms with `IRQ_Ack`. `KBD_MSG_IRQ_SCANCODE` is no longer dispatched.

## What remains on KChannel

Nothing. The legacy probe pair (`HELLO`, `SUBSCRIBE`, svcmgr `STATUS`) and the
bootstrap one-shot channel went with KChannel in Phase 13; every capability
kbd holds is a pre-start CSpace mint and every message it serves is an
endpoint message.

## Tests

| Test | What it proves |
|------|----------------|
| iris_test T034 | `"kbd.ep"` resolves to a real KEndpoint; PING answers OK |
| iris_test T035 | empty POLL → `WOULD_BLOCK`; unknown op → `NOT_SUPPORTED`; bulk payload → `INVALID_ARG` — all answered while sh's READ is parked |
| smoke marker `[SH] kbd ep OK` | sh resolved and uses the endpoint path |
| QMP `send-key` (manual, documented) | full chain IRQ → ring/parked reply → sh echo + dispatch verified interactively |
