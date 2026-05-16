# Bootstrap contract

## Purpose

Defines how the kernel, `init`, `svcmgr`, and child services exchange bootstrap authority and initial handles in the healthy path.

## Root bootstrap model

The current healthy-path bootstrap is four-stage:

1. The kernel spawns one minimal ring-3 bootstrap task from the dedicated linked `userboot` image slice.
2. The kernel injects one `KBootstrapCap` into that first user task.
3. `userboot` resolves `init` from the embedded initrd with `SYS_INITRD_VMO` and starts it via the ring-3 loader path.
4. `init` spawns `svcmgr`; `svcmgr` then bootstraps the remaining built-in services from the service catalog.

This keeps normal service image loading and topology in userland while leaving only one minimal kernel-seeded root task.

## Kernel bootstrap authority contract

The kernel injects one `KBootstrapCap` into the first user task (`userboot`) with:

- handle rights: `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`
- capability permissions: `IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_HW_ACCESS | IRIS_BOOTCAP_KDEBUG | IRIS_BOOTCAP_FRAMEBUFFER`

This is the only healthy-path bootstrap authority retained by the kernel.
After that point, child image selection is userland-driven through the composable
spawn primitives rooted in `SYS_INITRD_VMO`.

`userboot` uses that capability only long enough to load `init`, forwards the same
bootstrap authority to `init`, closes its own handles, and parks. That keeps the
root bootstrap task inert on the healthy path after handoff.

## Ring-3 child spawn contract

Service spawn is restricted by an explicit bootstrap capability handle.

The caller must present:

- object type: `KOBJ_BOOTSTRAP_CAP`
- handle right: `RIGHT_READ`
- capability permission: `IRIS_BOOTCAP_SPAWN_SERVICE`

On success:

- `SYS_INITRD_VMO` resolves a named initrd catalog entry to a read-only ELF VMO
- userland parses and relocates the ELF image
- `SYS_PROCESS_CREATE` creates a fresh child process
- `SYS_VMO_MAP_INTO` maps prepared segments into that process
- `SYS_THREAD_START` starts the first thread
- `SYS_HANDLE_INSERT` transfers bootstrap objects into the child handle table
- the child receives its bootstrap handle in `RBX`
- the parent retains the process handle and any bootstrap channels it created

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
- `sh`
  - receives console, kbd service, and vfs service/reply handles as explicit bootstrap gifts

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

- linked `userboot` image mapping and first-task creation
  - rationale: the kernel still seeds one root ring-3 task directly
  - implication: IRIS is closer to a pure microkernel, but the very first task still depends on a kernel-owned bootstrap path
- `irq_routing.c`
  - rationale: interrupt delivery, masking, ISR-context dispatch, and owner-tied teardown still need a small kernel-resident mechanism
  - phase-4 decision: stays in kernel for pragmatism; policy for who owns which IRQ remains in `svcmgr`

## Current bootstrap invariants

- the kernel healthy path seeds one fixed `userboot` root task and does not select `svcmgr`, `kbd`, or `vfs` by name
- `init` is now loaded by `userboot` through the ring-3 loader path, not by `kernel_main.c`
- `userboot` does not retain bootstrap authority after handoff; it parks with no live handles
- child bootstrap handles are private and are not reused as normal runtime service handles
- the service catalog is the source of truth for built-in service policy
- bootstrap-cap restriction is per-handle; narrowing one alias must not silently reduce authority held through another alias
