# Bootstrap contract

## Purpose

Defines how the kernel, `init`, `svcmgr`, and child services exchange
bootstrap authority in the healthy path. Every grant below is a CSpace slot:
Stage 4 deleted the handle table, and Phase 13 the bootstrap channel, so a
child's authority is what its root CNode holds when it starts.

## Root bootstrap model

The current healthy-path bootstrap is four-stage:

1. The kernel spawns one minimal ring-3 bootstrap task from the dedicated linked `userboot` image slice.
2. The kernel publishes the root task's capabilities into its CSpace — six boot control capabilities, its own root CNode, its own thread, its VSpace and the boot Untypeds — and describes all of them in a **BootInfo** region mapped read-only into the task (address in RBX).
3. `userboot` validates that description against its own CSpace, resolves `init` from the embedded initrd with `Boot_InitrdFrame` and starts it via the ring-3 loader path.
4. `init` spawns `svcmgr`; `svcmgr` then bootstraps the remaining built-in services from the service catalog.

This keeps normal service image loading and topology in userland while leaving only one minimal kernel-seeded root task.

## Kernel bootstrap authority contract

Stage 5: **one capability, one authority.**  The kernel publishes six boot
capabilities into the root task's CSpace, each carrying exactly one authority
and each matched by exact equality — a capability that merely contains an
authority cannot be constructed:

| CPtr | Capability | Authorises |
|---|---|---|
| `BOOT_CPTR_IRQ_CONTROL` (3) | IRQ control | `Boot_CreateIRQCap` |
| `BOOT_CPTR_IOPORT_CONTROL` (4) | ioport control | `Boot_CreateIOPort`, `Boot_IOPortNarrow` |
| `BOOT_CPTR_DEBUG_CONTROL` (5) | debug control | `Boot_KlogDrain`, `Boot_SchedInfo`, `Boot_Poweroff` |
| `BOOT_CPTR_PROC_CONTROL` (6) | process control | **nothing, since Stage 7-proc**: it authorised `SYS_PROCESS_CREATE`, which is retired.  A child is a TCB, a CNode and a VSpace retyped from a budget the spawner holds, and holding that budget IS the authority — seL4 has no spawn capability either.  The slot is still minted and still passed around; the number it guarded is gone (A-32 left three), so what remains is a slot nobody consults |
| `BOOT_CPTR_INITRD_CONTROL` (7) | initrd | `Boot_InitrdCount`, `Boot_InitrdFrame` |
| `BOOT_CPTR_FB_CONTROL` (8) | framebuffer | `Boot_FramebufferInfo` (one-shot) |

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
composable spawn primitives rooted in `Boot_InitrdFrame`.

`userboot` delegates to `init` exactly the capabilities it names — as CPtr
sources, so each grant is an MDB child of userboot's slot and stays revocable —
and parks.  That keeps the root bootstrap task inert on the healthy path after
handoff.

## Ring-3 child spawn contract

A spawn needs the **initrd** capability (`Boot_InitrdCount` /
`Boot_InitrdFrame`) to read a boot image, and an **Untyped** to build the
child out of.  That is
all: holding the budget is the authority to retype, exactly as in seL4.

It used to need a second capability, process control, and before Stage 5 both
were one permission bit — which is why `vfs`, a file server that only reads
boot images, used to hold the authority to create processes.  Stage 5 split
them; Stage 7-proc removed the need for the second one.  `svc_load_minted_ws`
still takes it as an argument and ignores it, recorded rather than hidden.

On success:

- `Boot_InitrdFrame` resolves a named initrd catalog entry to a read-only
  FRAME carved from the caller's budget, and ANSWERS ITS SIZE in the same call
  (ledger D-5) — a caller that has to ask how big the thing it was just given
  is has been given two things
- userland parses and relocates the ELF image; one `Frame_Map` covers the
  whole frame (ledger D-10)
- the parent RETYPES the child's address space and root CSpace out of the
  child's budget (`Untyped_Retype` of `IRIS_KOBJ_VSPACE` and
  `IRIS_KOBJ_CNODE`) — the kernel builds neither (Stage 6-pure)
- prepared segments are mapped into that address space with `Frame_Map`, and
  the parent supplies any paging level the map reports missing
  (`IRIS_ERR_MISSING_TABLE` → `PageTable_Map`)
- the first thread is composed the same way any thread is: retype an
  `IRIS_KOBJ_TCB`, `TCB_Configure` it with the child's CSpace and VSpace,
  `TCB_WriteRegs`, `TCB_Resume`.  `SYS_THREAD_START` and
  `SYS_PROCESS_CREATE` are both RETIRED (Stage 7)
- every capability the child starts with is a pre-start `CSpace_Mint` with the
  child's root CNode as the destination — the parent has it because it retyped
  it — sourced from the parent's own slots so the delegation stays revocable
- what the spawn hands back is the child's **first thread**.  A supervisor
  watches it (`TCB_Watch`), kills it (`TCB_Exit`) and reads its exit code
  (`TCB_ExitCode`); there is no process capability
- RBX carries 0: the child's authority is in its CSpace, not in a register

## `svcmgr` child bootstrap contract

There is no bootstrap MESSAGE any more. For each autostarted service `svcmgr`
retypes what the catalog declares and **pre-start-mints** it into the child's
root CNode, which svcmgr can do because it retyped that CNode:

| Slot | Capability | Given to |
|---|---|---|
| 1–4 | the core service endpoints (svcmgr, vfs, console, kbd) | every catalog child |
| `IRIS_CPTR_OWN_EP` (5) | the RECEIVE side of the child's own endpoint, `RIGHT_READ` | services with `own_service_ep` |
| `IRIS_CPTR_INITRD_CONTROL` (8) | initrd read authority | vfs |
| `IRIS_CPTR_IRQ_NOTIFY` (7) | the WAIT side of its IRQ notification | services with `irq_notify` |
| `IRIS_CPTR_IOPORT` (10) | a narrowed `KIoPort` | services with hardware I/O |
| 13 / 14 | reply objects, retyped from svcmgr's pool (Phase S1) | serving children |
| `IRIS_CPTR_OWN_UNTYPED` (12) | the child's memory budget | every child that retypes |
| `IRIS_CPTR_OWN_TCB` (19) | the child's own first thread | children that name themselves |

The SEND side of each service endpoint stays with svcmgr, which is what keeps
client capabilities valid across a restart, and is published under the
reserved `"<name>.ep"` name. Rights are the catalog's: a child gets `READ` on
what it serves and `WRITE` on what it calls.

## Lifecycle ownership contract

After spawning a child service, `svcmgr` does two things:

1. Registers IRQ ownership, if the manifest requires one:
   - `IRQ_SetNotification(irqcap, notification)` — the route's owner is the
     **notification it is bound to** (Stage 7-mem), not a process
2. Arms one exit watch on the child's **first thread**:
   - `TCB_Watch(tcb, notification, service_id)`

The lifecycle consequence is:

- exit notifications are delivered back to `svcmgr` as a signal on the
  notification it named
- IRQ route cleanup remains kernel-side and is tied to the lifetime of that
  notification

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
- `userboot` does not retain bootstrap authority after handoff; it delegates
  what it names as MDB children of its own slots, so every grant stays
  revocable, and parks
- a child's starting authority is exactly its root CNode: there is no register,
  no message and no second namespace carrying any of it
- the service catalog is the source of truth for built-in service policy
- narrowing one capability must not silently reduce authority held through
  another: a holder that wants less DELETES the slot holding what it no longer
  needs, which is why `SYS_BOOTCAP_RESTRICT` was retired rather than kept
