# IRIS — Stage 6: memory and the remaining objects come from Untyped

Status: **CLOSED — all six Etapas landed**, and then SUPERSEDED IN PART.

This document describes Stage 6's answer to *who pays* for memory: the kernel
creates the object and charges it to an Untyped the caller named.  Stage 6-pure
(see the [roadmap](sel4-convergence-roadmap.md)) replaced that answer for
address spaces with seL4's — the HOLDER retypes the object and hands it over —
so where this doc says a page table, a PML4 or a VSpace header is *charged*,
the kernel no longer creates it at all.  What still reads true here is
everything about VMO pages, mapping records and the KProcess object, and the
reasoning about why a budget must be named at all.

Precondition: Stage 1 (ownership/derivation, closed) and Stage 5 (closed — the
root task holds its Untypeds as described capabilities).
Normative frame: [purity charter](iris-sel4-purity-charter.md) §2.2 O1, §2.5 M1
and M3, §3.3 (no canonical object from kslab); the
[ledger](sel4-convergence-ledger.md) kslab rows and D-1..D-4.

**Closing criterion**: no kernel object and no page of user-visible memory is
created from kernel-private storage *after boot*.  Every allocation a running
process can cause comes out of an Untyped that someone paid for and delegated.

That criterion is met (see "What remains, and where it goes" at the end).  The
literal reading — the allowlist reaches zero — is not, and cannot be inside
this stage: what is left on the kernel slab is either the ROOT TASK, whose
address space is built before any Untyped exists, or an object whose whole
subsystem retires in Stage 7.  Both are named below rather than left as an
unexplained residue.

## What Stage 5 left standing

Stage 5 finished the *authority* story: one namespace, one derivation tree,
fine-grained boot capabilities, and a thread that is a retyped object
configured through capabilities.  What it did not touch is **who pays for
memory**, and the answer is still "the kernel, invisibly", in four places:

| Where | What the kernel allocates behind the caller's back |
|---|---|
| `paging_map_checked_in` | PDPT / PD / PT pages on every map that needs a new level, from the PMM reserve |
| `kframe_alloc` | a `struct KFrame` header per frame, from the kslab heap |
| `kvspace_alloc` + `paging_create_user_space` | the KVSpace object (kslab) and the PML4 page (PMM reserve) |
| `kslab_alloc` × 16 files | KProcess, KVMO, KIrqCap, KIoPort, KBootstrapCap, KInitrdEntry, KUntyped header, the root CNode, `KFrameMapping` nodes |

Charter M3 says "the kernel does not implicitly allocate user memory" and is
marked MET — which is true of *user pages* and false of the *page tables that
map them*.  Stage 6 is where that stops being a distinction the reader has to
make for themselves.

## A question this stage must answer, and Etapa 1 deliberately does not

seL4 has no per-frame kernel object: a Frame capability carries its physical
address and size *in the capability*, and the same is true of IRQHandler and
IOPort caps.  IRIS's model is `CSlot → KObject *`, so every authority needs an
object behind it, which is why `KIrqCap` and `KIoPort` are heap allocations for
what seL4 stores in a slot.

The purest end state is therefore not "retype these objects from an Untyped" —
it is "these authorities stop being objects".  That is a change to the slot
model itself (MDB, rights, badge and lifetime all assume an object pointer),
and it deserves its own analysis rather than being decided by whichever
increment reaches it first.  Etapa 1 makes the *object* path honest without
foreclosing the *no-object* path: nothing it adds becomes harder to delete if
the slot model later absorbs these authorities.

## Etapas

| Etapa | Subject | State |
|---|---|---|
| 6 | Mapping records and device capabilities; the allowlist shrinks | ✅ DONE |
| 1 | The Untyped pays for its objects' headers — two-ended carve; KFrame's header leaves the kslab heap | ✅ DONE |
| 2 | Page tables are charged to an Untyped the address space names; the implicit PMM reserve on map retires | ✅ DONE |
| 3 | VSpace and its PML4 come from Untyped | ✅ DONE |
| 4 | The per-process kernel state and sub-untyped headers move to the budget | ✅ DONE |
| 5 | KVMO CONVERTED: its pages, metadata and header come from a named budget | ✅ DONE |

## Etapa 6 — the last runtime allocations  ✅ DONE

Three paths still reached the kernel slab on every use, not just at boot:

- **Mapping records** (`KFrameMapping`, one per mapped page).  Carved from the
  address space's budget and recycled through a per-VSpace free list, because
  mappings churn and a bump allocator does not rewind: the budget pays once per
  CONCURRENT mapping, not once per map call.  Returned to the Untyped when the
  address space is destroyed.
- **VMO page frame headers** (one per mapped VMO page).  Charged to the VMO's
  own budget — the same one its page came from.  This was the frequent path: a
  process could still grow kernel memory by mapping.
- **Device capabilities** (`KIrqCap`, `KIoPort`).  Claiming an interrupt line
  or a port range fabricates a kernel object; it now comes out of the claimer's
  budget.  seL4 allocates nothing for these at all — an IRQHandler cap has no
  object behind it — which is the deeper divergence the ledger records.
  Charging is what IRIS can do without changing what a capability IS.

**The purity gate caught a mistake here, which is what it is for.**  Routing
mapping records through the VSpace moved a `kslab_alloc` from `kframe.c` to
`kvspace.c`, and `check-purity` refused it: the allowlist may only shrink, and
a use moved is still a use added to that file.  The fix was to remove it rather
than relocate it — the one address space that exists before any budget does is
the root task's, and it maps a handful of pages, so a fixed 64-entry bootstrap
arena serves it with no dynamic allocation at all.  **`scripts/purity_allowlist.txt`
shrank for the first time since Stage 4** (`kframe.c` 2 → 1).

Two host fixtures were re-anchored, not weakened: the ones that map 64 and 128
pages now do it through a budgeted address space, because that is the path that
exists for mapping at scale; and the allocation-failure case injects a budget
too small for a single record instead of a failing slab — asserting exactly
what it asserted before, that a failed record installs no PTE, counts no
mapping and leaves the frame untouched.

## Etapa 5 — user memory comes out of a named budget  ✅ DONE

Anonymous memory was the last thing the kernel handed out for free.  A process
asked for a VMO and got PMM pages, bounded only by a per-process quota the
kernel invented — not by a capability anyone delegated.  A VMO's pages, its
page-address array and its object header now all come from an Untyped.

**Which Untyped is the caller's to say.**  `SYS_VMO_CREATE`'s dead first
argument became the budget CPtr, and `SYS_INITRD_VMO` gained one, because a
process holds several budgets and they are not interchangeable: the small,
recycled pool its address space was built from is not where a service should
put the memory it serves out of.  Zero means "the budget my address space came
from", which is where the memory would have come from anyway.
`SYS_VMO_CREATE_FOR` keeps charging the payer's own budget — that is what
"charged to the payer" already meant.

### Reclamation, and why it needed a design and not a bigger number

A bump allocator never rewinds, so charging memory without a way to get it back
turns every spawn into a permanent cost: the first attempt exhausted an 8 MiB
pool in tens of spawns, and no budget size fixes a monotonic leak.  Two
mechanisms make consumption bounded instead of cumulative, and both are seL4's
answer (revoke the Untyped you used) in the form IRIS has:

- **A recycled budget per live child.**  The loader pairs each process leaf
  with a budget leaf; everything a child costs — address space, process state,
  segment and stack VMOs — is carved from it, so when the child dies and its
  last capability goes, that budget has no children left and `RESET` makes the
  whole region reusable.  Consumption is bounded by the number of children
  ALIVE, not by how many have ever run.  The spawner sizes it, because the
  spawner knows what it is launching.
- **A recycled scratch for image copies.**  `SYS_INITRD_VMO` copies a whole
  boot image; the loader parses it and drops it.  Pointed at a dedicated
  sub-untyped that is RESET between spawns, that transient copy costs one
  image instead of one image per spawn.

Reading a leaf's occupancy before touching its budget is what makes recycling
safe, and `SYS_CAP_IDENTIFY` — added in Stage 4 to ask exactly this question
without taking authority — is what asks it.  Resetting the budget of a child
that is still alive would strand its memory; the first version of this loop did
precisely that, and the runtime said so.

### A defect this etapa exposed

Three-argument syscall stubs did not clear `r10`.  That was harmless while no
syscall read a fourth argument, and became a bug the moment `SYS_INITRD_VMO`
grew a budget: the kernel read whatever the compiler had left there.  The stubs
now zero it explicitly, which closes the whole class rather than this instance.

Covered by **T300**: a VMO created against a named budget consumes that budget
by at least the size asked for, its pages are real and zero-filled, a budget of
the wrong type is refused, the budget cannot be RESET while the VMO lives, and
once the VMO is gone the region is reclaimable.

## Etapa 4 — a process's kernel state comes out of the budget  ✅ DONE

Three more families follow the address space into the Untyped:

| Object | Was | Is |
|---|---|---|
| `KProcess` | kslab | top-carved child block of the spawn budget |
| the child's 256-slot root CNode | kslab (~20 KiB, the largest single per-process allocation) | top-carved child block of the same budget |
| a sub-untyped's `KUntyped` header | kslab | top-carved child block of its PARENT |

The last one closes a small circularity that was easy to miss: **delegating a
budget used to cost kernel memory**.  Carving a sub-untyped took its region
from the parent and its header from the slab, so a service that handed its
children budgets was spending kernel memory to do it.  The header is now a
child block of the parent, and that block carries the `child_count` entry and
the parent retain — `alloc_parent` stays NULL for these, so nothing is
accounted twice.

`kprocess_alloc_from` is where the per-process state is charged, and the ladder
of budgets in init/svcmgr grew to match: a process now costs its spawner about
20 KiB of CNode plus its address space, where before the kernel absorbed it
silently.  That is the point — the cost did not appear, it became visible.

What stays on the kernel slab, and why it is not a leftover to clean up later:

- the **root task's** KProcess, root CNode, KVSpace and PML4 — built before any
  Untyped exists, the same bounded bootstrap exception as its page tables;
- **boot Untypeds** — created by `kernel_main` from raw PMM blocks; they have
  no parent to charge;
- `KFrameMapping` nodes, and the frames of a VMO or a bootstrap map — Etapa 5
  and the paths that retire with KVMO.

## Etapa 3 — the address space itself comes from the budget  ✅ DONE

The two pieces Etapa 2 left kernel-funded are gone for spawned processes: the
**PML4** is a page child of the same Untyped (`paging_create_user_space_from`),
and the **KVSpace header** is a top-carved child block of it
(`kvspace_alloc_at`).  One budget now pays for an entire address space —
header, root table, and every level beneath it — and a `SYS_PROCESS_CREATE`
that names no budget builds nothing.

Teardown order is not arbitrary and is worth stating: return the page children
(PML4 + tables) first, then the header block — which drops the retain the block
itself holds — and only then the VSpace's own pool retain.  Releasing the pool
first could destroy the Untyped whose region the header block lives in, and
that block is what records who the parent was.

A pooled PML4 is also never returned to the PMM, for the same reason a pooled
table is not: the page is inside somebody's Untyped, and handing it to the
buddy allocator would give the same memory out twice.

The root task keeps the kslab/PMM path (`kvspace_alloc`,
`paging_create_user_space`), because its address space is built before any
Untyped exists — the same bounded bootstrap exception as its page tables.

T299 gained a leg: creating the address space alone already consumes at least a
page of the budget, before anything is mapped into it.

## Etapa 2 — page tables are charged to a budget  ✅ DONE

Every level below the PML4 is memory, and mapping user memory took it silently
from the kernel's PMM reserve.  A process could therefore make the kernel spend
memory by mapping at scattered addresses — no budget, no capability, no
accounting — which is precisely what charter M3 says does not happen.

**The budget is a capability, named at creation.**  `SYS_PROCESS_CREATE` takes
the Untyped that will pay for the new address space's page tables; it is
REQUIRED (a spawn without one is refused rather than funded by the kernel), it
needs `RIGHT_WRITE` like any other carve, and the VSpace retains it for as long
as it lives.  `paging_map_checked_in_from` carves levels from it and FAILS when
it cannot — no fallback to kernel memory.

**A page table counts as a child of the Untyped that paid.**  Page tables have
no object header, so nothing would otherwise stop `SYS_UNTYPED_RESET` — which
only refuses while `child_count` is non-zero — from reclaiming a region whose
pages are the live tables of a running address space, handing the same memory
out twice.  `kuntyped_alloc_page_child` takes a child entry per table and the
VSpace returns them all at teardown, so the region becomes reusable exactly
when nothing maps through it any more.

**Who pays, in practice**: the spawner.  A per-child sub-untyped was the first
design and it was wrong — a bump allocator never rewinds, so every spawn
consumed a whole budget whether the child used it or not, and a churn workload
exhausted an 8 MiB pool in tens of spawns.  Charging the real cost (about five
tables per address space) to the pool the spawner already manages is cheaper
and more honest.

**Bootstrap exception**, bounded and stated: the root task's address space is
built before any Untyped exists, so its tables come from the PMM reserve, as do
the kernel's own (kstack region, physmap).  That is kernel memory for kernel
mappings; every other user page table is charged.

### Two defects this etapa exposed, both fixed here

- **Page-aligned carves were aligned in the wrong space.**
  `kuntyped_bump_alloc_phys_page` rounded the OFFSET within the region, which
  only yields a page-aligned address when the region itself starts on a page
  boundary.  Boot Untypeds do; a sub-untyped, carved at 64-byte granularity,
  does not.  So a frame retyped from a sub-untyped got a paddr the mapper
  silently masked DOWN — mapping the page *before* the frame, overlapping
  whatever that sub-untyped had carved earlier.  It had been latent since
  sub-untypeds existed; page tables made it fatal within a second of boot.
  The carve now aligns the absolute physical address.
- **A process created and never started could not be reclaimed.**  Kill found
  no threads and returned success without dropping the creation reference,
  which only the LAST THREAD exiting ever dropped.  The KProcess, its VSpace
  and its PML4 stayed pinned for the life of the system — and now its
  page-table budget with them.  Kill tears down a thread-less process, and
  `kprocess_free` drops the creation reference exactly once so a racing
  thread-exit cannot double-drop it.

Covered by **T299**: a spawn with no budget is refused, a budget of the wrong
type is refused, mapping into a fresh window really consumes page-sized amounts
of it, the budget cannot be RESET while those tables are live, and once the
address space is gone the region is reclaimable and reusable.

## Etapa 1 — the Untyped pays for its objects' headers  ✅ DONE

A frame retyped from an Untyped already carved its **page** from that Untyped;
its `struct KFrame` header came from the kernel's slab heap.  So a caller who
paid for a page of memory was also, invisibly, spending kernel memory that
nothing accounted, nothing bounded, and no capability authorised — the same
shape as the implicit page tables, one level down.

**Headers now come from the Untyped, carved from the opposite end.**
`KUntyped` gained a second bump pointer (`used_top`, growing down from the end
of the region) beside the existing one (`used`, growing up):

```text
 phys_base                                                    phys_base+total
 ├──────────────► used                            used_top ◄──────────────┤
 │ page-aligned regions (frames, sub-untypeds)  │  object headers          │
 │ and object blocks from the batch carve       │  (KUNTYPED_ALIGN + obj)  │
```

The direction matters, and is the whole reason for a second pointer: the
bottom carve rounds up to a page boundary for frames and sub-untypeds, so a
64-byte header taken from the bottom would push the next page carve to the
following page and waste almost 4 KiB per frame.  From the top, header carves
never perturb page alignment.  The two ends meet exactly once — every bounds
check is `used + used_top + need <= total_size` — and `kuntyped_available`
reports what is genuinely left between them.

**A header is never inside the frame it describes.**  "Headers inside the
region" means inside the *Untyped*, never inside the frame's own page: that
page is mapped into ring 3, and kernel bookkeeping placed in it would be
readable and writable by the process the frame was given to.  The two carves
come from opposite ends of the Untyped precisely so they cannot overlap, and
RBI-style unit tests pin it.

**Accounting is unchanged in shape**: one `child_count` entry per frame, held
now by the header block (`kuntyped_alloc_child_top`) instead of by the frame's
`alloc_parent` field, exactly as `KCNode`, `KTcb` and the other retyped
families already work.  Frames that are NOT retyped — the VMO page frames and
the bootstrap frames of a spawning process — keep their kslab header, because
they have no Untyped to charge; they are the memory paths Etapas 3 and 5
retire, and they are what keeps `kframe.c` on the purity allowlist for now.

**Reset**: `SYS_UNTYPED_RESET` clears both ends.  A reset with live children
is still `BUSY`, so the top region cannot be reclaimed out from under an object
that is still alive.

Covered by host unit tests (`test_kuntyped.c`: the two ends meet exactly, a
header carve does not move the page bump, available accounts both, reset clears
both) and runtime **T298** (retyping a frame consumes page + header from one
Untyped, the header is outside the frame's page, and the frame still maps).


## What remains, and where it goes

After Etapa 6, `kslab_alloc` survives in exactly two categories, and neither is
reachable by a running process asking for something:

**Bootstrap — the root task and the objects that predate any budget.**  Its
`KProcess`, root CNode, `KVSpace` and PML4; the boot Untypeds themselves
(created by `kernel_main` from raw PMM blocks, with no parent to charge); the
six boot control capabilities; the initrd catalog entries.  All of it exists
before the first Untyped is published, which is why it cannot be charged to
one.  It is bounded (one address space, a dozen objects), it does not grow with
load, and it is the same class of exception as the idle task's static backing.
It retires when the root task's own creation moves into user space — Stage 7.

**Subsystems that retire whole.**  `KVMO` is CONVERTED (pages, metadata and
header all come from a budget) but still exists as an object: a kernel-side
memory abstraction with an owner and a quota, where seL4 has only Frames.  Its
retirement is the memory server.  `KInitrdEntry` and `KBootstrapCap` likewise
belong to subsystems — a kernel-resident boot image store, a kernel-published
boot authority — that Stage 7 moves or removes.

**What Stage 6 did NOT do, stated plainly.**  IRIS charges these objects to a
budget; seL4 has the *user* retype them explicitly and, for frames, IRQ handlers
and I/O ports, has no kernel object at all.  Charging is the honest halfway
point available while a VSpace is still composed by the kernel inside process
creation (ledger D-5).  The remaining step — the user retypes a PageTable and
maps it, the user retypes a Frame — is Stage 7's, because it needs a spawner
that can name its child's CSpace and VSpace.
