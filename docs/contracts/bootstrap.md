# Bootstrap contract

## Purpose

Defines how the kernel, `svcmgr`, and child services exchange bootstrap authority and initial handles.

## Root bootstrap model

The current healthy-path bootstrap is two-tier:

1. Kernel bootstraps `svcmgr`.
2. Kernel attaches a narrow client handle to the first user task (`init`).
3. `svcmgr` bootstraps the remaining built-in services from the service catalog.

This means normal runtime discovery is userland-owned, but the first control path is still kernel-seeded.

## `svcmgr` bootstrap channel contract

The kernel allocates one private `KChannel` for `svcmgr` and inserts it into `svcmgr` with:

- rights: `RIGHT_READ | RIGHT_WRITE`
- delivery: `task_set_bootstrap_arg0(sm, h)`

The kernel retains its own reference to the same channel for:

- initial client attachment
- bootstrap capability delivery
- lifecycle watch traffic

## Kernel-to-`svcmgr` bootstrap payloads

Current mandatory bootstrap message:

- `SVCMGR_MSG_BOOTSTRAP_HANDLE`
  - `kind = SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP`
  - attached handle rights: `RIGHT_READ`

Current optional/best-effort bootstrap messages:

- `SVCMGR_BOOTSTRAP_KIND_IRQ_CAP`
  - one per catalog entry with `irq_num != 0xFF`
  - attached handle rights: `RIGHT_ROUTE`
- `SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP`
  - one per catalog entry with `ioport_count > 0`
  - attached handle rights: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`

`svcmgr` requires the spawn capability to proceed. Missing IRQ or I/O port capabilities degrade service bootstrap for the affected service.

## First-client attachment contract

The kernel may attach a client-visible handle to the retained `svcmgr` bootstrap channel into another process via `svcmgr_bootstrap_attach_client()`.

Current healthy path uses:

- target process: `init`
- granted rights: `RIGHT_WRITE`

The attach helper rejects:

- zero rights
- any rights outside `RIGHT_WRITE`

This is intentionally narrow: the first client can talk to `svcmgr`, but it is not granted broader authority over the bootstrap channel.

## `SYS_SPAWN_SERVICE` contract

`SYS_SPAWN_SERVICE` is restricted by an explicit bootstrap capability handle.

The caller must present:

- object type: `KOBJ_BOOTSTRAP_CAP`
- handle right: `RIGHT_READ`
- capability permission: `IRIS_BOOTCAP_SPAWN_SERVICE`

On success:

- the named ELF is loaded from the kernel initrd
- a fresh child process is created
- a new bootstrap channel pair is allocated
- the child receives its bootstrap handle in `RBX`
- the parent receives:
  - a `KProcess` handle with `RIGHT_READ | RIGHT_ROUTE | RIGHT_MANAGE | RIGHT_DUPLICATE`
  - optionally, a parent-side bootstrap channel handle if requested

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

## Current bootstrap invariants

- `svcmgr` is the only healthy-path caller of `SYS_SPAWN_SERVICE`.
- The kernel does not publish `svcmgr` through the old nameserver path for healthy boot.
- Child bootstrap handles are private and are not reused as normal runtime service handles.
- The service catalog is the source of truth for built-in service policy.
