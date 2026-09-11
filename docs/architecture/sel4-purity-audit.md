# How close IRIS is to seL4 — a file-by-file audit

**Date**: 2026-09-11. **Second pass** — the first read the tree by layer;
this one read all 58 kernel `.c`/`.S` files individually, plus the headers,
and it found things the layer view could not.

**Method**: every kernel file against what seL4 has in the same place, plus
the measurements the tree takes of itself. Where the answer is a number, the
number is the tree's, not an estimate.

**Gates**: 306/306 runtime tests, 27415 host assertions, `check_purity` OK
with the kernel-memory-reachable closure at 23 functions and **zero ring-3
exemptions**.

> **What the second pass changed.** One real defect (charter A9, fixed and
> regression-tested), one gap in the enforcement itself (the purity gate
> followed one of the kernel's two allocators, and its comment stripper hid
> most call sites from it), and roughly 700 lines of scaffolding that named
> mechanisms already removed. The kernel lost two whole allocators, a virtual
> address-space reservation, two object types, a device name, and every
> user-pointer read. Details in Part 6.

---

## The short answer

On the four things the charter is actually about — **authority, objects, IPC
and the mechanism/policy split** — IRIS is at seL4's model, and the last two
form divergences closed this cycle (A-32, the invocation ABI; A-33, the
message ABI).

What is left is not model debt. It is **absent features** (seL4 has them,
IRIS has not built them), **two deliberate differences** the charter
registers, and **one structural property IRIS has and seL4 does not need**.

| Dimension | State |
|---|---|
| Authority model (A1–A10) | **at seL4** |
| Object model (O1–O6) | **at seL4**, 11 retypeable types |
| IPC model (I1–I7) | **at seL4**, plus one deliberate extra |
| Scheduling (S1–S5) | **at seL4-MCS** |
| Memory (M1–M5) | **at seL4** |
| Policy (P1–P3) | **at seL4** |
| ABI FORM | **at seL4** since A-32/A-33 |
| Kernel architecture | **at seL4** since D-1 (one stack per core) |
| Feature coverage | **behind**: SMP, domains, IOMMU, 4 invocations |
| Verification | **not comparable**: seL4 is proved, IRIS is tested |

---

## Part 1 — the authority layer

### `new_core/src/cspace.c` (701) — CSpace resolution

seL4's `lookupCap`/`lookupSlot`. One namespace, one leg: a value is a CPtr or
it is `INVALID_ARG`. The dual resolver and the handle table are deleted
(Stage 4), not reduced to zero callers.

Guards exist and the walk consumes them (`CSpace_SetGuard`, D-2), which is
seL4's mechanism. **Half divergent**: seL4 resolves a CPtr by
`guard + radix` at every level and IRIS's default with no guard installed is a
pure radix walk, so the same CPtr means what the CNode sizes along the path
say it means. Additive, not a hole — recorded as D-2, half closed.

### `new_core/src/kcnode.c` (931) — slots and the derivation tree

The CDT/MDB. Every capability is a node with a parent; `CSpace_Revoke` walks
descendants, recursively and across processes, and is **preemptible** since
Stage 9-evt.

**LEGACY_ROOTs: 25** of 490 live nodes (T305, measured every run). Every one
is on the boot path, which seL4 has too — its BootInfo capabilities are roots.
Every class that was a *defect* is gone: fault delivery (A-22 made a fault an
IPC message, so nothing is published), the SELF syscalls (A-18), KVmo
publishes (D-5), and second-level Untypeds (A-14). T305 pins the count as a
CEILING, not only against growth.

### `new_core/src/kbootcap.c` (94) — boot authority

Six capabilities of exactly one authority each; a zero or multi-bit kind is
refused at the only place one can come into existence. seL4 has `IRQControl`,
`IOPortControl`, `ASIDControl` in the same role.

**Two have no seL4 equivalent**: initrd control and framebuffer control. seL4's
root task is handed its boot image as frames described in BootInfo rather than
through a kernel call. This is IRIS's one remaining *boot-shape* difference,
and it is small: `Boot_InitrdFrame` hands out a frame carved from the caller's
own budget, so the authority is capability-gated and the memory is paid for —
it is the *existence of the call* that differs, not its safety.

The slab path (`kbootcap_alloc_ports`) is boot-only and bounded; the ring-3
path (`kbootcap_alloc_from`) charges an Untyped the caller named. The purity
gate proves no ring-3 caller reaches the first.

### `new_core/src/kirqcap.c` (55), `kioport.c` (63)

seL4's `IRQHandler` and x86 `IOPort` capabilities. Both carve their header
from a named Untyped (`_alloc_from`), so a service that asks for one pays for
it. The slab-allocating twins are deleted, not allowlisted.

### `core/usercopy.c` (79) — user memory, one direction

**The kernel has no user-pointer READ.** Not "few": none. Every access to
user memory is a write-back — `Boot_KlogDrain`, `Boot_SchedInfo`,
`Boot_FramebufferInfo`, `Untyped_Info`, `Untyped_Query`, `TCB_GetInfo`,
`TCB_ReadRegs`, `Notification_Poll` — each answering a question INTO a buffer
the caller named and taking nothing but the address from it.

`user_range_readable`, `copy_from_user_checked` and `copy_user_cstr_bounded`
were deleted in this audit with no callers to update. A-33 removed the last
one when a message stopped being a struct in user memory; the functions
outlived it.

The consequence is worth stating positively: **there is no TOCTOU window on
any input the kernel acts on**, because it does not read its inputs from
memory another thread can unmap. That is seL4's arrangement — it reads the
IPC buffer through its own mapping and nothing else — reached from the other
direction, by having no second mechanism left that wanted a pointer.

### `new_core/src/kobject.c` (88) — lifetime

**D-7, deliberate and permanent.** IRIS reference-counts object lifetime
(`refcount` + `active_refs`); seL4 derives it from the CDT. The SEMANTICS are
met and measured — the charter's O3/O4/O5 hold — and the one disagreement the
mechanism produced (a slot that named itself) was fixed at Stage 7-proc. This
is upstream of the CSpace-cycle row and of D-8's unbounded revoke, both since
closed.

---

## Part 2 — the object layer

### `new_core/src/kuntyped.c` (433) — Untyped

**11 retypeable types**, against seL4's set:

| IRIS | seL4 |
|---|---|
| Untyped, TCB, Endpoint, Notification, CNode, SchedContext, Reply, Frame, PageTable, VSpace, ASIDPool | the same eleven (seL4 splits paging levels by architecture) |

Every one is born from Untyped, is a child of it, and the region refuses RESET
while a child lives. A DEVICE Untyped must be paired with a RAM one that pays
for its headers, or it refuses to retype (D-9) — no kernel fallback.

### Reserved-dead enumerators

`KOBJ_PROCESS`, `KOBJ_CHANNEL`, `KOBJ_VMO`, `KOBJ_INITRD_ENTRY` — four wire
type values kept so `Cap_Identify` does not renumber what every other
capability calls itself. **No struct, no allocator, no producer** for any of
them. The last one went in this audit: `KInitrdEntry` still held a row in the
purity allowlist for an allocation nobody could reach.

### `new_core/src/ktcb.c` (100), `kschedctx.c` (211)

S1 met: TCB and SchedulingContext are separate objects. MCS is real —
**sporadic replenishment** (budget is a per-period guarantee, not a slice),
**timeout faults** to a temporal supervisor, and **SC donation** across a Call
so a passive server runs on its caller's time.

**`SetMCPriority` is the one MCS invocation IRIS does not have**, and the
reason is a deliberate substitution: a thread inherits the ceiling of whoever
configured it (A-20), so a priority bound travels with delegation. That is the
same rule as seL4's with the common case built in; what IRIS cannot express is
*lowering* a ceiling after configure time.

### `new_core/src/kframe.c` (332), `kpagetable.c` (61), `kvspace.c` (646)

Stage 6-pure: the kernel creates **no** page table, no PML4, no CNode. The
holder retypes each and installs it, and a map whose walk is incomplete
answers `MISSING_TABLE` rather than allocating. One `Frame_Map` maps every
page of the frame (D-10).

**Missing invocations**: `PageTable_Unmap` and `Frame_GetAddress`. seL4 has
both. A page table today comes out only when its VSpace does; a frame's
physical address is not askable. Neither is a purity hole — you cannot use
either to acquire authority — but they are two entries of real seL4 API that
IRIS does not answer.

### `new_core/src/kasidpool.c` (80) — A-21

The last ambient RESOURCE. Identifiers come from a pool somebody holds, and a
VSpace with no identifier is refused by `TCB_Configure` — an address space
nobody was granted a name for is memory, not somewhere a thread can run.

---

## Part 3 — IPC

### `core/syscall/syscall_endpoint.c` (1563) — the largest file in the kernel

Synchronous rendezvous, badges, staged capability transfer, bulk payload.

- **Transfer is a COPY** (A-29). The sender keeps what it sent; the receiver's
  capability is a revocable derivation child. This was registered as a
  permanent divergence one session and retired the next — the kernel already
  installed the delivered capability as an MDB *child* and then deleted the
  parent, so the tree and the operation contradicted each other, and the tree
  was right.
- **The badge is unforgeable structurally**, not by overwriting: since A-33 it
  is a RETURN register, so a message has no field a sender could put one in.
- **A receive that declares no slot gets the message without the capability.**
  Handle materialization is retired; `iris_ipc_stat_handle_deliveries` and
  `_toctou_fallbacks` are structural zeros pinned by T094/T095/T096.
- **A staged capability's parent is an OBJECT, not a location** — fixed in
  this audit, see Part 6.1.

### `kernel/include/iris/ipc_msg.h` — the message ABI (A-33)

A MessageInfo word plus message registers; a payload lives in the thread's
registered IPC buffer and the message carries a **length**, never an address.
There is no pointer on the message path, so there is nothing to validate and
nothing for a second thread to unmap between the check and the copy.

**Two deliberate differences, both registered:**

1. **A delivered capability reports its RIGHTS.** seL4's `extraCaps` says a
   capability arrived; IRIS's MessageInfo says which rights it arrived with,
   and zero when none did — unambiguous, because a capability with no rights
   cannot be transferred at all. More than seL4 reports, on purpose: a
   receiver that must ask a second time about a capability it was just handed
   can be told a different answer in between.
2. **IRIS counts four message registers and bulk BYTES separately**; seL4
   counts registers in `length` and spills past four into the buffer. IRIS's
   bulk transfers are byte streams (paths, file contents), not word arrays.

### `core/syscall/syscall_reply.c` (539) — KReply

Explicit MCS-style reply objects, retyped by the server. One-shot per binding;
the object returns to free and is reusable. `ReplyRecv` exists and is what a
passive server needs.

### `core/syscall/syscall_invoke.c` (169) — the door (A-32)

62 labels, one flat list, the way `enum invocation_label` is. The dispatcher
routes on the label alone; the type is checked inside the method by the
resolver that fetches the capability with the type it requires, and a label
sent to the wrong kind answers `WRONG_TYPE` (A-30) — a better answer than
seL4's `IllegalOperation` for the same mistake, and free, because the check
was already being made.

**Deliberate difference:** seL4 keeps `Send`/`Recv`/`Call`/`Reply`/
`ReplyRecv`/`NBSend`/`NBRecv` as real syscalls, because `msgInfo`'s label is
application data and the NUMBER is what says which IPC verb was meant. IRIS's
message carries its own label, so the number is not needed for that and
`EP_Send` is a method like any other. More uniform than seL4; recorded rather
than rounded to "IRIS has seL4's shape".

### `core/syscall/syscall_dispatch.c` (399)

**Three syscall numbers**: `EXIT`, `YIELD`, `CLOCK_GET`. Each is there because
it invokes nothing — which is why seL4 keeps `seL4_Yield`. The
numbered-call gauge reads **zero**; T337 asserts it.

---

## Part 4 — architecture

### `core/scheduler/` + `arch/x86_64/syscall_entry.S` — D-1, closed

**One kernel stack per core, and no thread blocks inside the kernel.** This
was the deepest divergence in the ledger and the one that made "IRIS cannot
bound in-kernel latency" true. All three steps landed: restart-safe blocking
handlers, frame abandonment (`task_park_restart` →
`syscall_restart_trampoline`), and one stack per core, including the
conversion of the *preemption* path — a timer interrupt that lands on a
per-core stack and then preempts would otherwise leave the outgoing task's
frame on a stack the incoming one is about to use.

The bill is six pushes on the syscall path for the callee-saved registers,
and it is **the same bill seL4 pays**: an event kernel has no frame to leave a
user context on, so it saves the whole context into the TCB on every entry.

### `core/syscall/syscall_cspace.c` — D-8, closed

`CSpace_Revoke` is preemptible. It was recorded as blocked on D-1, and D-1's
step 1 built exactly the mechanism it needed.

---

## Part 5 — what is actually missing

Four kinds of gap. None of them is model debt.

### 5.1 — Absent seL4 invocations (4)

| Missing | Consequence |
|---|---|
| `PageTable_Unmap` | a paging level comes out only with its VSpace |
| `Frame_GetAddress` | a frame's physical address is not askable |
| `CNode_Rotate` | the three-slot atomic move; IRIS has Delete/Swap/Move/Mint/Revoke |
| `TCB_SetMCPriority` | a ceiling is inherited at configure time (A-20) and cannot be lowered afterwards |

### 5.2 — Absent subsystems (3)

| Missing | State |
|---|---|
| **SMP** | `cpu_local[]` exists, GS-relative per-core state is wired, the event kernel landed *specifically* so SMP atomicity is derived once — but **no AP is ever started**. Single core. |
| **Domain scheduler** | seL4's top-level time partitioning. Zero references in IRIS. |
| **IOMMU / IOSpace** | zero references. A device with DMA is trusted with memory, which is the one place IRIS's isolation is weaker than seL4-on-x86-with-VT-d. |

### 5.3 — Deliberate and permanent (2)

- **The rights set** (D-3): `READ/WRITE/DUPLICATE/TRANSFER/WAIT/ROUTE/MANAGE`
  against seL4's `Read/Write/Grant/GrantReply`. seL4 does not treat
  copyability as a right — it follows from type and derivation — while IRIS
  gates minting with `DUPLICATE` and transfer with `TRANSFER`, which is what
  makes a delegation non-re-delegable *today*. The two models are not
  translatable one-to-one, so "IRIS has seL4's rights" is never a correct
  statement.
- **Reference-counted lifetime** (D-7), above.

Plus the two IPC differences in Part 3, which are additions rather than gaps.

### 5.4 — Not comparable (1)

**seL4 is formally verified. IRIS is not, and does not claim to be.** Its
invariants are proven by construction plus adversarial gates: 306 runtime
tests including model-based syscall fuzzing, 27415 host assertions, and
`check_purity` on every build. That is a different kind of assurance, and the
charter says so in its first section rather than at the end.

---

## Part 6 — what the second pass found

### 6.1 — A real defect: charter A9, in the stage/deliver window

A transfer is a COPY, and the copy is installed as an MDB child of the
sender's source slot. Staging records WHERE that source was — a CNode and a
slot index — and the delivery happens LATER, at the rendezvous.

A slot is a reusable location, and the sending thread is not the only thread
in its process. A sibling could delete that slot and mint something unrelated
into it while the sender was blocked, and `kcnode_slot_install_linked` checked
only that the parent slot was **occupied**. The delivered capability was then
linked as a child of whatever now sat there: an ancestor that never authorised
it. Revoking the new occupant destroys a capability it has no relation to;
revoking the real ancestor does not reach the copy. **A9 fails in both
directions.**

The helper for it was already written — `kcnode_slot_holds`, commented
"Identity, not occupancy", describing this exact hazard — and had **zero
callers**. The delivery path's own comment claimed the property its code did
not check. Found by sweeping the kernel for functions nothing calls.

Now wired up, and **T339** drives the window: stage a notification, swap the
source slot for an endpoint while the sender is blocked, take delivery. The
message still arrives; the capability does not. Verified in both directions —
with the check disabled the suite reports `T339 FAIL: delivered a capability
whose parent had been replaced`.

### 6.2 — A gap in the enforcement itself

`make check-purity` is what turns charter M3 from a property into an enforced
one. It froze `kslab_alloc` and computed, transitively, whether any function a
syscall handler names can reach it.

**It never looked at the PMM.** `pmm_alloc_page`/`_pages`/`_block` is the
kernel's other way to memory, and unlike the slab it is not sealed after boot.
Extended to cover it, the gate immediately found a real static path:
`kframe_map_page` → `paging_map_checked_in` → `get_or_create` → `alloc_table`
→ `pmm_alloc_page`, named directly by `sys_frame_map`.

Not exploitable — the branch was guarded by `vs->kernel_funded`, set by one
constructor that allocates from the sealed slab — but held by a runtime flag
and a second mechanism's seal rather than by construction. So the branch is
**gone** rather than exempted: `bootstrap_kframe_map` fills the root task's
paging levels itself, exactly as `PageTable_Map` does for a ring-3 holder, and
`kframe_map_page` has one behaviour for every address space. A failed map now
always means `MISSING_TABLE`; it could previously mean `NO_MEMORY`, which was
the kernel reporting that IT had failed to allocate.

**And a second bug, found by probing the gate rather than reading it.** The
comment stripper dropped the WHOLE line when a comment opened on it, so
`f(x);  /* why */` was invisible — most call sites in this tree carry a
trailing comment. A probe call added to a syscall handler on such a line did
not trip the check. Fixed, and both probe forms now fail the gate as they
should.

### 6.3 — Scaffolding that outlived what it named

A divergence closes, the mechanism comes out, and the thing that named it does
not. Seven instances, all removed:

| What | Why it was still there |
|---|---|
| `kernel/mm/kpage/kpage.c` | a whole contiguous-pages allocator, linked into every build; `nm -u` over every kernel object finds no reference. Its only callers are host tests, which stub it themselves. A **second** path to kernel memory the gate did not model |
| `kernel/core/scheduler/kstack.c` + `KSTACK_VIRT_BASE` | D-1 deleted per-thread kernel stacks; the file, its fatal reporter, two prototypes, a TCB field written-never-read, and a **three-pages-per-task virtual address-space reservation** stayed |
| `KInitrdEntry` | an object type with no producer and no consumer, holding a frozen row in the purity allowlist for an allocation nobody could reach |
| `nc/kprocess.h` | a header named after an object deleted in Stage 7, included by 12 kernel files. It said so itself: "keeps its name until the symbols are renamed". Now `nc/kfault.h`; 25 comments described a process lifecycle that does not exist |
| `TASK_MAX`, `TASK_STACK_SIZE` | kept for two consumers (the kstack window, a `tasks_max` field) that had both since gone |
| `_sc_putc`, `copy_kbuf_r`, `kcnode_mint_badged` | uncalled. The first writes raw bytes to COM1 from ring 0, bypassing klog — a debugging aid that outlived its session |
| `kbd_proto.h`, `console_proto.h` | KChannel-era protocols, still `#include`d by three services that used nothing from them |

### 6.4 — One device the kernel knew about

`isr_handler` had a case for vector 33 labelled "IRQ1 — PS/2 keyboard" whose
body was identical to the generic 34..47 case. Not a special case: the general
case, written twice, with a device named in the second copy — in the one
kernel file whose subject is that the kernel does not know what is behind a
line. Merged. The timer (IRQ0) stays special and should: it is the kernel's
own, for preemption and MCS accounting, which is what seL4 keeps its timer
for.

### 6.5 — What the second pass did NOT find

No ambient authority. No second namespace. No policy in the kernel. No
allocator reachable from a syscall. Two honest `TODO`s, both SMP. Every
`sys_*` handler is dispatched; after this pass, every non-static kernel
function has a caller.

---

## The number

If the question is *how close to 100% pure seL4*, the honest decomposition is:

- **The capability model: complete.** Every box in charter §4 is checked, and
  the two that were checked optimistically (the IPC buffer, ReplyRecv) have
  since been closed for real rather than re-argued.
- **The ABI form: complete**, as of A-32 and A-33. It was registered as a
  permanent divergence twice and retired twice, both times because the row had
  priced the work correctly and the gain not at all.
- **The kernel architecture: complete**, as of D-1.
- **Feature coverage: roughly three-quarters.** SMP, domains and IOMMU are
  three real subsystems seL4 has and IRIS has not built, plus four invocations.
- **Verification: not started, and out of scope by charter.**

The gap that remains is **work IRIS has not done**, not shape IRIS got wrong.
There is no mechanism left in the kernel that a purity audit would ask to be
removed.

That claim is stronger after the second pass than before it, and for a reason
worth being precise about. The first pass could say "nothing is left to
remove" only of the mechanisms it looked at. The second pass looked at every
file, and found one real defect and one hole in the enforcement — neither of
which was a mechanism anybody had to argue about keeping. Both are closed.

What that suggests about the remaining distance: the risk is no longer that
IRIS has kept something seL4 would not have. It is that a property IRIS
believes it holds is held by a runtime condition rather than by construction,
and nothing checks which. That is what §6.2 was, and it is the shape to keep
looking for.

---

## What these audits changed

| Pass | Change |
|---|---|
| 1 | `docs/` described the ABI from before A-32 and A-33 — the syscall contract listed 71 live numbers by name. Fifteen documents and five protocol headers corrected |
| 1 | `KInitrdEntry` deleted — an object type with no producer, consumer, or reachable allocation |
| 2 | **Charter A9 defect fixed** in the stage/deliver window, with T339 as the regression (§6.1) |
| 2 | **Purity gate extended to the PMM**, the static path it found removed rather than exempted, and its comment stripper fixed (§6.2) |
| 2 | Seven pieces of scaffolding removed, including two kernel allocators and an address-space reservation (§6.3) |
| 2 | The kernel's one named device merged into the generic IRQ path (§6.4) |
| 2 | `nc/kprocess.h` → `nc/kfault.h`, closing a debt the file recorded against itself |

Net: **62 files changed, 544 insertions, 706 deletions** across the second
pass; 306 runtime tests, 27415 host assertions, purity clean.
