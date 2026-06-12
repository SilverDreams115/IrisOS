# KBD Endpoint Protocol (Fase 7.4)

The keyboard service delivers key events to sh **exclusively** over KEndpoint
(`"kbd.ep"`). The wire format is `kernel/include/iris/kbd_ep_proto.h`; the
server is `services/kbd/main.S` (ring-3 assembly). This replaces the Class D
KChannel debt `kbd event channel (sh ← kbd)` from `docs/kchannel-migration.md`.

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
4. **How does the consumer block without polling?** The kernel creates a
   per-call **KReply** for every `EP_CALL`; the receiver may answer it
   *later* (`SYS_REPLY` on the stashed cap). kbd parks the reply cap when no
   event is buffered and answers it from the next IRQ scancode. The
   consumer's `EP_CALL(KBD_EP_OP_READ)` therefore doubles as the blocking
   wait — no busy-poll, no sleep loops, no extra notification object.
5. **Bursts?** A 16-deep scancode ring absorbs typing while the consumer is
   processing; on overflow the OLDEST event is dropped (newest kept).
6. **Consumer dies?** kbd's deferred `SYS_REPLY` fails; the cap is closed,
   nothing leaks, no event is delivered twice.
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

Blocking pull. Ring non-empty → immediate reply. Ring empty → the per-call
KReply cap is **parked** (at most one; a second concurrent READ gets
`IRIS_ERR_WOULD_BLOCK`) and answered from the next IRQ scancode. Single
interactive consumer (sh) by design.

### IRIS_EP_OP_PING (0xFF01)

Health check; replies `IRIS_EP_REPLY_OK`. Served even while a READ is parked.

## Server loop (services/kbd/main.S)

```
for (;;) {
    while (SYS_EP_NB_RECV(kbd_ep_h) == OK)   /* drain EP requests */
        dispatch → SYS_REPLY (or park);
    SYS_CHAN_RECV_TIMEOUT(service_h, 10ms);  /* IRQ + legacy probes */
    on KBD_MSG_IRQ_SCANCODE:
        read port 0x60, SYS_IRQ_ACK;
        parked reply? answer it : push ring (drop-oldest);
        forward to legacy subscriber if one is registered;
}
```

EP constants are mirrored as plain hex for the assembler
(`KBD_EP_PING_OP`, `KBD_EP_E_*`, `KBD_EP_KIND_SERVICE_EP`) and sync-checked
against `endpoint_proto.h`/`nc/error.h` with `_Static_assert` when the header
is included from C (iris_test does).

## Discovery and bootstrap

- Catalog: kbd has `own_service_ep = 1`; svcmgr creates the endpoint, sends
  the recv side at bootstrap (kind 0x21) and publishes `"kbd.ep"`.
- sh resolves `"kbd.ep"` through the svcmgr discovery endpoint and prints
  `[SH] kbd ep OK` / `[SH] kbd ep FAILED` (gated by
  `scripts/run_qemu_headless.sh`; no silent fallback).
- `SVCMGR_BOOTSTRAP_KIND_KBD_CAP` (9) and the `give_kbd` catalog flag are
  retired; svcmgr no longer forwards the kbd write-end to sh.

## What remains on KChannel (Class D residue)

- **Kernel IRQ delivery**: IRQ1 still arrives as `KBD_MSG_IRQ_SCANCODE` on
  kbd's legacy service channel. Moving this to IRQ→KNotification is kernel
  work (proposed Fase 7.6) — `EP_CALL`/`SYS_REPLY` must never run in IRQ
  context.
- **init S2/S7 probes** (`HELLO`, `SUBSCRIBE`) and **svcmgr STATUS** stay on
  the legacy pair. init **unsubscribes after the S7 gate** so sh's EP pull is
  the only live key consumer (no double echo); the subscriber path remains
  only as a probed legacy mechanism.

## Tests

| Test | What it proves |
|------|----------------|
| iris_test T034 | `"kbd.ep"` resolves to a real KEndpoint; PING answers OK |
| iris_test T035 | empty POLL → `WOULD_BLOCK`; unknown op → `NOT_SUPPORTED`; bulk payload → `INVALID_ARG` — all answered while sh's READ is parked |
| smoke marker `[SH] kbd ep OK` | sh resolved and uses the endpoint path |
| QMP `send-key` (manual, documented) | full chain IRQ → ring/parked reply → sh echo + dispatch verified interactively |
