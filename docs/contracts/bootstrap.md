# Bootstrap contract

## Purpose

Defines how the kernel, `init`, `svcmgr`, and child services exchange bootstrap authority and initial handles in the healthy path.

## Root bootstrap model

The current healthy-path bootstrap is four-stage:

1. The kernel spawns one minimal ring-3 bootstrap task from the dedicated linked `userboot` image slice.
2. The kernel publishes the root task's capabilities into its CSpace — six boot control capabilities, its own root CNode, its own thread, its VSpace and the boot Untypeds — and describes all of them in a **BootInfo** region mapped read-only into the task (address in RBX).
3. `userboot` validates that description against its own CSpace, resolves `init` from the embedded initrd with `SYS_INITRD_VMO` and starts it via the ring-3 loader path.
4. `init` spawns `svcmgr`; `svcmgr` then bootstraps the remaining built-in services from the service catalog.

This keeps normal service image loading and topology in userland while leaving only one minimal kernel-seeded root task.

## Kernel bootstrap authority contract

Stage 5: **one capability, one authority.**  The kernel publishes six boot
capabilities into the root task's CSpace, each carrying exactly one authority
and each matched by exact equality — a capability that merely contains an
authority cannot be constructed:

| CPtr | Capability | Authorises |
|---|---|---|
| `BOOT_CPTR_IRQ_CONTROL` (3) | IRQ control | `SYS_CAP_CREATE_IRQCAP` |
| `BOOT_CPTR_IOPORT_CONTROL` (4) | ioport control | `SYS_CAP_CREATE_IOPORT` |
| `BOOT_CPTR_DEBUG_CONTROL` (5) | debug control | `SYS_KLOG_DRAIN`, `SYS_SCHED_INFO`, `SYS_POWEROFF` |
| `BOOT_CPTR_PROC_CONTROL` (6) | process control | `SYS_PROCESS_CREATE` |
| `BOOT_CPTR_INITRD_CONTROL` (7) | initrd | `SYS_INITRD_COUNT`, `SYS_INITRD_VMO` |
| `BOOT_CPTR_FB_CONTROL` (8) | framebuffer | `SYS_FRAMEBUFFER_VMO` (one-shot) |

Each is minted with `RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER`.  Slot 1
(`BOOT_CPTR_BOOTSTRAP_CAP`) held the monolithic predecessor and is now
permanently reserved and empty; `SYS_BOOTCAP_RESTRICT` is retired, because a
holder that wants less deletes the slot holding what it no longer needs.

The root task also holds capabilities to its own root CNode
(`BOOT_CPTR_CNODE`), its initial thread (`BOOT_CPTR_TCB`) and its VSpace
(`BOOT_CPTR_VSPACE`), plus one KUntyped per drained memory block.  All of it is
described in the BootInfo region (`<iris/root_bootinfo.h>`), which the root
task validates before delegating anything: a description that disagrees with
the CSpace it describes halts the boot with a serial diagnostic.

This is the only healthy-path bootstrap authority retained by the kernel.
After that point, child image selection is userland-driven through the
composable spawn primitives rooted in `SYS_INITRD_VMO`.

`userboot` delegates to `init` exactly the capabilities it names — as CPtr
sources, so each grant is an MDB child of userboot's slot and stays revocable —
and parks.  That keeps the root bootstrap task inert on the healthy path after
handoff.

## Ring-3 child spawn contract

A spawn needs TWO authorities and presents them as two capabilities: the
process control capability (`SYS_PROCESS_CREATE`) and the initrd capability
(`SYS_INITRD_COUNT`/`SYS_INITRD_VMO`).  They were one permission bit until
Stage 5, which is why `vfs` — a file server that only reads boot images — used
to hold the authority to create processes.

On success:

- `SYS_INITRD_VMO` resolves a named initrd catalog entry to a read-only ELF VMO
- userland parses and relocates the ELF image
- the parent RETYPES the child's address space and root CSpace out of the
  child's budget (`SYS_UNTYPED_RETYPE2` of `IRIS_KOBJ_VSPACE` and
  `IRIS_KOBJ_CNODE`) and `SYS_PROCESS_CREATE` composes a process from them —
  the kernel builds neither (Stage 6-pure)
- `SYS_VMO_MAP_INTO` maps prepared segments into that process, and the parent
  supplies any paging level the map reports missing
  (`IRIS_ERR_MISSING_TABLE` → `SYS_VSPACE_MAP_TABLE`)
- the first thread is composed the same way any thread is: retype an
  `IRIS_KOBJ_TCB`, `SYS_TCB_CONFIGURE` it with the child's CSpace and VSpace,
  `SYS_TCB_WRITE_REGS`, `SYS_TCB_RESUME`.  `SYS_THREAD_START` is RETIRED
  (Stage 7)
- every capability the child starts with is a pre-start CSpace mint
  (`SYS_PROC_CSPACE_MINT` / `SYS_CSPACE_MINT_INTO`), sourced from the parent's
  own slots so the delegation stays revocable
- RBX carries 0: the child's authority is in its CSpace, not in a register
- the parent retains the process capability it created

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
