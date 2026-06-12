# `kbd` contract

## Purpose

Defines the current keyboard service contract for liveness, status, scancode forwarding, and hardware I/O ownership.

## Ownership

`kbd` owns:

- PS/2 controller I/O in userland via `KIoPort`
- client-visible keyboard liveness and status replies
- forwarding of raw scancode events to one subscriber channel

The kernel still owns:

- low-level IRQ reception
- routing of IRQ events into the service channel

## Bootstrap contract

`kbd` is spawned by `svcmgr` and receives in `RBX`:

- its private bootstrap channel

From `svcmgr`, `kbd` expects these bootstrap deliveries:

- public service/IRQ channel handle
- reply channel handle
- `KIoPort` capability handle when hardware I/O is required

If bootstrap delivery is incomplete, `kbd` fails its startup path.

## IRQ-facing contract (Fase 7.6: KNotification)

Keyboard IRQ delivery uses the generic IRQ routing layer with a
**KNotification** destination (catalog flag `irq_notify = 1`):

- svcmgr owns the notification master (kept across restarts) and registers
  it as the IRQ1 route; the WAIT side reaches `kbd` at bootstrap as
  `SVCMGR_BOOTSTRAP_KIND_IRQ_NOTIFY` (0x23) — mandatory for startup
- on each IRQ the kernel signals bit `1 << irq` (signal-only; no message)
- `kbd` wakes from `SYS_NOTIFY_WAIT_TIMEOUT`, reads the scancode byte from
  port 0x60 via its `KIoPort` cap and re-arms with `SYS_IRQ_ACK`

> **Historical (retired in Fase 7.6):** IRQ1 used to be routed to the kbd
> public service channel as `KBD_MSG_IRQ_SCANCODE` messages. That delivery
> path is no longer dispatched; the opcode remains defined only as a
> historical constant.

Key events reach consumers over the `"kbd.ep"` KEndpoint
(`KBD_EP_OP_POLL` / `KBD_EP_OP_READ` with parked KReply — see
`docs/kbd-endpoint.md`); the legacy channel below carries only probes.

## Client request/response surface

Current client requests:

- `KBD_MSG_HELLO`
- `KBD_MSG_STATUS`
- `KBD_MSG_SUBSCRIBE`

Current replies/events:

- `KBD_MSG_HELLO_REPLY`
- `KBD_MSG_STATUS_REPLY`
- `KBD_MSG_SCANCODE_EVENT`

## `HELLO` contract

`KBD_MSG_HELLO` is a zero-payload liveness probe.

Healthy path expectation:

- `init` must receive `KBD_MSG_HELLO_REPLY`
- reply error code must be `0`

## `STATUS` contract

`KBD_MSG_STATUS` is a zero-payload status query.

Current healthy-path status requirement:

- returned flags must equal `KBD_STATUS_NORMAL`

That means both are set:

- `KBD_STATUS_READY`
- `KBD_STATUS_PS2_OK`

If a one-shot reply handle is attached, it must grant `RIGHT_WRITE`.

## `SUBSCRIBE` contract

`KBD_MSG_SUBSCRIBE` attaches one writable subscriber channel.

Current semantics:

- only one subscriber is active at a time
- a new subscription replaces the previous one
- subsequent key events are forwarded as `KBD_MSG_SCANCODE_EVENT`
- fire-and-forget: no reply is emitted on the shared `kbd.reply` channel

Healthy path currently lets `init` validate the path first; `sh` may also subscribe
without racing on shared replies because `SUBSCRIBE` no longer produces one.

## Current invariants

- `kbd` is the sole healthy-path owner of PS/2 port access.
- `kbd` is the only built-in service that currently requires both IRQ routing and I/O port capability delivery.
- `kbd` status is part of the global health gate checked by `init` and aggregated by `svcmgr`.
