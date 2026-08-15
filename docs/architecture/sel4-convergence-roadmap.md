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
| 4 — Dual namespace retirement | ← NEXT |
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

## Stage 4 — Dual namespace retirement  ← NEXT

Precondition: Stages 2–3 (both closed — no authority lives handle-only anymore).

- Remove the value-range discrimination (<1024 / ≥1024).
- Remove handle resolution from every dual resolver.
- Remove the bootstrap's handle producers (kernel_main dual insert,
  `SYS_CSPACE_RESOLVE` materialization).
- Remove the handle table when it has zero consumers; the `check_purity`
  allowlist must reach empty.

Measured surface at the close of Stage 3 (from `scripts/purity_allowlist.txt`,
which is the executable inventory — it only shrinks):

| Frozen consumer | Occurrences | Files |
|---|---|---|
| `cspace_or_handle_resolve_` | 104 | 17 |
| `handle_table_get_object` | 52 | 14 |
| `handle_table_insert` | 42 | 14 |

(`kslab_alloc`, 20 occurrences across 16 files, is Stage 6's inventory, not
Stage 4's.)

Productive userspace consumers to migrate first: `SYS_CSPACE_RESOLVE` (the
sanctioned CPtr→handle bridge, used by userboot/init/svcmgr/pager) and
`SYS_HANDLE_DUP` (init ×3, svcmgr ×2).

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
