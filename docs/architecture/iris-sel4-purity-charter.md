# IRIS — seL4 Purity Charter (constitutional, normative)

**Status**: IN FORCE since Fase S2 inc.2.
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
- The current hybrid model (handle table + dual resolution) is
  **exclusively transitional** and doomed to retirement (roadmap Stages 1–4).
  No future decision may consolidate it.

## 2. Non-negotiable invariants

Each invariant is a review rule: a change that violates it is rejected citing
this charter. The "today" states are honest: `MET`, `PARTIAL` (debt recorded
in the ledger), or `PENDING` (a roadmap stage).

### 2.1 Authority

| # | Invariant | Today |
|---|---|---|
| A1 | Every sensitive operation requires a valid capability | MET |
| A2 | CSpace is the ONLY persistent authority namespace | PARTIAL — handle table still live (Stages 2–4) |
| A3 | CPtr is the only capability identifier exposed productively | PARTIAL — same |
| A4 | No productive handles exist in the final state | PENDING (Stage 4) |
| A5 | No ambient authority exists | PARTIAL — ioport whitelist, kernel quotas (ledger) |
| A6 | `ACCESS_DENIED` never falls back to another namespace | MET (<1024/≥1024 split with no fallback) |
| A7 | Rights are only kept or reduced; mint never amplifies | MET (`rights_reduce`, collapse to NONE rejected) |
| A8 | Badges are kernel-sealed identity; a badged cap is never re-badged | MET |
| A9 | Every derived capability is traceable to its ancestor | MET for CSpace derivation (native MDB/CDT, Fase S3); the legacy handle-tree (`SYS_CAP_DERIVE`) still runs in parallel, frozen (Stage 3) |
| A10 | Revoke recursively removes all descendant authority, even cross-process | MET for CSpace caps (`SYS_CSPACE_REVOKE`, Fase S3 — crosses CNodes and processes, proven by T288-T290 + fuzzing); `SYS_CAP_REVOKE` handle-only is still intra-table (Stage 3) |

### 2.2 Objects

| # | Invariant | Today |
|---|---|---|
| O1 | Every canonical object is born from Untyped via retype | PARTIAL — EP/Notif/Reply/CNode/SC/TCB via RETYPE2; TCB execution, Frame header, VSpace, page tables still pending (Stages 0/6) |
| O2 | The object's storage belongs to the Untyped that produced it | MET for the RETYPE2 family |
| O3 | The last capability does not destroy an object with active execution | MET — the scheduler holds its own execution reference |
| O4 | A terminated object stays observable while a valid cap exists | MET (TERMINATED TCB answers GET_INFO) |
| O5 | Storage is not reused until: execution ended ∧ active references released ∧ capabilities gone ∧ out of every internal registry ∧ reaper complete | MET (destructor = sole backing releaser) |
| O6 | Untyped reset/revoke respects descendance and lifecycle | MET (`child_count != 0 → BUSY`; generation as reuse witness) |

### 2.3 IPC

| # | Invariant | Today |
|---|---|---|
| I1 | Capability transfer uses CSpace as source and destination | PARTIAL — destination yes (receive slots); source still handle (Stage 2) |
| I2 | A failed transfer leaves the state equivalent to before | MET (peek/commit staging, A1.9/A1.10) |
| I3 | The source cap is not consumed before a confirmed delivery | MET |
| I4 | Reply is one-shot | MET (explicit KReply; double REPLY → NOT_FOUND) |
| I5 | Sender identity is unforgeable | MET (sealed badge; reply forces badge 0) |
| I6 | Close, death, cancellation and rollback have deterministic semantics | MET (proven by lifecycle/stress/fuzzing) |
| I7 | IPC never silently degrades to handles | PARTIAL — the TOCTOU slot→handle fallback exists, is counted (`iris_ipc_stat_toctou_fallbacks`) and retires in Stage 2 |

### 2.4 Scheduling

| # | Invariant | Today |
|---|---|---|
| S1 | TCB and SchedulingContext are separate objects | MET |
| S2 | The TCB describes execution, not global process authority | MET (KProcess separate, doomed — Stage 7) |
| S3 | The SC represents a delegable time budget/policy | MET (budget/period; donation pending — Stage 8) |
| S4 | SC bind/unbind are capability-gated | MET (`SYS_SC_BIND` by CPtr; `THREAD_SET_SC` FROZEN) |
| S5 | The kernel contains no service policy | MET (catalog/restart/manifests in svcmgr) |

### 2.5 Memory

| # | Invariant | Today |
|---|---|---|
| M1 | Frames, page tables, VSpace converge to creation from Untyped | PENDING (Stage 6; sidecar headers in the ledger) |
| M2 | Mapping authority comes from capabilities | MET (Frame/VSpace caps, RIGHT_MANAGE) |
| M3 | The kernel does not implicitly allocate user memory | MET (no kernel demand paging; ring-3 pager) |
| M4 | Every partial failure has an exact rollback | MET in RETYPE2/quotas; the general rule for every new path |
| M5 | Shared memory requires explicit delegation | MET (VMO share / file grants) |

### 2.6 Policy

| # | Invariant | Today |
|---|---|---|
| P1 | Discovery, restart, FS, pager, drivers, service quotas and manifests live in user space | MET |
| P2 | The kernel implements mechanism, not product policy | PARTIAL — per-process quotas and ioport whitelist in the kernel (ledger; Stages 6/7) |
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
7. Add **CPtr-to-handle fallbacks** (the receive-slot TOCTOU fallback is the
   only exception, counted and doomed — Stage 2).
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

- [x] Native CDT/MDB tied to CNode slots (global parent/child) — **Fase S3**
      (`docs/architecture/cspace-cdt-mdb.md`); recursive cross-process revoke
      included. The parallel handle-tree still needs retiring.
- [x] Recursive cross-process revoke with deterministic rollback/cleanup —
      **Fase S3** (`SYS_CSPACE_REVOKE`).
- [ ] CPtr-based cap transfer (source and destination in CSpace) — destination
      CSpace (receive slots) already; SOURCE still handle (Stage 2).
- [x] derive/mint/copy/move/delete/revoke operating on slots — **Fase S3**
      (`kcnode_slot_*` primitives); `SYS_CSPACE_MINT`/`MINT_INTO`/`REVOKE`.
- [ ] CSpace-only invocation: zero dual resolution, zero value-range
      discrimination.
- [ ] Zero productive handles; handle table removed or reduced to zero
      consumers.
- [ ] Bootstrap with fine-grained capabilities (structured BootInfo; no
      monolithic `KBootstrapCap`).
- [ ] All canonical objects born from Untyped (including the executing TCB,
      page tables, VSpace, Frame headers).
- [ ] No authority object identified by PID or global index.
- [ ] Adversarial lifecycle and revocation suite (creation, cross death,
      chained revocation, storage reuse, stale caps) as a permanent gate.

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

## 6. Registered deliberate divergences

| Divergence | Justification | Status |
|---|---|---|
| No formal verification | out of the project's scope; offset by adversarial gates | Permanent, deliberate |
| Own ABI (not seL4) | IRIS does not seek binary compatibility | Permanent, deliberate |
| Separate `SYS_REPLY` (no combined ReplyRecv) | simplicity of the current synchronous path; revisit in Stage 8 (MCS) | Deliberate, revisable |
| Reply objects with DUPLICATE (supervisor mints them into the child) | IRIS supervision pattern; documented in RETYPE2 | Deliberate, revisable in Stage 1 (CDT) |
| Untyped RESET (bump reset with child_count==0) in addition to revoke | useful as a reuse primitive; real revoke arrives with the CDT | Temporary until Stage 1, then revisable |
