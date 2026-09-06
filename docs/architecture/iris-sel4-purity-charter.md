# IRIS — seL4 Purity Charter (constitutional, normative)

**Status**: IN FORCE since Phase S2 inc.2.
**Precedence**: this document prevails over every other document in the repo
(README, phase docs, comments) in case of conflict. It may only be amended in
a commit that cites it explicitly and updates the
[ledger](sel4-convergence-ledger.md) in the same change.
**Sibling documents**: the [convergence roadmap](sel4-convergence-roadmap.md)
orders the stages; the [ledger](sel4-convergence-ledger.md) records every
transitional mechanism and its retirement condition; the executable guard
`make check-purity` (`scripts/check_purity.sh`) freezes the existing legacy
consumers.

## 1. Official identity

> IRIS is a **pure capability-based microkernel, of its own implementation,
> in semantic convergence toward seL4/MCS**, with all non-essential services
> and policy outside the kernel.

Binding clarifications:

- "seL4-pure" refers to the **architectural and authority model** (typed
  objects born from Untyped, CSpace/CPtr, CDT, recursive revoke, absence of
  ambient authority, mechanism without policy) — not to seL4's ABI or code,
  which IRIS neither reuses nor promises to reproduce.
- IRIS does **not** claim to be formally verified. Its invariants are proven
  by construction plus adversarial tests, and must be stated that way.
- Every IRIS-specific extension must **preserve the capability-based purity**;
  a feature that violates it is not a feature, it is a design defect.
- The end goal is not an seL4 clone: it is a long-lived platform of its own,
  built on equivalent principles, able to grow (drivers, storage, networking,
  optional POSIX personality) **exclusively in user space**, without
  re-contaminating the kernel.
- The hybrid model (handle table + dual resolution) was **exclusively
  transitional** and is RETIRED as of Stage 4.  Reintroducing a second
  authority namespace is a design defect, not a feature.

## 2. Non-negotiable invariants

Each invariant is a review rule: a change that violates it is rejected citing
this charter. The "today" states are honest: `MET`, `PARTIAL` (debt recorded
in the ledger), or `PENDING` (a roadmap stage).

### 2.1 Authority

| # | Invariant | Today |
|---|---|---|
| A1 | Every sensitive operation requires a valid capability | MET |
| A2 | CSpace is the ONLY persistent authority namespace | **MET** — Stage 4: the handle table is DELETED (`HandleTable`, `KProcess.handle_table`, the implementation and its unit suite are gone).  There is one namespace |
| A3 | CPtr is the only capability identifier exposed productively | **MET** — Stage 4: the eight handle syscalls are retired to `NOT_SUPPORTED`, every creator requires a destination slot, and a CPtr addresses exactly one capability (leftover bits are `INVALID_ARG`) |
| A4 | No productive handles exist in the final state | **MET** — Stage 4: no handle is produced anywhere.  `iris_test` T095 pins handle-live, handle-delivery and TOCTOU at structural zero |
| A5 | No ambient authority exists | **MET (ledger A-18).**  The ioport whitelist this row used to name went in Stage 5 — the range a holder may claim travels on the capability — and every per-process quota went with the resource domain in Stage 7.  The LAST of it was three syscalls: `SYS_VSPACE_SELF`, `SYS_CSPACE_SELF` and `SYS_TCB_SELF` handed a thread capabilities to its own address space, CSpace and thread for the asking, requiring none.  All three are retired.  A service is given its own at `IRIS_CPTR_OWN_VSPACE`/`OWN_CSPACE`/`OWN_TCB` before its first instruction, the root task finds them in BootInfo (seL4's arrangement), and a thread the loader never saw is handed its own TCB by its creator in the entry register.  `mdb_legacy_roots` 32 → 23, because each of those calls also published a capability no revoke could reach |
| A6 | `ACCESS_DENIED` never falls back to another namespace | MET — vacuously, since Stage 4: there is no other namespace to fall back to.  A value that is not a CPtr is `INVALID_ARG` |
| A7 | Rights are only kept or reduced; mint never amplifies | MET (`rights_reduce`, collapse to NONE rejected) |
| A8 | Badges are kernel-sealed identity; a badged cap is never re-badged | MET |
| A9 | Every derived capability is traceable to its ancestor | **PARTIAL — measured.**  Phase S4/Stage 3 deleted the parallel handle-tree (`SYS_CAP_DERIVE`/`SYS_CAP_REVOKE` retired; derived-insert, revoke-children and the parent array removed), so there is exactly ONE derivation tree: the native CSpace MDB/CDT.  What the MET claim did not say is that the tree still has **LEGACY_ROOTs** — capabilities in a CSpace with no parent, which no revoke can reach because revoke walks descendants.  T305 measures it: **43 live roots of 335 MDB nodes** (max depth 6) at Stage 7 close.  The absolute number moves with what is alive; what T305 pins is that it does not GROW across a spawn/fault/kill or a mint/revoke cycle, which is the shape a new productive producer would have.  Two classes remain, both legitimate for now: the BOOT PATH (permanent — seL4's BootInfo caps are roots too) and KVmo publishes (a VMO is fabricated rather than retyped, so it has no ancestor to name; retires with the object, ledger D-5).  The third — FAULT DELIVERY — was a defect and is **FIXED**: the capability a fault publishes is a child of the TCB slot the registrant named when it armed the handler, verified by identity at delivery, exactly as an IPC-delivered capability is a child of the sender's source slot.  T305 pins the count against growth AND asserts that a live fault delivery adds no root |
| A10 | Revoke recursively removes all descendant authority, even cross-process | MET — `SYS_CSPACE_REVOKE` is the ONLY revoke: recursive, cross-CNode and cross-process (T288-T290 + model-based fuzzing).  The intra-table `SYS_CAP_REVOKE` is retired (Phase S4/Stage 3) |

### 2.2 Objects

| # | Invariant | Today |
|---|---|---|
| O1 | Every canonical object is born from Untyped via retype | **MET for creation; PARTIAL for form.**  EP/Notif/Reply/CNode/SC/TCB via RETYPE2 since Phase S1-S2.  **Stage 6** put every remaining object's storage in an Untyped as well, charged rather than user-retyped (ledger D-5).  **Stage 6-pure** converted the address-space objects to seL4's form outright: a PAGE TABLE and a VSPACE are retyped by the HOLDER (`IRIS_KOBJ_PAGE_TABLE`, `IRIS_KOBJ_VSPACE`) and installed or handed over explicitly, and `SYS_PROCESS_CREATE` takes a VSpace and a root CNode its caller made.  **Stage 7** retired `SYS_THREAD_START`, so no thread is carved from the static pool at all except the root and idle tasks, built before any Untyped exists, and **Stage 7-proc DELETED `KProcess`** — the object is not converted, it is gone.  What is still charged rather than retyped: VMO pages and mapping records (D-5, with the memory server) |
| O2 | The object's storage belongs to the Untyped that produced it | MET for the RETYPE2 family |
| O3 | The last capability does not destroy an object with active execution | MET — the scheduler holds its own execution reference |
| O4 | A terminated object stays observable while a valid cap exists | MET (TERMINATED TCB answers GET_INFO) |
| O5 | Storage is not reused until: execution ended ∧ active references released ∧ capabilities gone ∧ out of every internal registry ∧ reaper complete | MET (destructor = sole backing releaser) |
| O6 | Untyped reset/revoke respects descendance and lifecycle | MET (`child_count != 0 → BUSY`; generation as reuse witness) |

### 2.3 IPC

| # | Invariant | Today |
|---|---|---|
| I1 | Capability transfer uses CSpace as source and destination | MET — Phase S4/Stage 2 made the SOURCE a CPtr resolved to its slot (`syscall_ipc_stage_cap_peek_badged`; a handle source is `INVALID_ARG` with no fallback).  Stage 4 closed the DESTINATION half: handle materialization for a receiver that declared no slot is retired, so a receive without a declared destination gets the message and not the capability.  `iris_ipc_stat_handle_deliveries` is a structural 0, pinned by T095/T096 |
| I2 | A failed transfer leaves the state equivalent to before | MET (peek/commit staging, A1.9/A1.10) |
| I3 | The source cap is not consumed before a confirmed delivery | MET |
| I4 | Reply is one-shot | MET (explicit KReply; double REPLY → NOT_FOUND) |
| I5 | Sender identity is unforgeable | MET (sealed badge; reply forces badge 0) |
| I6 | Close, death, cancellation and rollback have deterministic semantics | MET (proven by lifecycle/stress/fuzzing) |
| I7 | IPC never silently degrades to handles | MET — Phase S4/Stage 2: the TOCTOU slot→handle fallback is REMOVED; a raced/occupied destination slot fails closed (no cap delivered, source untouched). `iris_ipc_stat_toctou_fallbacks` is a structural 0, guarded by T094/T095 |

### 2.4 Scheduling

| # | Invariant | Today |
|---|---|---|
| S1 | TCB and SchedulingContext are separate objects | MET |
| S2 | The TCB describes execution, not global process authority | **MET** — Stage 7-proc: there is no KProcess.  A "process" is threads configured with the same CSpace and the same VSpace, which is a fact about two capabilities rather than a third object |
| S3 | The SC represents a delegable time budget/policy | **MET** — Stage 8-mcs made the word *delegable* true.  Budget and period are enforced with **sporadic replenishment**, so the budget is a real per-period guarantee; **timeout faults** deliver an overrun to a temporal supervisor that decides; and **donation** lends the SC to a passive server across a Call, which is delegation of time as an authority rather than a property a thread is born with |
| S4 | SC bind/unbind are capability-gated | MET (`SYS_SC_BIND` by CPtr; `THREAD_SET_SC` FROZEN) |
| S5 | The kernel contains no service policy | MET (catalog/restart/manifests in svcmgr) |

### 2.5 Memory

| # | Invariant | Today |
|---|---|---|
| M1 | Frames, page tables, VSpace converge to creation from Untyped | **MET.**  Stage 6 carved all of them from an Untyped somebody named, each a child of it so the region cannot be reset under live objects.  **Stage 6-pure** closed the form as well for the paging objects: the kernel CREATES no page table, no PML4 and no CNode — the holder retypes each and installs it (`SYS_VSPACE_MAP_TABLE`), and a map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE` rather than allocating.  A task that maps therefore needs a budget of its own, which is an authority its spawner grants explicitly.  Kernel-funded after Stage 6-pure: the KERNEL's own address space, which has no holder to ask, and the root task's maps made BEFORE it runs — six levels, measured, ended by `kvspace_end_bootstrap` the moment it can speak for itself |
| M2 | Mapping authority comes from capabilities | MET (Frame/VSpace caps, RIGHT_MANAGE) |
| M3 | The kernel does not implicitly allocate user memory | MET (no kernel demand paging; ring-3 pager) |
| M4 | Every partial failure has an exact rollback | MET in RETYPE2/quotas; the general rule for every new path |
| M5 | Shared memory requires explicit delegation | MET (VMO share / file grants) |

### 2.6 Policy

| # | Invariant | Today |
|---|---|---|
| P1 | Discovery, restart, FS, pager, drivers, service quotas and manifests live in user space | MET |
| P2 | The kernel implements mechanism, not product policy | PARTIAL — and the cause this row named is GONE: the ioport whitelist was removed in Stage 5 (the range a holder may claim travels on the capability), and the per-process quotas went with the resource domain in Stage 7.  What keeps it partial has not been re-audited since, so the row says PARTIAL because nobody has looked, not because something is known to be wrong |
| P3 | A hardcoded whitelist is tolerated only as temporary bootstrap with a ledger entry | MET (entry added) |

## 3. Permanent prohibitions

Prohibited from now on, with no exception and no "temporarily":

1. Add **new handle producers** (no new productive syscall returns handles;
   no new canonical object is inserted into the handle table).
2. Add **new handle consumers** (no new productive path calls
   `handle_table_get_object` or the dual resolver; enforced by
   `make check-purity`).
3. Create canonical objects directly from **kslab** (enforced by
   `check_purity`; the closed list of bootstrap uses is in the ledger).
4. Add **global identifiers that confer authority**.
5. Use a **PID, index, address or pointer** as a substitute for a capability.
6. Introduce syscalls that accept authority through **two namespaces** (the
   existing dual resolvers are frozen legacy, not a pattern to imitate).
7. Add **CPtr-to-handle fallbacks**. As of Phase S4 (Stage 2) there is NO
   exception: the receive-slot TOCTOU fallback — the last one — is removed and
   its counter is pinned at 0 by T094/T095.
8. Trust **service names** as authority (names are discovery; authority is the
   delivered cap).
9. Add **restart, filesystem or driver policy to the kernel**.
10. Declare a **migration finished** while the productive path still depends on
    the prior mechanism.

The legacy-consumer allowlist (`scripts/purity_allowlist.txt`) may only
**shrink**. Growing it requires amending this charter and the ledger in the
same commit, with a written technical justification.

## 4. Mandatory end state of the capability model

The capability model is declared COMPLETE only when all of this is true and
proven:

- [x] Native CDT/MDB tied to CNode slots (global parent/child) — **Phase S3**
      (`docs/architecture/cspace-cdt-mdb.md`); recursive cross-process revoke
      included. The parallel handle-tree is RETIRED (Phase S4/Stage 3): there
      is exactly one derivation tree in the system.
- [x] Recursive cross-process revoke with deterministic rollback/cleanup —
      **Phase S3** (`SYS_CSPACE_REVOKE`).
- [x] CPtr-based cap transfer (source and destination in CSpace) — **Phase S4**
      (Stage 2): source resolved to a CSpace slot, destination the receive
      slot, and the delivered cap installed as an MDB **child of the source
      slot** (real ancestry, no LEGACY_ROOT from IPC).
- [x] derive/mint/copy/move/delete/revoke operating on slots — **Phase S3**
      (`kcnode_slot_*` primitives); `SYS_CSPACE_MINT`/`MINT_INTO`/`REVOKE`.
- [x] CSpace-only invocation: zero dual resolution, zero value-range
      discrimination — **Stage 4**.  The resolvers have one leg; the split
      that remains is "CPtr or malformed".
- [x] Zero productive handles; handle table removed — **Stage 4**.  Not
      reduced to zero consumers: deleted.
- [x] Bootstrap with fine-grained capabilities (structured BootInfo; no
      monolithic `KBootstrapCap`) — **Stage 5**.  The root task reads a
      structured BootInfo region describing every capability the kernel
      installed in its CSpace (including its own root CNode and thread), and
      boot authority is six capabilities of one authority each.  The monolith
      is unrepresentable, not merely unused.
- [x] All canonical objects born from Untyped (including the executing TCB,
      page tables, VSpace, Frame headers) — **Stage 7-mem/7-proc**.  The two
      objects that were still CHARGED rather than retyped are gone rather than
      converted: `KVmo`'s owner relation and quota retired with the per-process
      resource domain (a VMO's memory comes from an Untyped the caller names),
      and `struct KProcess` is DELETED.  What remains kernel-funded is the
      boot path — the root task's CSpace and address space, built before
      anything exists that could name them — which the ledger records as not
      retiring with the process server.
- [x] No authority object identified by PID or global index — **Stage 7
      Step 7**.  `SYS_EXCEPTION_RESUME` stopped taking a task id, the global
      thread lookup (`task_find_by_id`) is deleted, and the kernel contains
      zero `->id ==` comparisons: nothing selects an object by number.  The
      ids that survive (`task.id`, the fault record's `task_id`,
      `SYS_GETPID`) are diagnostics that confer nothing.
- [x] Adversarial lifecycle and revocation suite (creation, cross death,
      chained revocation, storage reuse, stale caps) as a permanent gate —
      278 runtime tests including model-based syscall fuzzing, 18740 host
      assertions, and `check_purity` as a hard gate on every build.

## 5. Governing priority

```text
lifecycle correctness
→ authority purity
→ atomicity
→ isolation
→ mechanism/policy separation
→ extensibility
→ performance
→ system features
```

No new feature justifies keeping a structural deviation. Any semantic
divergence from seL4 must be: (1) documented, (2) technically justified,
(3) isolated, (4) covered by tests, and (5) marked as temporary or
deliberate — in the ledger if temporary, in this charter if deliberate.

This rule covers divergences found by COMPARING the two architectures, not only
those left behind by a migration.  Four such were unrecorded until Stage 5
closed and are now in the ledger's "Structural divergences from seL4" section:
per-thread kernel stacks with in-kernel blocking (D-1), CNodes without guards
(D-2), the rights set (D-3), and the absent per-thread IPC buffer (D-4).  D-1
and D-2 carry no retirement stage, which is stated rather than implied: an
entry with no stage is honest, an unrecorded divergence is not.

### 5.1 Revisit triggers must fire

A divergence marked "revisable in Stage N" is a PROMISE, and a promise with no
enforcement is a wish.  Three of them — reply-with-DUPLICATE (Stage 1), the
IPC buffer (Stage 6) and Untyped RESET (Stage 1) — pointed at stages that
closed without anyone returning to the row.  Two were then answered in the
affirmative (kept, deliberately) and one turned out to be real debt that had
been sitting unscheduled since Stage 6.

The rule from now on: **a stage cannot be marked CLOSED while any charter §6
row or ledger entry names it as a revisit trigger.**  Closing a stage includes
walking those rows and writing the answer — kept, retired, or rescheduled with
a new trigger.  An unanswered trigger is the same class of dishonesty as an
unrecorded divergence.

## 6. Registered deliberate divergences

| Divergence | Justification | Status |
|---|---|---|
| No formal verification | out of the project's scope; offset by adversarial gates | Permanent, deliberate |
| Own ABI (not seL4) | IRIS does not seek binary compatibility.  Concretely: 68 live numbered syscalls out of 127 numbers (0-126), the other 59 retired-and-reserved or never assigned, each taking CPtrs and checking rights itself — where seL4 has a handful of syscalls and expresses every other operation as an INVOCATION on a capability carrying a method label.  The authority semantics are equivalent (nothing is reachable without naming a capability); the shape is not, and no amount of convergence work changes it | Permanent, deliberate |
| Rights set is IRIS's own (`READ/WRITE/DUPLICATE/TRANSFER/WAIT/ROUTE/MANAGE`) | seL4 has `Read/Write/Grant/GrantReply` and does NOT treat copyability as a right — whether a capability can be copied follows from its type and the derivation tree.  IRIS gates minting with `RIGHT_DUPLICATE` and IPC transfer with `RIGHT_TRANSFER`, which is what makes a delegation non-re-delegable today.  The two models are not translatable one-to-one, so "IRIS has seL4's rights" is never a correct statement | Deliberate, revisable — ledger D-3 |
| No per-thread IPC buffer object | A message carries four inline words plus a bulk payload staged in the kernel (`task.ipc_kbuf`, 256 B) and copied to/from a user pointer named per call; seL4 registers an IPC buffer FRAME per TCB.  The bulk size is therefore a kernel constant rather than a frame the user chose and paid for | **Condition FIRED, scheduled.**  Its revisit trigger was Stage 6 (frames from Untyped), which closed — and the trigger was never acted on, which is the process failure this row now records.  Retirement is scheduled: **Stage 8-cap**.  Until then, 256 B of kernel memory per TCB that the user did not choose and did not pay for — ledger D-4 |
| Separate `SYS_REPLY` (no combined ReplyRecv) | simplicity of the current synchronous path; revisit in Stage 8 (MCS) | Deliberate, revisable |
| Reply objects with DUPLICATE (supervisor mints them into the child) | IRIS supervision pattern; documented in RETYPE2 | **Deliberate, KEPT.**  Its revisit condition (Stage 1, CDT) fired and was answered: the MDB makes the minted reply capability a traceable child of the supervisor's, so the supervision pattern costs nothing the derivation tree cannot express or revoke.  No further review scheduled |
| Untyped RESET (bump reset with child_count==0) in addition to revoke | useful as a reuse primitive; real revoke arrives with the CDT | **Temporary → KEPT, deliberate.**  Its condition (Stage 1) fired.  Real revoke exists (`SYS_CSPACE_REVOKE`) and RESET was not removed, because the two answer different questions: revoke destroys a subtree of CAPABILITIES, RESET rewinds a bump allocator whose children are already gone.  seL4 expresses the second by revoking the Untyped, which IRIS also supports; RESET is the cheap path and is gated on `child_count == 0`, so it can never destroy anything.  Kept as an addition, not a substitute |
