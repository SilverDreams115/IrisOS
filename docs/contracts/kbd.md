# `kbd` contract

## Purpose

Defines the current keyboard service contract for liveness, status, scancode forwarding, and hardware I/O ownership.

## Ownership

`kbd` owns:

- PS/2 controller I/O in userland via `KIoPort`
- client-visible keyboard liveness and status replies
- a 16-deep scancode ring, and the parked reply that drains it

The kernel still owns:

- low-level IRQ reception
- routing the IRQ line to a KNotification

## Bootstrap contract

`kbd` is spawned by `svcmgr` with `RBX` = 0 — the bootstrap channel went with
KChannel in Phase 13. Everything it holds is a pre-start CSpace mint:

| Slot | Capability |
|---|---|
| `IRIS_CPTR_OWN_EP` (5) | the receive side of `"kbd.ep"` |
| `IRIS_CPTR_IRQ_NOTIFY` (7) | the WAIT side of the IRQ1 notification |
| `IRIS_CPTR_IOPORT` (10) | the `KIoPort` for the PS/2 ports |
| 13 / 14 | its two reply objects |
| `IRIS_CPTR_OWN_TCB` (19) | itself — needed to bind the notification |

A missing endpoint leaves kbd with no request surface and fails the smoke
gate.

## IRQ-facing contract (Phase 7.6: KNotification)

Keyboard IRQ delivery uses the generic IRQ routing layer with a
**KNotification** destination (catalog flag `irq_notify = 1`):

- svcmgr owns the notification master (kept across restarts) and registers
  it as the IRQ1 route; the WAIT side reaches `kbd` as a pre-start CSpace
  mint at `IRIS_CPTR_IRQ_NOTIFY` (slot 7; bootstrap kind 0x23 retired in
  Phase 8)
- on each IRQ the kernel signals bit `1 << irq` (signal-only; no message)
- `kbd` binds that notification to its own thread (`TCB_BindNotification`,
  ledger A-23), so the signal wakes it out of its blocking endpoint receive
  and arrives as a message labelled `IRIS_MSG_LABEL_NOTIFICATION`; it reads
  the scancode byte from port 0x60 via its `KIoPort` cap and re-arms with
  `IRQ_Ack`. One blocking point, and idle costs nothing — before A-23 the
  loop woke a hundred times a second to serve two wait surfaces

> **Historical (retired in Phase 7.6):** IRQ1 used to be routed to the kbd
> public service channel as `KBD_MSG_IRQ_SCANCODE` messages. That delivery
> path is no longer dispatched; the opcode remains defined only as a
> historical constant.

## Client request/response surface

Everything reaches consumers over the `"kbd.ep"` KEndpoint. Full semantics in
`docs/kbd-endpoint.md`:

| Opcode | Value | Semantics |
|---|---|---|
| `KBD_EP_OP_POLL` | 0x0201 | non-blocking: oldest buffered scancode in `words[1]`, or `WOULD_BLOCK` |
| `KBD_EP_OP_READ` | 0x0202 | blocking pull: answered now, or the reply object is PARKED and answered from the next IRQ |
| `IRIS_EP_OP_PING` | 0xFF01 | liveness; served even while a READ is parked |

Requests carry no payload (`buf_len > 0` → `INVALID_ARG`); unknown opcodes →
`NOT_SUPPORTED`; exactly one reply per request.

> **Historical (retired with KChannel, Phase 13).** The client surface used to
> be `KBD_MSG_HELLO` / `STATUS` / `SUBSCRIBE` with `HELLO_REPLY`,
> `STATUS_REPLY` and `SCANCODE_EVENT` over a shared reply channel, and one
> subscriber at a time that a new subscription silently replaced. `POLL` and
> `READ` answer the same questions with the consumer naming its own reply and
> no shared channel to race on.

## Current invariants

- `kbd` is the sole healthy-path owner of PS/2 port access.
- `kbd` is the only built-in service that currently requires both IRQ routing and I/O port capability delivery.
- `kbd` liveness is part of the global health gate: init and sh both PING it,
  and `[SH] kbd cptr OK` is smoke-gated.
- kbd is the one service written in ring-3 ASSEMBLY, which is why the ABI
  headers it reads (`iris/invoke.h`, `iris/ipc_msg.h`) are assembler-safe: a
  label and a message layout are ABI, and ABI has to be readable by the
  assembler.
