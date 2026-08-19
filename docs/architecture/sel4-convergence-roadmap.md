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
| 4 — Dual namespace retirement | ✅ CLOSED |
| 5 — seL4-like bootstrap | ✅ CLOSED |
| 6 — Remaining memory and objects | 🔄 OPEN (Etapas 1-3 landed) |
| 7 — KProcess retirement | pending |
| 8 — Full MCS scheduling | pending |
| 9 — SMP | pending |
| 10 — General-purpose platform | pending |

Charter invariants closed so far by this roadmap: **A2, A3, A4, A6, A7, A8,
A9, A10** (authority); **O2–O6** (objects); **I1–I7** (IPC); **S1–S5**
(scheduling); **M2–M5** (memory); **P1, P3** (policy).  Still open: **A5** and
**P2** (Stages 6–7), **O1** and **M1** (Stage 6).  Stage 5 moved A5 most of the
way — boot authority is fine-grained and named, the monolith cannot be
constructed — leaving the ioport whitelist and the kernel's per-process quotas
as the remaining ambient policy.

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

## Stage 4 — Dual namespace retirement  ✅ CLOSED

**Closing criterion met: there is one authority namespace.**  The handle table
is not reduced to zero consumers — `HandleTable`, `KProcess.handle_table`, the
implementation and its unit suite are DELETED, and the purity allowlist has no
`handle_table_*` or `cspace_or_handle_resolve_` entries left because those
identifiers no longer exist.  What the allowlist still holds is Stage 6's
inventory: the object families born from the kslab heap.

Retired to `NOT_SUPPORTED`, numbers permanently reserved: `SYS_HANDLE_CLOSE`
(15), `SYS_HANDLE_DUP` (22), `SYS_IOPORT_RESTRICT` (43), `SYS_VMO_SHARE` (46),
`SYS_HANDLE_INSERT` (59), `SYS_HANDLE_TYPE` (52), `SYS_HANDLE_SAME_OBJECT`
(53), `SYS_CNODE_MINT` (81), `SYS_UNTYPED_RETYPE` (87), `SYS_CNODE_MOVE` (89),
`SYS_CNODE_FETCH` (90), `SYS_CSPACE_RESOLVE` (95).  Added: `SYS_CAP_IDENTIFY`
(117) and `SYS_CAP_SAME_OBJECT` (118), CSpace-native and strictly weaker than
what they replace.

Three structural zeros are the permanent gate (T095): handle-live,
handle-delivery and TOCTOU.  Any of them moving means a second namespace came
back.

Precondition: Stages 2–3 (both closed — no authority lives handle-only anymore).

- ~~Remove the value-range discrimination (<1024 / ≥1024).~~  ✅ the boundary
  is the handle TAG BIT, defined once in `nc/handle.h`; CPtrs own the low 31
  bits and a CPtr addresses exactly one capability (Etapa 6b).
- ~~Remove the bootstrap's handle producers (kernel_main dual insert).~~  ✅
  the bootstrap capability and every boot Untyped are published into CSpace
  ONLY; RBX carries 0 and a failed publish is fatal rather than "non-fatal
  because the legacy handle still works".
- Remove handle resolution from every dual resolver.
- Remove `SYS_CSPACE_RESOLVE` and `SYS_HANDLE_DUP` — the last two producers,
  both with no consumer outside `iris_test`.
- Remove the handle table when it has zero consumers; the `check_purity`
  allowlist must reach empty.

**Retired in Stage 4 so far** (numbers permanently reserved, `NOT_SUPPORTED`):
`SYS_IOPORT_RESTRICT` (43), `SYS_VMO_SHARE` (46), `SYS_HANDLE_INSERT` (59),
`SYS_UNTYPED_RETYPE` (87), `SYS_CNODE_FETCH` (90), plus the handle leg of IPC
delivery (`syscall_ipc_deliver_cap_badged`).  Every one was a handle PRODUCER
whose CSpace form already existed.

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
| after the VMO fixture migration | 111 |
| after retiring the cross-process producers and legacy retype | 104 |
| after retiring handle delivery in IPC | 99 |
| after the loader workspace (processes and VSpaces born in CSpace) | 85 |
| after VMO/initrd/self-VSpace destinations | 81 |
| after the liveness, identity and second-holder probes | 60 |

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

## Stage 5 — seL4-like bootstrap  ✅ CLOSED

Precondition: Stage 4 (the initial caps can only be CSpace now).
Design: `docs/architecture/stage5-root-task-bootinfo.md`.

- Replace the monolithic `KBootstrapCap` with structured BootInfo.
- Root task with: root CNode, initial TCB, initial VSpace, IRQ control cap,
  ASID/PCID control, Untyped list, fine-grained per-device caps.
- TCB_CONFIGURE/TCB_WRITE_REGS (execution of retyped TCBs) is defined here
  because its arguments (CSpace root, VSpace, fault EP) now exist as caps.

**Closing criterion**: the root task receives a structured BootInfo and
fine-grained capabilities, with no monolithic bootstrap object left to
restrict, and the executing TCB is a retyped object configured through
capabilities.

### Etapa 1 — the root task is told what it holds  ✅ DONE

The kernel writes a structured BootInfo region (`struct iris_root_bootinfo`,
`kernel/include/iris/root_bootinfo.h`) describing the initial capabilities by
CPtr, the shape of the root CNode, and every boot Untyped with its physical
region; it maps the region read-only / non-executable into the root task and
passes its address in RBX — the register that carried a bootstrap HANDLE until
Stage 4 deleted that namespace and left it carrying 0.

What retires is GUESSING.  The root task used to know its capabilities by
compile-time constants shared with the kernel (`BOOT_CPTR_BOOTSTRAP_CAP`,
`BOOT_CPTR_UNTYPED_START`) and count its untypeds by invoking slots until one
answered `NOT_FOUND`; its one liveness "probe" invoked a slot and ignored the
answer.  userboot now validates the description against the CSpace it describes
— every untyped must answer from its slot with the physical region the page
claims — and halts the boot with a serial diagnostic on disagreement.

The page is not authority (charter §3.5): it is read-only and every CPtr in it
names a slot the kernel had already populated.  What bounds it is the converse
rule — a grant that cannot be described is not made, so the untyped drain stops
where the description stops, and the region is two pages so that "describable"
covers every one of a 256-slot root CNode's 240 untyped slots (static-asserted).

Covered by RBI-1..RBI-10 (`tests/kernel/test_root_bootinfo.c`) for the builder,
and by the boot itself for the contract: an unreadable or untrue BootInfo is
fatal in userboot, so a healthy `make smoke-runtime` is the runtime witness.

### Etapa 2a — device control is its own authority  ✅ DONE

`IRIS_BOOTCAP_HW_ACCESS` — one bit authorising BOTH interrupt-line and I/O-port
capability creation, on an object that also carried spawn, debug and
framebuffer authority — is replaced by two capabilities the kernel matches
EXACTLY.  init printing a boot line to COM1 no longer holds the authority to
claim any IRQ, spawn processes and power the machine off.

Each is published into its own root-CNode slot, recorded in BootInfo v2, and
delegated down the chain as a CPtr source (so every grant is an MDB child of
the granter's slot and stays revocable).  svcmgr renounces hardware authority
by DELETING those two slots once it has claimed the catalog's devices —
previously a `SYS_BOOTCAP_RESTRICT` derive-then-delete whose first half was
load-bearing only because the authority was a bit on a shared object.

Covered by T296 (each control capability authorises its own syscall, neither
authorises the other's, the capability they were split from authorises
neither, an empty slot authorises nothing); T069 and T291 re-anchored.

### Etapa 2b — debug is its own authority  ✅ DONE

`IRIS_BOOTCAP_KDEBUG` — kernel-log drain, scheduler statistics, poweroff — is
now a capability of its own (`BOOT_CPTR_DEBUG_CONTROL`, BootInfo v3), matched
exactly and delegated to the two processes that use it: svcmgr and the suite.
The child-side slot reuses the retired `IRIS_CPTR_SVC_REPLY` constant, dead
since KChannel was removed, because root CNodes are 256 slots and the suite's
is full.  T296 gained a third leg; T291's oracle moved to the framebuffer bit.

### Etapa 2c — the monolith is gone  ✅ DONE

The last three authorities split: `SPAWN_SERVICE` became TWO capabilities
(process control and initrd control — one bit was authorising both spawning a
service and reading a boot image, which is why vfs, a file server, held the
authority to create processes), and `FRAMEBUFFER` became the framebuffer
control capability.

`SYS_BOOTCAP_RESTRICT` (45) is RETIRED with its number reserved, and the
monolith is unrepresentable rather than merely unused: `kbootcap_alloc` refuses
a zero or multi-bit kind, every kernel check is exact equality, and
`kbootcap_allows` / `kbootcap_clone_restricted` are deleted.  Slot 1
(`BOOT_CPTR_BOOTSTRAP_CAP`) stays reserved and permanently empty.

The loader API carries the split into userland: `svc_load_minted_ws` takes a
process capability and an initrd capability, so "can read images, cannot spawn"
is expressible in the signature.  T291 died with its mechanism (its subject was
the retired syscall); T148 pins 45; T296 covers what replaced it.  Suite:
269/269.

### Etapa 3 — the root task's own objects  ✅ DONE

The root task holds capabilities to its own root CNode and its initial thread
(`BOOT_CPTR_CNODE`, `BOOT_CPTR_TCB`, BootInfo v5), validated by userboot.  The
one process those objects belong to was the only one that could not name them:
the CNode was reachable structurally plus the `arg0 == 0` convention, the
thread only through `SYS_TCB_SELF`.

The self-capability makes the CSpace reachable from itself, so
`kprocess_teardown` empties the root CNode's slots before dropping its
references — a cycle cannot be collected by a refcount the cycle is holding up.
BC-11..BC-13 pin it, negative control included.  ASID/PCID control is
deliberately NOT added: no operation exists for it to authorise until VSpaces
are retyped from Untyped (Stage 6).

### Etapa 4 — a retyped TCB executes  ✅ DONE

`RETYPE2(KOBJ_TCB)` produced inactive threads from Fase S2 onward; what was
missing was not code but ARGUMENTS — a thread runs in a CSpace and a VSpace,
and neither was addressable as a capability until Stages 3–5.  Three CPtr-only
syscalls close it: `SYS_CSPACE_SELF` (119, a capability to the caller's own
root CNode — the CNode counterpart of `SYS_TCB_SELF`), `SYS_TCB_CONFIGURE`
(120) and `SYS_TCB_WRITE_REGS` (121).

`SYS_THREAD_CREATE` (48) is RETIRED with its number reserved: it carved a
thread from the kernel's static pool and returned a global thread id — no
capability authorised it, no Untyped paid for the storage, and the identity was
an array index (charter §3.4/§3.5).  Every in-tree thread is now retyped,
configured with capabilities, and started; creation returns a capability in a
slot.  `SYS_THREAD_START` (a spawned process's FIRST thread) remains the last
pool-born execution path and is Stage 7 work — a spawner cannot yet name its
child's CSpace and VSpace.

Two lifecycle defects surfaced and were fixed: the kernel stack was keyed by
the task's position in the static pool (a retyped TCB has none — it is recorded
per task now and keyed by the registry slot), and teardown released the
registry slot before freeing the stack, so a new thread could map its stack
over a range the dying one still unmapped afterwards.  Covered by T297 plus
every threaded test in the suite.

**Stage 5 closing criterion met**: the root task receives a structured BootInfo
and fine-grained capabilities, no monolithic bootstrap object remains to
restrict, and the executing TCB is a retyped object configured through
capabilities.

## Stage 6 — Remaining memory and objects  🔄 OPEN

Precondition: Stage 1 (ownership/derivation); may overlap with 5.
Design: `docs/architecture/stage6-memory-from-untyped.md`.

- Page-table objects retyped from Untyped (retires the paging_map PMM
  reserve).
- Canonical VSpace from Untyped; Frame headers inside the region.
- Retire the remaining object kslab paths (ledger list).
- Convert or retire KVMO; separate file-backed and anonymous memory in user
  services (the pager/VFS already provide the base).

**Closing criterion**: no kernel object and no page of user-visible memory is
created from kernel-private storage.  Stage 5 finished the authority story;
this stage answers *who pays for memory*, which is still "the kernel,
invisibly" in four places — page tables on map, frame headers, the VSpace and
its PML4, and sixteen `kslab_alloc` consumers.

### Etapa 3 — the address space itself comes from the budget  ✅ DONE

The PML4 and the KVSpace header follow the page tables into the Untyped: one
budget pays for a whole address space, and a spawn that names none builds
nothing.  A pooled PML4 is never returned to the PMM (the page belongs to the
Untyped), and teardown returns page children, then the header block, then the
pool retain — in that order, because the header block lives in the region the
pool owns.  The root task keeps the kernel-funded path: its address space is
built before any Untyped exists.

### Etapa 2 — page tables are charged to a budget  ✅ DONE

Mapping user memory needed page tables and took them from the kernel's PMM
reserve: unbudgeted, unauthorised, and drivable from ring 3 by mapping at
scattered addresses.  Every user address space now names the Untyped that pays
for its levels at `SYS_PROCESS_CREATE` — required, `RIGHT_WRITE`, retained by
the VSpace — and the carve fails rather than falling back to kernel memory.
Each table counts as a child of that Untyped, so `SYS_UNTYPED_RESET` cannot
reclaim a region whose pages are somebody's live page tables; the address space
returns them all at teardown.

Kernel mappings and the root task (built before any Untyped exists) stay
kernel-funded — bounded and stated, like the idle task.

Two pre-existing defects surfaced and were fixed: page-aligned carves aligned
the offset rather than the absolute address (a frame retyped from a sub-untyped
got a paddr the mapper masked DOWN, overlapping earlier carves), and a process
created but never started could not be reclaimed (kill found no threads and
dropped nothing, pinning its address space forever).  Covered by T299.

### Etapa 1 — the Untyped pays for its objects' headers  ✅ DONE

A frame retyped from an Untyped carved its PAGE from that Untyped and its
header from the kslab heap: the caller paid for the page, the kernel quietly
paid for the rest.  `KUntyped` now carves from **both ends** — page-aligned
regions from the bottom (`used`), object headers from the top (`used_top`) —
and a retyped frame's header is a child block of the same Untyped.

The direction is the point: a 160-byte header taken from the bottom would push
the next page-aligned carve onto the following page and cost almost 4 KiB per
frame.  From the top it costs its own size, and consecutive frames stay
page-dense.  The header is also, structurally, never inside the frame's own
page — that page is mapped into ring 3, where kernel bookkeeping would be
readable and writable by the process that received it.

`child_count` accounting is unchanged in shape (one child per frame, held by
the header block as for every other retyped family), and `SYS_UNTYPED_RESET`
reclaims both ends.  Frames with no Untyped to charge — VMO pages and a
spawning process's bootstrap frames — keep their kslab header and are what
Etapas 3 and 5 retire.

Covered by UT-TOP-1..5 (`tests/kernel/test_kuntyped.c`) and T298 (a frame costs
page + header out of one Untyped, four frames stay page-dense, and the frame
still maps, reads clean and writes).

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
