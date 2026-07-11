# Fase 18 — untyped / retype / revoke hardening

Status: ACCEPTED — implemented in this phase.  Companion to
`lifecycle-hardening.md` (Fase 16), `scheduler-hardening.md` (Fase 17) and
`a1-authority-namespace-endgame.md`.  Fase 18 audits the memory-authority
surface — `KUntyped → retype → derived caps → revoke` — end to end and locks its
contracts with runtime tests T125–T131.

**No new authority bug was found.**  The audit confirmed retype is atomic at the
object level, rights derivation is monotonic, and revoke tears down exactly its
handle-derivation subtree.  This phase makes the guarantees observable (additive
live per-type object counts) and regression-proof, and it writes down — honestly
— the *scope* of `SYS_CAP_REVOKE` and the one design gap (ring-3 frame mapping).
The runtime selftest suite rises from 120/120 to 127/127.

---

## The KUntyped model

A `KUntyped` (`kernel/new_core/src/kuntyped.c`) is a capability over a physical
region `[phys_base, phys_base + total_size)` removed from the PMM.  It is a
**bump allocator**, not a free-list:

- `used` advances monotonically as objects are carved; it never shrinks except
  via `SYS_UNTYPED_RESET`, which zeroes it only when `child_count == 0`.
- `child_count` counts live objects/sub-untypeds carved from the region.  Every
  typed child retains a reference on its parent untyped and increments
  `child_count`; the child's destructor releases that reference and decrements
  it.  This is the authority that keeps a region pinned while any child lives,
  and the gate that makes `SYS_UNTYPED_RESET` safe.

`SYS_UNTYPED_RESET` returning `0` (vs `IRIS_ERR_BUSY`) is therefore a hard,
observable proof that **every** object carved from a region has been destroyed —
the primary "authority died" signal the tests rely on.

## Retype

`SYS_UNTYPED_RETYPE(ut_cptr, obj_type, obj_arg)` resolves the untyped with
`RIGHT_WRITE` (CSpace-first, handle fallback; `ACCESS_DENIED` is a hard stop —
no fallback) and carves one object:

| obj_type | obj_arg | backing |
|----------|---------|---------|
| `KOBJ_ENDPOINT` | — | `kuntyped_alloc_child` (header + object) |
| `KOBJ_NOTIFICATION` | — | `kuntyped_alloc_child` |
| `KOBJ_CNODE` | num_slots (pow2, ≤ max) | `kuntyped_alloc_child` |
| `KOBJ_SCHED_CONTEXT` | — | `kuntyped_alloc_child` |
| `KOBJ_UNTYPED` | size (≥4096, page-mult) | page-aligned sub-region + fresh `KUntyped` header |
| `KOBJ_FRAME` | size (≥4096, page-mult) | page-aligned physical region + slab `KFrame` header |

`KOBJ_PROCESS` / `KOBJ_TCB` / `KOBJ_VSPACE` / `KOBJ_REPLY` are **not**
retypeable from untyped (they are created by process/thread lifecycle); an
unsupported type returns `IRIS_ERR_NOT_SUPPORTED`.

The new object is installed as a **fresh handle-table entry** (a derivation
*root* — `derivation_parent = HANDLE_INVALID`).  The untyped→child relationship
is tracked only by `child_count`, not by the handle-derivation tree.

### Retype failure atomicity

Every validation failure (bad type, bad size, non-pow2 CNode, insufficient
rights, no space) returns before any object is constructed or any handle is
inserted — **no partial object is ever visible** (U5).  The one documented
non-atomicity is memory-only, never object-visibility: for `KOBJ_FRAME`, if the
`KFrame` header slab-allocation fails *after* the page-aligned physical region
was carved, the bump pointer cannot rewind — the region is wasted until the next
`SYS_UNTYPED_RESET` (which is still correctly gated on `child_count == 0`, which
is unchanged by the failure).  No object, handle, or reference leaks; only
reclaimable bump space is lost.

## Derived caps and rights

Two derivation mechanisms exist, in two namespaces:

1. **Handle-table derivation** — `SYS_CAP_DERIVE(src_h, new_rights)` requires
   `RIGHT_DUPLICATE` on the source, reduces rights via `rights_reduce` (never
   amplifies — U7), and records `derivation_parent = src_h` on the new slot.
2. **CSpace CNode mint** — `SYS_CNODE_MINT` / `SYS_PROC_CSPACE_MINT` install a
   cap into a CNode slot with its own independent object references, again
   rights-reduced. CNode slots are **not** part of the handle-derivation tree.

`rights_reduce(base, requested)` returns `base & requested` (or `base` for
`RIGHT_SAME_RIGHTS`); a result of `RIGHT_NONE` is rejected `INVALID_ARG`.  There
is no path that grants a right the parent lacks, and no fallback after
`ACCESS_DENIED` (U20).

## Revoke

`SYS_CAP_REVOKE(h)` validates `h`, then `handle_table_revoke_children` runs a BFS
over `derivation_parent[]` and closes **every handle transitively derived from
`h`** — `h` itself survives.  Each closed slot runs the full
`handle_entry_reset` (active-release → object `close`; lifetime-release →
`destroy` when the last reference drops) and bumps the slot generation, so a
revoked handle fails `IRIS_ERR_BAD_HANDLE`, never phantom authority.

### Scope of revoke (documented, tested)

`SYS_CAP_REVOKE` is **handle-derivation-scoped**.  It does **not**:

- touch CSpace CNode slots — a cap copied/minted into a CNode is an independent
  reference and survives revoke (T127 asserts this and then reclaims it);
- destroy retyped objects via the untyped `child_count` link — retyped children
  are derivation roots, so revoking the untyped's handle does not reach them;
- force-unmap frames — see below.

This is a coherent per-handle authority model: an object lives as long as *any*
reference (derived handle, CNode slot, mapping, or blocked-IPC ref) holds it;
revoke collapses one derivation subtree, and the object is destroyed only when
its **last** reference drops.

Generation is 22 bits (`HANDLE_GEN_MAX`), so the derivation-parent match is
robust against slot reuse for 4 194 303 reuses of a slot before a theoretical
ABA — a bounded, documented limit, not a live risk.

## Relationship to CSpace

CNode `close`/`destroy` releases every slot's object reference. A revoked or
deleted CNode slot releases exactly one reference. `kcnode_mint_excl_badged`
refuses an occupied slot (`IRIS_ERR_ALREADY_EXISTS`, U4). Retype of a CNode
validates a power-of-two, in-range slot count before allocation.

## Relationship to VSpace / frame mappings

A `KFrame` carries `mapped_count`. Each `kframe_map_page` retains the frame for
the life of the mapping record; `kframe_unmap_page` decrements first, then
releases. `kframe_obj_destroy` **asserts `mapped_count == 0`** — the model
guarantees a frame object can never be destroyed while a PTE references it, so a
stale mapping can never outlive its frame.

Because revoke is cap-scoped, it never force-unmaps: a mapping holds an
independent reference, so revoking (or closing) a frame's cap while it is mapped
drops only that cap; the frame stays alive until the mapping is unmapped (or the
VSpace is torn down at process teardown, which unmaps all).

**Documented gap:** ring 3 cannot MAP a retyped frame in the iris_test process —
`SYS_FRAME_MAP` requires a VSpace capability by CPtr that iris_test is not
granted (VMO is the ring-3 mapping path). So T128 exercises the frame's *cap*
lifetime (retype → derive → revoke → release → `frame_live` baseline + region
reset) rather than a live PTE. The PTE-install / `mapped_count` / unmap
invariants remain covered by the host `KFrame` suites (`FR: KFrame capability
model`, `FR: KFrame stress / invariant audit`), which run under `make test-unit`.

## Relationship to IPC-visible objects

Retyped endpoints/notifications are ordinary objects: a blocked EP_RECV waiter
holds a **refcount** ref (not an active-ref), so closing the last handle fires
`kendpoint_obj_close`, which wakes the waiter with `IRIS_ERR_CLOSED` and leaves
no dead waiter (T129). Revoking a derived handle of such an endpoint drops one
active-ref without disturbing the waiter; the object dies only when the last
handle closes and the waiter releases its ref.

## Instrumentation (additive, ABI-safe)

Fase 18 adds a fourth `SYS_SCHED_INFO` tier, written only when the caller passes
`buf_size >= 136` (a caller passing 112..135 gets the exact 112-byte Fase-17
snapshot — same additive rule as every prior tier):

```text
offset 112: uint32_t untyped_live       KUntyped objects live
offset 116: uint32_t frame_live         KFrame objects live
offset 120: uint32_t endpoint_live      KEndpoint objects live
offset 124: uint32_t notification_live  KNotification objects live
offset 128: uint32_t cnode_live         KCNode objects live
offset 132: uint32_t _pad1
```

These per-type live counts, plus the existing handle-live word and the
`SYS_UNTYPED_RESET` gate, are the observables that prove objects are born and
destroyed exactly once.

## Authority audit matrix

| Area | Current behavior | Invariant | Coverage | Risk |
|------|------------------|-----------|----------|------|
| Retype authority | resolves untyped `RIGHT_WRITE`, `ACCESS_DENIED` no-fallback | U1, U20 | T125, T130 | none |
| Retype range/size | pow2 CNode, ≥4096 page-mult frame/sub-untyped | U2, U3 | T125, T126 | none |
| Retype wrong type | unsupported → `NOT_SUPPORTED` | U3 | T125, T126, T131 | none |
| Retype failure atomicity | validate-before-construct; no partial object | U5 | T126, T131 | frame header-alloc bump-waste (memory-only, RESET-reclaimed) |
| Object ancestry | `child_count` + parent retain per child | U6 | T125 (RESET) | none |
| Rights derivation | `rights_reduce`, needs `RIGHT_DUPLICATE` | U7 | T130 | none |
| Revoke cascade | BFS over `derivation_parent[]` | U8 | T127, T131 | none |
| Revoke idempotence | empty subtree → 0; gen-bumped slots | U9 | T127, T131 | none |
| Revoke scope | subtree only; CNode/mappings independent | U10, U16 | T127 | scope documented, not a bug |
| Revoke + mapped frame | cap-scoped; `mapped_count==0` destroy assert | U11 (as gap) | T128 + host KFrame | ring-3 map gap documented |
| Stale caps | slot gen bump → `BAD_HANDLE` | U12 | T127, T131 | none |
| Refs without owner | last-ref `destroy`; live counts baseline | U13, U17 | all T125–T131 | none |
| Revoke + IPC waiters | close wakes `CLOSED`, no dead waiter | U14 | T129 | none |
| Ghost KReply | no rendezvous → no reply cap | U15 | T129 | none |
| Object free once | refcount `destroy` at 0 | U17 | T125–T131 | none |
| Memory accounting | RESET gate + live counts baseline | U18 | T125–T131 | none |
| Error-path refs | every failure drops resolve refs | U19 | T126, T131 | none |

## Invariants (U1–U20)

U1 retype needs authority; U2 objects stay within the region; U3 size/alignment
validated; U4 no overwrite of an occupied CNode slot; U5 retype failure atomic
(no partial object visible); U6 every child has a traceable ancestor
(`child_count`); U7 derived rights monotonic; U8 revoke removes all reachable
descendants; U9 revoke idempotent on an empty subtree; U10 revoke stays within
its subtree; U11 mapped-frame teardown correct *(here: cap-scoped + destroy
assert; ring-3 map gap documented)*; U12 no stale usable caps; U13 no live ref
without an owner; U14 no hung endpoint waiters; U15 no phantom KReply/caps; U16
shared objects outside the subtree survive; U17 object freed exactly once; U18
memory accounting returns to baseline; U19 error paths leak no refs; U20 no
authority fallback after `ACCESS_DENIED`.

## Tests T125–T131

| Test | Scenario | Invariants | Failure paths |
|------|----------|-----------|---------------|
| T125 | retype endpoint/notif/cnode/sc/frame/sub-untyped, prove usable, destroy, RESET | U1,U2,U3,U6,U17,U18,U20 | wrong type, bad size, bad CNode slots, missing `RIGHT_WRITE` |
| T126 | drive each retype failure mode; assert atomic + valid-after-fail | U5,U17,U18,U19 | nomem, bad type, bad size, bad slots |
| T127 | derivation tree (root→child→grandchild+branch) revoke cascade; CNode copy scope | U8,U9,U10,U16 | stale revoke, empty-subtree revoke |
| T128 | frame cap lifetime: derive → revoke → release → baseline (map gap documented) | U10,U13,U17,U18 | — |
| T129 | worker blocked in EP_RECV on a retyped endpoint; derive+revoke; close wakes CLOSED | U8,U14,U15,U17 | wrong wake error, ghost KReply |
| T130 | rights monotonicity across derive + CNode mint; no amplification | U7,U20,U16 | escalation via derive, mint amplify |
| T131 | fixed-seed retype/derive/mint/revoke/delete stress + forced failures | U4,U5,U8,U9,U17,U18,U19 | bad type, bad size, stale revoke |

## Boot-chain wiring

The ring-3 suite needs an untyped it owns. One boot `KUntyped` is forwarded
down the boot chain via additive CSpace mints: kernel_main hands the boot
untypeds to userboot (`BOOT_CPTR_UNTYPED_START`); userboot mints one into init
(`IRIS_CPTR_INIT_UNTYPED`, slot 12); init mints it on into iris_test
(`IRIS_CPTR_TEST_UNTYPED`, slot 55) with full rights.  No new legacy handle
producer, no ABI change — just two `svc_mint` entries.

## Limits and remaining gaps

- Ring-3 frame **mapping** is not exercisable in iris_test (no VSpace cap); the
  PTE path stays under host-test coverage. Granting a self-VSpace cap to a
  spawned process would let a future test drive the live map/unmap/revoke PTE
  interaction from ring 3.
- Revoke is handle-derivation-scoped. There is no single "revoke this object
  everywhere" primitive that also sweeps CNode slots and mappings; teardown of
  those references is explicit (delete / unmap) or happens at process teardown.
- The `KOBJ_FRAME` header-alloc bump-waste is reclaimable only at
  `SYS_UNTYPED_RESET`, not incrementally.
- Generation ABA is bounded at 2²² slot reuses (documented, not mitigated
  further).

## Validation

`make`, `make test-unit` (10200/10200), `make smoke-runtime` (127/127),
`make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests` (127/127) — all green,
no flakes across repeated runs.
