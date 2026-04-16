# Hardening invariants

## Purpose

Documents the current security and lifecycle invariants around handle rights, transfers, IRQ ownership, and bootstrap capability delivery.

This is an audit summary of the current code paths, not a theoretical design note.

## Handle rights invariants

Rights model source:

- `kernel/new_core/include/iris/nc/rights.h`
- `kernel/core/syscall/syscall.c`
- `kernel/new_core/src/handle_table.c`

Current enforced invariants:

- rights can only be reduced, never elevated
- `RIGHT_SAME_RIGHTS` is an operation flag and is never stored in a live handle entry
- closing a handle requires no rights
- stale handle ids are rejected by generation mismatch
- live handle ids are never generation 0
- `handle_table_get_object()` returns a retained object reference

Current syscall-level hardening:

- `SYS_HANDLE_DUP` requires `RIGHT_DUPLICATE`
- `SYS_HANDLE_TRANSFER` requires `RIGHT_TRANSFER` on the source handle
- `SYS_HANDLE_TRANSFER` requires `RIGHT_MANAGE` on the destination process handle
- `SYS_CHAN_SEND` requires `RIGHT_TRANSFER` on any attached handle
- reduced rights collapsing to `RIGHT_NONE` are rejected by:
  - `SYS_CHAN_SEND`
  - `SYS_HANDLE_DUP`
  - `SYS_HANDLE_TRANSFER`

## Transfer and cleanup invariants

Handle movement paths today:

- duplication:
  - object retained into a new slot
  - source handle remains valid
- transfer:
  - destination handle inserted first
  - source handle closed only after destination insert succeeds
  - transfer is move semantics, not copy semantics
- channel-attached transfer:
  - sender must hold `RIGHT_TRANSFER`
  - queued rights are reduced before enqueue
  - receiver-side install happens into the destination process handle table on receive

Cleanup invariants:

- `handle_entry_reset()` drops both active and regular object references
- `handle_table_close_all()` is the bulk process-local cleanup path
- `kprocess_teardown()` closes the process handle table before final process destruction
- `kchannel_destroy()` releases all queued attached objects still buffered in the channel

## IRQ routing ownership invariants

Ownership source:

- `kernel/core/irq/irq_routing.c`
- `kernel/core/syscall/syscall.c`
- `kernel/new_core/src/kprocess.c`

Current enforced invariants:

- IRQ routes are capability-gated by `KIrqCap`
- `SYS_IRQ_ROUTE_REGISTER` requires:
  - `irqcap_handle`: `KOBJ_IRQ_CAP` with `RIGHT_ROUTE`
  - `chan_handle`: `KOBJ_CHANNEL` with `RIGHT_READ | RIGHT_WRITE`
  - `proc_handle`: `KOBJ_PROCESS` with `RIGHT_READ | RIGHT_ROUTE`
- callers do not choose an arbitrary IRQ number; it comes from the `KIrqCap`
- the route owner is the target `KProcess`, not the calling supervisor
- `kprocess_teardown()` always calls `irq_routing_unregister_owner(owner)`
- route cleanup is therefore process-scoped and automatic on exit

Operational consequence:

- `svcmgr` can supervise and restart a service without owning the IRQ route lifetime itself
- replacing a service requires re-registering the route for the new process instance

## Critical capability delivery invariants

### `KBootstrapCap`

Current delivery path:

- kernel -> `svcmgr` private bootstrap channel
- attached rights: `RIGHT_READ`
- permission bit: `IRIS_BOOTCAP_SPAWN_SERVICE`

Current use:

- only authorizes `SYS_SPAWN_SERVICE`
- does not grant general process-management authority

### `KIoPort`

Current healthy-path delivery:

- kernel -> `svcmgr` bootstrap channel
- `svcmgr` -> child service bootstrap channel

Current rights:

- kernel-to-`svcmgr`: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`
- `svcmgr`-to-child: reduced to `RIGHT_READ`

Current consequence:

- `kbd` may execute `SYS_IOPORT_IN`
- `kbd` may not execute `SYS_IOPORT_OUT` under the currently delivered child rights

This is a real current invariant of the code. If `kbd` ever needs output port writes on the healthy path, the delivered rights contract must change deliberately.

### First-client bootstrap handle to `svcmgr`

Current delivery path:

- kernel inserts the retained `svcmgr` bootstrap channel into `init`

Current rights:

- only `RIGHT_WRITE`

Current consequence:

- `init` can send requests into `svcmgr`
- `init` cannot read from or manage the bootstrap channel directly

## Residual risks still present

- The model is intentionally internal and not yet a stable external ABI.
- There are few dedicated host-side tests for rights reduction and transfer edge cases.
- Runtime validation of capability misuse still depends mainly on boot/runtime paths rather than isolated unit tests.
