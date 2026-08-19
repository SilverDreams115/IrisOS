# IRIS — Stage 6: memory and the remaining objects come from Untyped

Status: **OPEN — Etapa 1 landed**.
Precondition: Stage 1 (ownership/derivation, closed) and Stage 5 (closed — the
root task holds its Untypeds as described capabilities).
Normative frame: [purity charter](iris-sel4-purity-charter.md) §2.2 O1, §2.5 M1
and M3, §3.3 (no canonical object from kslab); the
[ledger](sel4-convergence-ledger.md) kslab rows and D-1..D-4.

**Closing criterion**: no kernel object and no page of user-visible memory is
created from kernel-private storage.  Every canonical object is retyped from an
Untyped that some holder paid for, and `make check-purity`'s allowlist — which
today lists sixteen `kslab_alloc` consumers — reaches zero for the object
families (the allocator itself may remain for kernel-internal bookkeeping that
is not a capability).

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
| 1 | The Untyped pays for its objects' headers — two-ended carve; KFrame's header leaves the kslab heap | ✅ DONE |
| 2 | Page tables are objects retyped from Untyped; the implicit PMM reserve on map retires | pending |
| 3 | VSpace and its PML4 come from Untyped | pending |
| 4 | The remaining kslab families, one decision each: retype, slot-encode, or retire with the subsystem | pending |
| 5 | KVMO converted or retired; anonymous vs file-backed memory separated in user space | pending |

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
