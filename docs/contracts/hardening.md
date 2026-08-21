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

Current delivery path (Stage 5 — one capability, one authority):

- kernel -> root task's CSpace at spawn time, one capability per slot
- rights on each: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`
- described in the BootInfo region the root task reads and validates

Current use:

- `IRIS_BOOTCAP_PROC_CONTROL`: authorized `SYS_PROCESS_CREATE`, which is retired (Stage 7-proc). It now authorizes nothing — a child is a TCB, a CNode and a VSpace retyped from a budget the spawner holds, and holding that budget is the authority
- `IRIS_BOOTCAP_INITRD_CONTROL`: authorizes `SYS_INITRD_COUNT` and `SYS_INITRD_VMO`
- `IRIS_BOOTCAP_IRQ_CONTROL`: authorizes `SYS_CAP_CREATE_IRQCAP`
- `IRIS_BOOTCAP_IOPORT_CONTROL`: authorizes `SYS_CAP_CREATE_IOPORT`
- `IRIS_BOOTCAP_DEBUG_CONTROL`: authorizes `SYS_POWEROFF`, `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO`
- `IRIS_BOOTCAP_FB_CONTROL`: authorizes `SYS_FRAMEBUFFER_VMO` (one-shot; the kernel clears the flag after first claim)

Each is matched by EXACT equality, so holding one says nothing about any other,
and a capability carrying two authorities cannot be constructed.  Giving one up
is deleting the slot that holds it — `SYS_BOOTCAP_RESTRICT` is retired, and
svcmgr drops its two device control capabilities that way once the catalog's
hardware is claimed.  None of them grants general process-management authority.

### `KIoPort`

Current healthy-path delivery:

- kernel -> first user task via `KBootstrapCap` authority
- first user task -> `svcmgr` by handle transfer
- `svcmgr` -> child service bootstrap channel

Current rights:

- kernel-to-`svcmgr`: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`
- `svcmgr`-to-child: reduced to `RIGHT_READ`

Current consequence:

- `kbd` may execute `SYS_IOPORT_IN`
- `kbd` may not execute `SYS_IOPORT_OUT` under the currently delivered child rights

This is a real current invariant of the code. If `kbd` ever needs output port writes on the healthy path, the delivered rights contract must change deliberately.

## Residual risks still present

- The model is intentionally internal and not yet a stable external ABI.
- There are few dedicated host-side tests for rights reduction and transfer edge cases.
- Runtime validation of capability misuse still depends mainly on boot/runtime paths rather than isolated unit tests.
