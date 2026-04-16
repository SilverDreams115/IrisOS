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

## IRQ-facing contract

Keyboard IRQ delivery uses the generic IRQ routing layer:

- IRQ1 is routed to the `kbd` public service channel
- routed hardware events arrive as `KBD_MSG_IRQ_SCANCODE`
- payload:
  - one raw PS/2 scancode byte

The message opcode is shared with the generic routing layer value and must remain in sync with `IRQ_MSG_TYPE_SIGNAL`.

## Client request/response surface

Current client requests:

- `KBD_MSG_HELLO`
- `KBD_MSG_STATUS`
- `KBD_MSG_SUBSCRIBE`

Current replies/events:

- `KBD_MSG_HELLO_REPLY`
- `KBD_MSG_STATUS_REPLY`
- `KBD_MSG_SUBSCRIBE_REPLY`
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

Healthy path currently uses `init` as the subscriber.

## Current invariants

- `kbd` is the sole healthy-path owner of PS/2 port access.
- `kbd` is the only built-in service that currently requires both IRQ routing and I/O port capability delivery.
- `kbd` status is part of the global health gate checked by `init` and aggregated by `svcmgr`.
