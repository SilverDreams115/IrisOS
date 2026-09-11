# How close IRIS is to seL4 — a file-by-file audit

**Date**: 2026-09-11, at commit `0ae36d2`.
**Method**: every kernel file read against what seL4 has in the same place,
plus the measurements the tree takes of itself. Where the answer is a number,
the number is the tree's, not an estimate.

**Gates at the time of writing**: 305/305 runtime tests, 27418 host
assertions, `check_purity` OK with the slab-reachable closure at 10 functions
and **zero ring-3 exemptions**.

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
invariants are proven by construction plus adversarial gates: 305 runtime
tests including model-based syscall fuzzing, 27418 host assertions, and
`check_purity` on every build. That is a different kind of assurance, and the
charter says so in its first section rather than at the end.

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
That is a different position from the one this project was in a year of
ledger rows ago, and it is the one worth stating precisely: there is no
mechanism left in the kernel that a purity audit would ask to be removed.

---

## What this audit changed

Two things, both committed:

1. `docs/` described the ABI from before A-32 and A-33 — the syscall contract
   listed 71 live numbers by name. Fifteen documents and five protocol
   headers corrected.
2. `KInitrdEntry` — an object type with no producer and no consumer, holding a
   frozen row in the purity allowlist. Deleted; the slab-reachable closure went
   from 11 functions to 10.
