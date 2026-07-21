# IRIS — seL4 Convergence Roadmap (normative, by dependencies, no dates)

Orders the stages toward the [purity charter](iris-sel4-purity-charter.md).
Each stage declares its **technical precondition** (what must be closed
first) and its **closing criterion** (what must be demonstrable when it ends).
The [ledger](sel4-convergence-ledger.md) maps every transitional mechanism to
its retirement stage. No stage may be declared closed while its productive
path still depends on the mechanism it retires (charter §3.10).

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

## Stage 2 — CSpace-only cap transfer  ← NEXT

Precondition: Stage 1 (closed — staging already registers the delivered cap
as a LEGACY_ROOT MDB node; it still needs a real CSpace ancestor).

- CPtr source (retires the handle-only peek of
  `syscall_ipc_stage_cap_peek_badged`); the delivered cap becomes an MDB
  child of the source slot instead of a LEGACY_ROOT.
- Destination: the receive slot (already present) as the only path.
- Staged transfer over slots with the same peek/commit atomicity.
- Remove the TOCTOU slot→handle degradation (`iris_ipc_stat_toctou_fallbacks`
  and `cdt_ipc_transfer` as LEGACY_ROOT must reach a structural 0).

## Stage 3 — CSpace-only derive and revoke

Precondition: Stages 1–2.

- Retire the handle-only `SYS_CAP_DERIVE`/`SYS_CAP_REVOKE` (or redefine them
  over slots) and the handle table's `derivation_parent[]` tree.
- Migrate the productive consumers; non-regression guards (retired syscall
  numbers stay reserved → NOT_SUPPORTED).

## Stage 4 — Dual namespace retirement

Precondition: Stages 2–3 (no authority lives handle-only anymore).

- Remove the value-range discrimination (<1024 / ≥1024).
- Remove handle resolution from every dual resolver.
- Remove the bootstrap's handle producers (kernel_main dual insert,
  `SYS_CSPACE_RESOLVE` materialization).
- Remove the handle table when it has zero consumers; the `check_purity`
  allowlist must reach empty.

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
