# Bootstrap contract

## Purpose

Defines how the kernel, `init`, `svcmgr`, and child services exchange bootstrap authority and initial handles in the healthy path.

## Root bootstrap model

The current healthy-path bootstrap is three-stage:

1. The kernel spawns one opaque bootstrap image from the embedded initrd catalog.
2. The kernel injects one `KBootstrapCap` into that first user task.
3. In the current healthy path, that task is `init`, which spawns `svcmgr`; `svcmgr` then bootstraps the remaining built-in services from the service catalog.

This keeps runtime discovery and service topology in userland while leaving one minimal kernel-seeded entry point.

## Kernel bootstrap authority contract

The kernel injects one `KBootstrapCap` into the first user task with:

- handle rights: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`
- capability permissions: `IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_HW_ACCESS | IRIS_BOOTCAP_KDEBUG`

This is the only healthy-path service/bootstrap policy retained by the kernel.
Image selection after that point is fully userland-driven through `SYS_INITRD_LOOKUP`
and `SYS_SPAWN_ELF`.

## `SYS_INITRD_LOOKUP` + `SYS_SPAWN_ELF` contract

Service spawn is restricted by an explicit bootstrap capability handle.

The caller must present:

- object type: `KOBJ_BOOTSTRAP_CAP`
- handle right: `RIGHT_READ`
- capability permission: `IRIS_BOOTCAP_SPAWN_SERVICE`

On success:

- `SYS_INITRD_LOOKUP` resolves a named initrd catalog entry to `KOBJ_INITRD_ENTRY`
- `SYS_SPAWN_ELF` loads that ELF in kernel-space and creates a fresh child process
- the child receives its bootstrap handle in `RBX`
- the parent receives a `KProcess` handle and, optionally, a bootstrap channel handle

## `svcmgr` child bootstrap contract

For each autostarted service, `svcmgr` currently creates:

- one public service channel
- one reply channel

Then `svcmgr` sends `SVCMGR_MSG_BOOTSTRAP_HANDLE` messages over the child bootstrap channel to deliver:

- public service endpoint
- reply endpoint
- optional `KIoPort` capability for services that require hardware I/O

Child rights come from the declarative service catalog:

- `kbd`
  - service handle: `RIGHT_READ`
  - reply handle: `RIGHT_WRITE`
- `vfs`
  - service handle: `RIGHT_READ | RIGHT_WRITE`
  - reply handle: `RIGHT_WRITE`

## Lifecycle ownership contract

After spawning a child service, `svcmgr` does two things:

1. Registers IRQ ownership, if the manifest requires one:
   - `SYS_IRQ_ROUTE_REGISTER(irqcap_h, public_h, proc_h)`
2. Arms one process exit watch:
   - `SYS_PROCESS_WATCH(proc_h, state->bootstrap_h, service_id)`

The lifecycle consequence is:

- exit notifications are delivered back to `svcmgr` over its bootstrap channel
- IRQ route cleanup remains kernel-side and is tied to child process ownership

## Pragmatic kernel-side mechanisms

The following mechanisms remain in the kernel in the current architecture:

- `elf_loader.c`
  - rationale: process creation still requires trusted ELF validation, page-table construction, and segment mapping in the same failure domain as `task_spawn_elf()`
  - phase-4 decision: stays in kernel for pragmatism; not extracted in this phase
- `irq_routing.c`
  - rationale: interrupt delivery, masking, ISR-context dispatch, and owner-tied teardown still need a small kernel-resident mechanism
  - phase-4 decision: stays in kernel for pragmatism; policy for who owns which IRQ remains in `svcmgr`

## Current bootstrap invariants

- the kernel healthy path consumes one opaque bootstrap image and does not select `svcmgr`, `kbd`, or `vfs` by name
- `init` is the current bootstrap image, but that is an initrd catalog decision, not a `kernel_main.c` string dependency
- child bootstrap handles are private and are not reused as normal runtime service handles
- the service catalog is the source of truth for built-in service policy
- bootstrap-cap restriction is per-handle; narrowing one alias must not silently reduce authority held through another alias
