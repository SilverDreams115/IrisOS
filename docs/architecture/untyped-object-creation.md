# IRIS — Untyped Object Creation (Fase S1, normative)

Complements [`sel4-canonical-object-model.md`](sel4-canonical-object-model.md)
and supersedes `untyped-retype-revoke-hardening.md` (Fase 18) as the contract
of the allocation substrate.

## KUntyped — identity and layout

```
struct KUntyped {
    KObject   base;         /* header first */
    lock;                   /* IRQ-off spinlock: bump/reset */
    phys_base, total_size;  /* EXACT physical range, no overlap (U1/U2) */
    used;                   /* bump offset — grows only (U6) except on RESET */
    child_count;            /* live objects/sub-untypeds inside (U10) */
    is_device;              /* device: no zero-fill, restricted types */
    alloc_parent;           /* sub-untyped → parent (bookkeeping) */
    generation;             /* +1 per successful RESET — reuse witness */
}
```

- Size: boot Untypeds are buddy blocks (powers of two); sub-untypeds use the
  explicit "page-multiple bytes" contract.
- Allocation state: a **monotonic watermark** (`used`). Chosen over a
  bitmap/tree for determinism, trivial atomicity, auditability (`used_bytes`
  observable) and because it preserves no-overlap structurally: a carved range
  is never re-issued before RESET, and RESET requires `child_count == 0`. It is
  not a general allocator: it is a retype operation over an explicit authority
  region.
- Derivation: a `KOBJ_UNTYPED` retype creates sub-untypeds with a back-pointer
  to the parent; the parent cannot RESET while a child is alive (descendants).

## Invariants U1–U15

```
U1  a physical region belongs to a single root Untyped (boot drain: disjoint
    buddy blocks; sub-untypeds carved exclusively from the parent)
U2  two live Untypeds do not overlap (exclusive carve + no unbump)
U3  a retyped object resides entirely inside the source Untyped
    (block = header+payload carved from the range; T252)
U4  the object respects its type's size/alignment (per-type validation;
    alignment asserts ≤ KUNTYPED_ALIGN; physical types: page)
U5  a region does not back two live objects (watermark; T253/T259)
U6  retype only reduces capacity (monotonic bump)
U7  deriving caps (resolve/derive/mint) consumes no physical memory (T262)
U8  deleting a cap does not destroy the object if caps remain (T255)
U9  revoke removes authority descendants (handle-table tree TODAY;
    CSpace CDT = S2/S3, ledger)
U10 the region is reusable only with no objects or caps retaining it
    (child_count gate on RESET; T259)
U11 device Untyped produces only UNTYPED/FRAME
U12 normal Untyped produces no device authority
U13 overflow/invalid ranges fail before mutating state (full validation
    first; T254)
U14 batch retype is atomic (single carve under lock + verified publication;
    T253)
U15 a partial failure consumes no memory (exact rollback kuntyped_unbump_exact
    + destroy of unpublished objects; T253)
```

Atomicity note: IRIS is today uniprocessor with IRQ-off spinlocks and a
non-preemptive kernel (no yield inside retype), so the
validate→reserve→initialize→publish→commit sequence is atomic against any
other syscall; the locks keep the discipline for a future SMP.

## SYS_UNTYPED_RETYPE2 (111) — canonical path

```
RETYPE2(ut, type | count<<32, dest_cnode | slot<<32, obj_arg) → 0 | error
```

- `ut`: Untyped cap (CPtr <1024 or handle ≥1024), RIGHT_WRITE.
- `count`: 0→1, max 32 (batch ≤128 KiB); UNTYPED/FRAME require count=1 in S1.
- `dest_cnode`: 0 = the caller's root CNode; otherwise a CNode cap with
  RIGHT_WRITE.
- `slot`: first slot; `[slot, slot+count)` must exist and be empty; slot 0
  (CPTR_NULL) is rejected.
- `obj_arg`: CNODE → num_slots (power of 2, ≤4096); UNTYPED/FRAME → bytes.

Up-front validation (no mutation): valid source CPtr of type Untyped; rights;
allowed type (closed manifest, T251); valid size/count; overflow-free
multiplications; capacity; alignment; valid, writable destination CNode; empty
slots; device/normal restriction; limits (`KCNODE_MAX_SLOTS`,
`KUNTYPED_RETYPE_MAX_*`).

Commit:

```
validate everything
reserve the complete range      (kuntyped_alloc_children_atomic: one lock)
initialize every object         (placement in the region, zero-filled)
prepare every capability
publish all destination slots   (one CNode critical section)
commit Untyped state
```

A failure at any point ⇒ no cap published, no object live, no byte consumed,
no slot mutated, no counter drift.

The capabilities appear **directly in CSpace**; no handles are created and no
authority is returned via indices or pointers.

## SYS_UNTYPED_RETYPE (87) — legacy TRANSITIONAL

Restricted to the NON-migrated types: `UNTYPED`, `FRAME`, `SCHED_CONTEXT`.
The migrated family (ENDPOINT/NOTIFICATION/CNODE/REPLY/TCB) returns
`NOT_SUPPORTED` (S20 — no migrated object is born from a handle). Recorded in
the ledger as MIGRATING; retired with the CSpace-only phase.

## SYS_UNTYPED_RESET (88)

`child_count == 0` → `used = 0`, `generation++`, reclaim/reuse counters.
`BUSY` otherwise (S13). Reuse never exposes prior state: blocks are zero-filled
both on destroy AND on carve (S28), and capability identity prevents an old
protocol from reaching the new object (S29/T259); `generation` is the
observable witness. No direct kernel pointers survive the physical reuse:
EP/notification queues are dequeued on close/cancel, `pending_kreply`/
`ep_reply_obj` are cleared on teardown, and the IRQ paths retain the
notification (child_count > 0 until deregistration) — which is why no
per-object generation was added (rule: do not add a generation where the cap's
identity already closes the stale path).

## SYS_UNTYPED_QUERY (112) — instrumentation (never authority)

- kind 1: global — `live_untypeds, retype_count, retype_failures,
  reset_count, reclaimed_bytes, reuse_count, overlap_denials`.
- kind 2: per-Untyped — `phys_base, total, used, generation, child_count,
  is_device` (RIGHT_READ on the cap).
- kind 3: per-migrated-type gauges — `endpoints/notifications/replies/cnodes
  live` (the per-type high-water/retype/destroy are derived from the global
  counters + gauges; the Fase 18 per-type counters remain in
  SYS_SCHED_INFO ext3).

Versioned structs (`version`, `struct_size`). No new resource-domain syscall
was created and `SYS_RESOURCE_INFO` did not grow.

## Bootstrap and delegation

```
kernel (PMM drain) → userboot [slots 16..]
userboot → init      [slot 12, one block]
init     → svcmgr    [slot 12, 256 KiB sub-untyped]
init     → iris_test [slot 55, 8 MiB sub-untyped]
svcmgr   → per service: EP/notification retyped from the pool + a 4 KiB reply
           sub-untyped per service (RESET+retype on each respawn)
```

Least authority: each service receives its endpoints, notifications and reply
objects minted — never the root Untyped. The pager and the VFS own no global
Untyped (verified by the report-slot masks, T156/T162/T201+).

## Failures and tests

Failure paths covered by T253 (partial batch/capacity), T254 (full validation,
stale cap, invalid dest, device/normal), T259 (retention by a live cap, clean
reuse), T262 (deterministic stress with an exact shadow model). Provenance:
T252 (exact region consumption + child_count + kslab delta 0). Legacy
retirement: T260 and T125/T126 adapted. Real services: T261.
