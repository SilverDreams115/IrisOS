# IRIS — seL4 Convergence Roadmap (normative, by dependencies, no dates)

Orders the stages toward the [purity charter](iris-sel4-purity-charter.md).
Each stage declares its **technical precondition** (what must be closed
first) and its **closing criterion** (what must be demonstrable when it ends).
The [ledger](sel4-convergence-ledger.md) maps every transitional mechanism to
its retirement stage. No stage may be declared closed while its productive
path still depends on the mechanism it retires (charter §3.10).

## Status

| Stage | State |
|---|---|
| 0 — TCB consolidation | ✅ CLOSED (Fase S2 inc.2) |
| 1 — CDT/MDB | ✅ CLOSED (Fase S3) |
| 2 — CSpace-only cap transfer | ✅ CLOSED (Fase S4) |
| 3 — CSpace-only derive and revoke | ✅ CLOSED (Fase S4) |
| 4 — Dual namespace retirement | ← IN PROGRESS (Etapas 1–6b done; 6c remaining) |
| 5 — seL4-like bootstrap | pending |
| 6 — Remaining memory and objects | pending |
| 7 — KProcess retirement | pending |
| 8 — Full MCS scheduling | pending |
| 9 — SMP | pending |
| 10 — General-purpose platform | pending |

Charter invariants closed so far by this roadmap: **A6, A7, A8, A9, A10**
(authority); **O2–O6** (objects); **I1–I7** (IPC); **S1–S5** (scheduling);
**M2–M5** (memory); **P1, P3** (policy).  Still open: **A2, A3, A4** (Stage 4),
**A5** and **P2** (Stages 5–7), **O1** and **M1** (Stages 5–6).

## Stage 0 — TCB consolidation  ✅ CLOSED (Fase S2 inc.2)

- The open increment is closed and committed; the working tree is clean.
- Canonical KTCB: `struct task` IS the object (KObject at offset 0); the
  wrapper is removed; five separated lifetimes (cap / object / execution /
  registry / storage) with no ambiguous refcount.
- Stable lifecycle: TERMINATED ≠ destroyed; the destructor is the sole
  storage releaser; pointer-based run queues; registry with generation.
- Retypable storage: `RETYPE2(KOBJ_TCB)` creates canonical TCBs (inactive,
  `configured=0`) with storage inside the Untyped and the cap directly in
  CSpace; the migrated family is now {EP, Notif, Reply, CNode, SC, TCB}.
- No new handle: RETYPE2 creation publishes no handles; the `make
  check-purity` guard freezes the existing consumers.
- Recorded debt: the thread EXECUTION path (SYS_THREAD_CREATE) still comes
  from the static pool + handle; its replacement (TCB_CONFIGURE over a
  retyped TCB) requires CSpace/VSpace caps as arguments and is defined in
  Stage 5/6 (post-CDT). The idle task is an isolated bootstrap exception
  (registry slot 0, never retyped or reused).

## Stage 1 — CDT/MDB  ✅ CLOSED (Fase S3)

Precondition: Stage 0 (closed).
Design: `docs/architecture/cspace-cdt-mdb.md`.

- Intrusive per-slot derivation metadata (not in handles): parent /
  first-child / doubly-linked siblings.
- Global parent/child relationships (cross-CNode, cross-process) — the links
  are slot pointers, agnostic of the owning KProcess.
- Single canonical primitives (`kcnode_slot_install_linked/derive/move/
  delete/revoke`); no TU mutates `cn->slots[]` directly.
- Recursive cross-process revoke (`SYS_CSPACE_REVOKE`) with deterministic
  order (deepest-leftmost post-order) and lifecycle effects outside the lock;
  proven by T288-T290 (runtime, real processes) + model-based fuzzing
  (5 seeds × 4000 ops, parent-vector comparison).
- Exact rollback (retype2 publishes via the primitive; a failure uninstalls
  the leaves and undoes the carve). delete ≠ revoke; intermediate delete
  reparents to the grandparent.
- Untyped as the MDB ancestor of its retyped objects (D.1/D.2/D.3).
- Locking: global `mdb_lock` → `cn->lock`; releases outside the lock.

Debt that stays live (does NOT block, retired in later stages):
`legacy_handle_derivation_migrated` (parallel handle-tree, `SYS_CAP_DERIVE`)
→ Stage 3; `mdb_legacy_roots` (non-CSpace origins) → Stages 2/4/5;
`cdt_ipc_transfer` (IPC delivery = LEGACY_ROOT) → Stage 2.

## Stage 2 — CSpace-only cap transfer  ✅ CLOSED (Fase S4)

Precondition: Stage 1 (closed).

- CPtr source: `syscall_ipc_stage_cap_peek_badged` resolves the source
  through `cspace_resolve_slot` and carries the slot identity
  (`task.ep_cap_src_cn`/`ep_cap_src_idx`) across a blocking send.  A handle
  value is `INVALID_ARG` — no fallback (invariant A6).
- The delivered cap is installed with `kcnode_slot_install_linked` as an MDB
  **child of the source slot**, not a LEGACY_ROOT: an IPC delegation is now
  revocable from the sender or any of its ancestors.
- Order: DELIVER, then commit.  MDB parenting needs the source slot occupied,
  so the source is consumed only after the child exists; move semantics are
  preserved because `kcnode_slot_delete` reparents the delivered cap to the
  grandparent.  A cap revoked while staged is never delivered (entry
  invariant 4).
- The TOCTOU slot→handle degradation is REMOVED — the last CPtr→handle
  fallback in the kernel.  A raced/occupied destination fails closed: the
  message arrives with no capability and the source slot is untouched.
  `iris_ipc_stat_toctou_fallbacks` is a structural 0 (T094 forces the race,
  T095 pins the counter).
- Endpoint close leaves the source-slot refs for the woken sender to drop:
  releasing the last ref on a CNode runs a destructor that tears down every
  slot, which must not happen under `ep->lock`.

Debt that stays live (does NOT block): delivery into the receiver's handle
table when it declares NO receive slot (legacy receivers) → Stage 4 with the
dual namespace; `SYS_CNODE_MINT` still marks its slot a LEGACY_ROOT → Stages
3/4.

## Stage 3 — CSpace-only derive and revoke  ✅ CLOSED (Fase S4)

Precondition: Stages 1–2 (both closed).

- `SYS_CAP_DERIVE` (78) and `SYS_CAP_REVOKE` (79) are RETIRED: the numbers
  stay permanently reserved and answer `NOT_SUPPORTED`.
- The handle table's parallel derivation tree is DELETED — its derived-insert
  and revoke-children entry points and the per-slot parent array are gone.
  The handle table is now a flat reference table with no derivation semantics.
- Every productive and test path derives with `SYS_CSPACE_MINT` (slot→slot,
  installing a real MDB child) and revokes with `SYS_CSPACE_REVOKE`
  (recursive, cross-CNode, cross-process).
- Unblocked earlier in Fase S4 by giving `KIoPort`/`KIrqCap` a CSpace-native
  origin (see the Stage 3 prep note in the ledger).
- `legacy_handle_derivation_migrated` has zero callers: a structural 0, kept
  as the retirement witness in the `UNTYPED_QUERY` layout.

Result: there is exactly ONE derivation tree in the system.  Charter A9 and
A10 move to MET.

## Stage 4 — Dual namespace retirement  ← IN PROGRESS

Precondition: Stages 2–3 (both closed — no authority lives handle-only anymore).

- Remove the value-range discrimination (<1024 / ≥1024).
- Remove handle resolution from every dual resolver.
- Remove the bootstrap's handle producers (kernel_main dual insert,
  `SYS_CSPACE_RESOLVE` materialization).
- Remove the handle table when it has zero consumers; the `check_purity`
  allowlist must reach empty.

Measured surface (from `scripts/purity_allowlist.txt`, which is the executable
inventory — it only shrinks):

| Frozen consumer | Close of Stage 3 | Now | Files |
|---|---|---|---|
| `cspace_or_handle_resolve_` | 104 | 107 | 17 |
| `handle_table_get_object` | 52 | 37 | 14 → 9 |
| `handle_table_insert` | 42 | 41 | 14 → 13 |

`cspace_or_handle_resolve_` grew by 3 under charter §3 (ledger A-2): three
syscalls resolved their object arguments either way while their NOTIFICATION
argument stayed handle-only, so each trade swapped a handle-namespace consumer
for a dual one on the same argument.  The dual resolver's handle leg is deleted
wholesale when the namespace retires.

(`kslab_alloc`, 20 occurrences across 16 files, is Stage 6's inventory, not
Stage 4's.)

### Etapa 4 — the CSpace root stops being a handle  ✅ DONE

`KProcess.cspace_root_h` (a `handle_id_t` into the process's own handle table)
became `KProcess.cspace_root` (a `struct KCNode *`, holding the same lifecycle
+ active ref pair, released in `kprocess_teardown`).  This removed the
namespace inversion at the base of the whole stage: **every** CPtr resolution
began by looking the root up in the namespace CSpace was built to replace.  It
also ended cross-process handle-table access — `SYS_CSPACE_MINT_INTO`,
`SYS_PROC_CSPACE_MINT`, retype2's `dest_cnode == 0`, `SYS_CNODE_DELETE` and IPC
receive-slot delivery all read the target's root structurally now.

Two userspace consequences, both retiring guesswork rather than adding API:

- `SYS_CNODE_MINT` accepts `arg0 == 0` = "my own root CNode", the convention
  `SYS_CNODE_DELETE` and `SYS_UNTYPED_RETYPE2` already used.  svcmgr and
  `iris_test` used to *probe their own handle table* for the first CNode-typed
  generation-1 id, which only worked because the kernel published the root as
  every process's first handle.  Both probes are deleted.
- userboot's two liveness-only CPtr probes are deleted, and its two founding
  mints for `init` now take CPtr sources (`SYS_CSPACE_MINT_INTO`) instead of a
  `SYS_HANDLE_DUP` + `SYS_CSPACE_RESOLVE` pair — so init's founding caps are
  installed as MDB children of userboot's slots, i.e. revocable, instead of
  handed over forever.

### Etapa 5 — the productive path leaves the bridge  ✅ DONE

`init` has ZERO uses of the CPtr→handle bridge and zero `SYS_HANDLE_DUP`: its
bootstrap authority, every object it fabricates, its death watch and its
selftest notifications are capabilities in slots.  `svcmgr` is down to one use
(the delivered-cap path, which is the IPC-delivery-without-receive-slot legacy,
not svcmgr's to fix).  `userboot`, `vfs`, `kbd`, `console` and `sh` were already
clean.

Four defects surfaced, none of them the migration's own:

- `SYS_BOOTCAP_RESTRICT` resolved its argument either way but published the
  restricted clone with a handle-table write — a CPtr could never succeed.  It
  derives into a destination slot now, as an MDB child of the source.
- KDEBUG was **ambient**: `SYS_KLOG_DRAIN`/`SYS_SCHED_INFO`/`SYS_POWEROFF`
  scanned the caller's handle table instead of taking a capability.  Retired
  (ledger A-3); it is Stage 5 groundwork, not Stage 4 cleanup.
- Three syscalls (`SYS_PROCESS_WATCH`, `SYS_EXCEPTION_HANDLER`,
  `SYS_IRQ_ROUTE_REGISTER`) had their object arguments migrated and their
  NOTIFICATION argument left behind.  **Any syscall taking a notification
  beside an already-dual argument should be assumed to have this gap until
  checked** (ledger A-2).
- The pager's manifest oracle leaked one handle-table entry per occupied slot,
  per request, with no ceiling.

### Etapa 6a — CSpace-native introspection  ✅ DONE

The bridge had two distinct users, and only one of them was a test.  Every
remaining PRODUCTIVE use of `SYS_CSPACE_RESOLVE` was asking one of two
questions about a slot the caller already named — *what type is this
capability* (svcmgr, dispatching on a delivered cap) and *is this slot
occupied* (the `pager` and `lifecycle_probe` manifest oracles).  Both were
answered by materialising the slot into a handle and immediately closing it:
asking for authority to learn a fact, and consuming a handle-table entry per
occupied slot on every request.

`SYS_CAP_IDENTIFY` (117) and `SYS_CAP_SAME_OBJECT` (118) answer those two
questions natively — CPtr only, no handle produced, nothing retained past the
call, no dual resolution and no fallback.  They are the CSpace-native
successors of `SYS_HANDLE_TYPE` (52) and `SYS_HANDLE_SAME_OBJECT` (53), which
now retire *with* the namespace instead of blocking it.

This reverses the previous conclusion that the oracles "retire WITH the bridge
rather than before it".  That held while the only way to ask was to mint a
handle.  It does not hold against a primitive that is strictly weaker than the
bridge: identify returns a scalar where resolve returned authority.  Observing
which of your OWN slots are occupied is not a leak — a caller learns the same
thing by invoking any slot and reading `NOT_FOUND`, in IRIS and in seL4 alike,
and a CSpace's layout is not a secret kept from its owner.

Result: **svcmgr, `pager` and `lifecycle_probe` are off the bridge.**  The
suite is now the only consumer left, which is what Etapa 6b addresses.
Covered by T292/T293 (type per family, no right required, empty slot is
`NOT_FOUND`, identity survives rights reduction and badging, handle value is
`INVALID_ARG` on every argument).

### Etapa 6b — CSpace stops being ten bits wide  ✅ DONE

Two mechanisms still assumed the pre-`HANDLE_TAG` world, and both of them
capped what a CSpace could be:

**The IPC receive slot was a direct index into the root CNode.**  Declaration
and delivery both open-coded `slot < 1024` and installed straight into
`proc->cspace_root[slot]` — no traversal.  A process whose root CNode is full
therefore could not receive a capability at all, which is not a hypothetical:
this suite's root is ~97% allocated, which is why its fabricated objects
already live in a second-level CNode.  The declaration is a full CPtr now,
resolved by `cspace_resolve_dest_slot` — the destination analogue of
`cspace_resolve_slot`, which allows the terminal slot to be EMPTY (that is the
normal case for an install target) while requiring every intermediate level to
really be a CNode.

**The delivery discriminator was the literal 1024.**  `iris_msg_cap_is_cptr`
was written when a handle was `slot | gen << 10` and so always ≥ 1024.  Handles
carry bit 31 now and CPtrs own the low 31 bits, so a two-level CPtr such as
`(leaf << 8) | 80` — 64080 for leaf 250 — was classified as a *handle* by every
consumer of that helper.  `IRIS_CPTR_LIMIT` is `HANDLE_TAG` now and agrees with
`CSPACE_DIRECT_CPTR_LIMIT`, which `nc/cspace.h` already declared to be the one
definition of the boundary.

**A CPtr addressed more than one capability.**  Resolution consumed radix bits
per level and treated a slot as terminal when the CPtr was exhausted *or* the
slot held a non-CNode — the second clause silently DISCARDED the leftover bits.
With a 256-slot root, CPtr `k`, `k+256`, `k+512` … all resolved to slot `k`:
roughly 2^23 aliases per capability.  A capability address space whose
addresses are not injective cannot be reasoned about — an off-by-one in a
computed CPtr hits a live capability instead of failing, and a value chosen
*because* it is invalid may not be.  The suite's own fuzz constant 4095 aliased
root slot 255, its serial `KIoPort`.  Leftover bits with nothing to descend
into are now `INVALID_ARG` on all three resolvers, which is how seL4 treats the
same shape (depth mismatch).

Covered by T294 (deliver into a second-level slot; the returned value is the
declared CPtr and is classified as a CPtr; occupied deep slot fails fast),
T295 (aliases rejected on invoke / identify / mint-source / receive-slot
paths), and host cases in `tests/kernel/test_cspace.c`.

### Etapa 6c — the test suite  ← REMAINING

`iris_test` is what keeps Stage 4 open, and it is not one migration.  All 268
tests classified against a single rule — a test whose SUBJECT is the handle
namespace dies with the mechanism; a test asserting an authority property that
survives in a CSpace-only kernel is rewritten, because the property is real and
only the vehicle changes:

| | tests | disposition |
|---|---|---|
| subject is the handle namespace | 57 | deleted WITH the mechanism, not before |
| use the bridge incidentally | 84 | rewritten in CSpace |
| already CSpace-only | 127 | untouched |

T011 (`SYS_HANDLE_TYPE`) and T012 (`SYS_HANDLE_SAME_OBJECT`) are the first
group: migrating them would leave them asserting nothing.  T019 is the second —
"dropping the last capability to an endpoint wakes a blocked receiver" is as
true in seL4, it was merely spelled `SYS_HANDLE_CLOSE`.

Migration is per-test and cannot be batched blindly: a sweep over every
`it_ep_create()` inside tests that also call `it_register_ep` broke eight at
once, because those tests use the same endpoint for other things and their
`it_close()` calls also close process and thread handles that are not moving.

It CAN be batched behind the runtime gate, which is how the bulk moved: convert
a group, run the suite, and read the failures as a map of what the group was
really doing.  Every failure so far was a place where an operation had a CSpace
form nobody had switched to — `SYS_CNODE_MINT` (handle-only source) where
`SYS_CSPACE_MINT` belonged, a handle-to-handle identity comparison where
`SYS_CAP_SAME_OBJECT` belonged, `SYS_HANDLE_DUP` where a slot-to-slot derive
belonged — not a place where the handle was load-bearing.  `it_cs_reduce` is
the last of those: the CSpace form of "a rights-reduced copy", which is also
strictly better than the dup it replaces, because the reduced cap is an MDB
child of its source and therefore revocable.

**Progress.**  Bridge uses inside `iris_test`, counted as occurrences of
`it_ep_create_h` / `it_notify_create_h` / `it_retype_handle` /
`SYS_HANDLE_DUP` / `SYS_HANDLE_TYPE` / `SYS_HANDLE_SAME_OBJECT` /
`SYS_HANDLE_CLOSE` / `SYS_CSPACE_RESOLVE` / `SYS_CNODE_MINT`:

| point | uses |
|---|---|
| close of Etapa 5 | 197 |
| after Etapa 6a/6b (lookups, liveness probes, self-proc, vestigial KDEBUG staging) | 178 |
| after the endpoint/notification fixture migration | 137 |
| after the retyped-object fixture migration | 118 |

What is left is dominated by fixtures the suite cannot yet fabricate into a
slot: `SYS_HANDLE_DUP` on VMO / KProcess / KVSpace / KFrame caps.  Those retire
as their CREATORS gain CSpace destinations — the same move the object-cap
accessors just made — not by rewriting the tests around them.

Two tests keep a handle deliberately, and are the pattern for the rest of the
"dies with the mechanism" group.  T073's third leg asserts that a HANDLE value
as an IPC transfer source is `INVALID_ARG` with no fallback, which needs a real
handle to hold wrong.  T127 and T130 assert that a copy made with
`SYS_CNODE_MINT` is an independent reference and NOT a derivation child, so it
survives a revoke of its source — that is the LEGACY_ROOT behaviour the ledger
tracks to zero, and rewriting it with `SYS_CSPACE_MINT` would assert the
opposite of what it exists to pin.  T125 now splits deliberately: it identifies
the four families with a CSpace birth through their slots and the two without
two through the handles the LEGACY `SYS_UNTYPED_RETYPE` produced.  That leg is
not an oversight: 87 is still live for `KFrame` / `KUntyped` / `KSchedContext`,
so something has to keep exercising it until it retires.  `RETYPE2` accepts
both types into a slot already, so that leg is a deletion when 87 goes, not a
rewrite.

Nothing remains of the bridge outside the suite: svcmgr's delivered-cap path
and the `pager` / `lifecycle_probe` manifest oracles all moved to
`SYS_CAP_IDENTIFY` in Etapa 6a.  Probing by attempting a mint was considered
and rejected first: it requires `RIGHT_DUPLICATE` on the source, which several
of those slots lack, so it would report absent for capabilities that are
present.

**Second-order benefit, not just hygiene.** The `<1024` split caps the whole
CPtr namespace at 10 bits.  A root CNode of 256 slots therefore consumes most
of the addressable space, and multi-level CSpace resolution — which
`cspace_resolve_slot` already implements as a radix walk — is effectively
unusable because only 2 bits remain for deeper levels.  The symptom is
concrete: `iris_test`'s root CNode is ~97% allocated, with six free slots
left, and three separate bring-up failures during Fase S4 were slot
collisions.  Removing the split frees the full 64-bit CPtr space and makes
real CSpace hierarchies possible.

## Stage 5 — seL4-like bootstrap

Precondition: Stage 4 (the initial caps can only be CSpace now).

- Replace the monolithic `KBootstrapCap` with structured BootInfo.
- Root task with: root CNode, initial TCB, initial VSpace, IRQ control cap,
  ASID/PCID control, Untyped list, fine-grained per-device caps.
- TCB_CONFIGURE/TCB_WRITE_REGS (execution of retyped TCBs) is defined here
  because its arguments (CSpace root, VSpace, fault EP) now exist as caps.

## Stage 6 — Remaining memory and objects

Precondition: Stage 1 (ownership/derivation); may overlap with 5.

- Page-table objects retyped from Untyped (retires the paging_map PMM
  reserve).
- Canonical VSpace from Untyped; Frame headers inside the region.
- Retire the remaining object kslab paths (ledger list).
- Convert or retire KVMO; separate file-backed and anonymous memory in user
  services (the pager/VFS already provide the base).

## Stage 7 — KProcess retirement

Precondition: Stages 5–6 (a process = TCB+CSpace+VSpace composition).

- Process server in user space; process creation and policy outside the
  kernel; PID stops conferring authority; per-domain quotas become the
  process server's policy.

## Stage 8 — Full MCS scheduling

Precondition: Stages 0–2 (canonical SC/TCB + CSpace-only IPC).

- SC delegation and donation during IPC where appropriate; timeouts;
  replenishment; revised priority semantics; budget tests.
- Revisit the "no combined ReplyRecv" divergence here (charter §6).

## Stage 9 — SMP

Hard precondition: single authority namespace (4), CDT (1), lifecycle (0),
CSpace-only IPC (2), and a documented locking model.

- Re-derive EVERY atomicity property that today depends on the
  non-preemptive uniprocessor kernel (catalog: IPC staging, RETYPE2, reply
  bind, teardown). Per-CPU run-queue ownership. No correctness may still be
  argued "because the kernel is non-preemptive".

## Stage 10 — General-purpose platform

Precondition: consolidated microkernel (0–9 as applicable).

- User-space drivers; PCI/ACPI/IOMMU; storage; persistent FS; networking;
  optional POSIX personality via servers/libraries; advanced security;
  performance; real hardware. None of this lands earlier: charter §5.

---

## Entry contract for the CDT/MDB increment (Stage 1)

What the CDT increment had to implement, defined so that Stage 0 would not
leave it any ambiguity. Delivered in Fase S3; kept here as the historical
contract:

**Structures.** Per-CNode-SLOT derivation metadata (not per handle): a link
to the parent slot + a list/ring of children (seL4 MDB style: a
doubly-linked list ordered by depth, or an explicit tree). The metadata
storage lives inside the slot itself (a CNode is already born from Untyped —
no kslab).

**Relationships.** Original (retype/mint from Untyped) vs derived
(copy/mint/transfer). Derivation crosses CNodes and processes. The parent
Untyped is the root ancestor of every retyped object (child_count integrates
or reconciles with the tree).

**Operations.** `copy` (same rights), `mint` (rights↓ + badge once), `move`
(relocates the slot preserving its position in the tree), `delete` (single
slot; if it is the object's last cap, destroy), `revoke` (recursively removes
ALL descendants of the slot, in any CSpace; the revoked slot survives).

**Invariants.** (1) a child's rights ⊆ parent's rights; (2) badge immutable
after the first badging; (3) deleting a parent does NOT orphan the tree
(reparent or sweep, choose and document — seL4 uses the MDB for this);
(4) revoke is atomic w.r.t. staged IPC: a cap in peek staging that is revoked
is not delivered (commit fails cleanly); (5) exact rollback if a tree
operation fails partway.

**CNode integration.** `kcnode_mint*/fetch/delete/swap` maintain the tree;
tearing down a CNode (close) deletes each slot through the tree, not just a
refcount release.

**IPC integration.** Receive-slot delivery registers the delivered cap as a
child of the source cap (prepares Stage 2).

**Untyped integration.** retype registers the destination slots as originals
of the Untyped; RESET requires an empty tree (replaces/refines child_count);
revoking the Untyped = revoking all its originals.

**Teardown integration.** A process's death deletes all its slots through the
tree; caps that OTHER processes derived from its own remain where the chosen
model defines (documented: seL4 keeps them — derivation does not impose the
holder's lifetime).

**Required tests.** Cross-process chain A→B→C + revoke at A; revoke during a
staged transfer; deleting the intermediate; mint with rights↓ and re-badge
denied; death of the intermediate holder; retype/revoke/reset stress with
gauge verification and no-UAF; a guard that `legacy_handle_derivation_migrated`
→ 0.
