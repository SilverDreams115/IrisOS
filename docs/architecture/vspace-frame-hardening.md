# Fase 19 — VM / VSpace / frame mapping hardening

Status: ACCEPTED — implemented in this phase.  Companion to
`untyped-retype-revoke-hardening.md` (Fase 18), `lifecycle-hardening.md`
(Fase 16) and `scheduler-hardening.md` (Fase 17).  Fase 19 closes the gap Fase
18 left explicit: ring 3 can now drive `SYS_FRAME_MAP` / `SYS_FRAME_UNMAP`
directly against its own address space, so the `Frame ↔ VSpace ↔ PTE ↔
mapped_count ↔ cleanup` border is proven, not assumed.  Locked with runtime
tests T132–T139.

**No new VM bug was found.**  The audit confirmed map/unmap is correct, map
failure is observably atomic, and process-death cleanup sweeps every mapping
before the address space is destroyed.  This phase adds a minimal self-authority
primitive (`SYS_VSPACE_SELF`), additive VM instrumentation, and the tests/docs
that make the guarantees regression-proof.  The runtime selftest suite rises
from 127/127 to 135/135.

---

## The VSpace model

A `KVSpace` (`kernel/new_core/src/kvspace.c`) is a capability over a process
address space.  It wraps the process's `cr3` (PML4 physical base) plus a `valid`
flag and a singly-linked list of `KFrameMapping` nodes:

```text
struct KVSpace { KObject base; spinlock lock; uint64_t cr3; int valid;
                 uint32_t mapping_count; KFrameMapping *mappings; }
struct KFrameMapping { KFrame *frame; uint64_t user_va; KFrameMapping *next; }
```

Ownership:

- `KProcess` holds one lifecycle ref on its `KVSpace`; each CSpace slot that
  names it holds lifecycle + active refs.
- Page tables are owned by `KProcess`; `KVSpace` never frees them.
- The mapping list is dynamically allocated (no fixed ceiling), so runtime frame
  maps and bootstrap maps coexist.

Every process is born with bootstrap `KFrame` mappings (its text r-x and stack
rw-) installed via `bootstrap_kframe_map` → `kframe_map_page`, so a live process
always contributes to the global live-mapping count.

## Authority required for map / unmap

`SYS_FRAME_MAP(frame_cptr, vspace_cptr, user_va, flags)`:

- **VSpace** resolves via `cspace_resolve_vspace` — **CPtr only, no handle
  fallback**, `KOBJ_VSPACE`, `RIGHT_WRITE` required (to install a PTE).
  `ACCESS_DENIED` is a hard stop.
- **Frame** resolves via `cspace_or_handle_resolve_frame` (CPtr or handle),
  `KOBJ_FRAME`, `RIGHT_READ` always, `RIGHT_WRITE` additionally when `flags`
  bit 0 (writable) is set.
- `flags`: bit 0 = writable, bit 1 = exec; **W^X enforced** (`INVALID_ARG` if
  both).
- `user_va`: `kframe_va_valid` requires page alignment, the user private window
  `[0x0000_0080_0000_0000, 0x0000_8000_0000_0000)`, and canonical form (bits
  63:47 clear).  A kernel or non-canonical VA is `INVALID_ARG`.

`SYS_FRAME_UNMAP(frame_cptr, vspace_cptr, user_va)` requires the same VSpace
`RIGHT_WRITE` and `RIGHT_READ` on the frame; it removes the PTE only if the VA
maps **exactly this frame**.

### SYS_VSPACE_SELF (Fase 19, syscall 106)

Ring-3 code has no VSpace CPtr by default (only the boot process gets
`BOOT_CPTR_VSPACE`).  `SYS_VSPACE_SELF()` returns a handle to the **caller's own**
`KVSpace` with `RIGHT_READ|RIGHT_WRITE|RIGHT_DUPLICATE` — the same object-cap
accessor shape as `SYS_TCB_SELF`.

This is **self-authority only** and grants no new power: a process already
controls its own address space through the VMO map/unmap syscalls; a cap to its
own VSpace merely lets it drive the frame-mapping path on itself by CPtr.  There
is no argument and no way to name another process's VSpace — cross-process
address-space authority still requires a process capability with `RIGHT_MANAGE`
(`SYS_VMO_MAP_INTO`).  iris_test is currently the only caller: it obtains the
cap, mints it into `IRIS_CPTR_TEST_VSPACE` via its self-process cap, and uses it
for T132–T139.

## Map contract

`kframe_map_page`:

1. Allocate the `KFrameMapping` node **before** taking `vs->lock` (so a slab
   failure returns `NO_MEMORY` before any state changes).
2. Under the lock: reject an invalid/reaped VSpace (`BAD_HANDLE`); reject an
   already-mapped VA (`paging_virt_to_phys_in != 0` → `BUSY`); install the PTE
   via `paging_map_checked_in`.
3. Retain the frame for the mapping's lifetime, prepend the node, bump
   `mapping_count`, release the lock, then `mapped_count++` and
   `kframe_stat_map()`.

**Map failure atomicity (V9/V11):** every validation failure returns before the
PTE leaf is written, before the node is linked, and before `mapped_count`
changes — no partial mapping is observable.  The one documented non-atomicity is
memory-only: `paging_map_root` allocates intermediate page-table pages
(PDPT/PD/PT) on demand; if a deeper level fails, the shallower tables already
allocated are **not** individually rolled back.  They are valid empty tables
retained by the VSpace, reused by later maps, and freed wholesale at
`paging_destroy_user_space` (process teardown).  No leaf PTE, no `mapped_count`
change, no frame ref, no mapping node leaks.

## Unmap contract

Two symmetric paths decrement exactly once each:

- `kframe_unmap_page` (`SYS_FRAME_UNMAP`): finds the node by `(frame, va)` —
  `NOT_FOUND` if the VA is unmapped, `INVALID_ARG` if the VA maps a **different**
  frame — removes the PTE (`invlpg`), frees the node, `mapped_count--`,
  `kframe_stat_unmap()`, releases the frame.
- `kvspace_unmap_page`: finds the node by `va` (used by VMO rollback); same
  decrements.

Unmap is clean-idempotent per the documented contract: a second unmap of the
same VA returns `NOT_FOUND` and changes nothing (V14).

## Duplicate / overlap / remap contract

Verified by T135:

| Operation | Result |
|-----------|--------|
| map A @ X, then map A @ X again | `BUSY` (VA occupied) |
| map B @ X (X already mapped) | `BUSY` (VA occupied, any frame) |
| map A @ Y (second VA, same frame) | ok — two mappings, `A.mapped_count == 2` |
| unmap A @ Y then @ X (any order) | exact, one decrement each |
| unmap an unmapped VA | `NOT_FOUND`, no change |

A VA holds at most one mapping; a frame may be mapped at many VAs.  There is no
silent overwrite — occupancy is by VA, checked against the live PTE.

## Cleanup contract

- **Explicit:** `SYS_FRAME_UNMAP` / VMO unmap remove one mapping.
- **VSpace invalidate** (`kvspace_invalidate`, called by
  `kprocess_reap_address_space` on process death): sets `valid = 0`, takes the
  whole mapping list under the lock, then for each node unmaps the PTE, frees the
  node, `mapped_count--`, `kframe_stat_cleanup()`, releases the frame — so every
  frame observes `mapped_count == 0` before `paging_destroy_user_space` runs.
- **VSpace destroy** (`kvspace_obj_destroy`): a safety-net sweep for unusual
  teardown (direct CNode-slot deletion) mirroring invalidate.

**Frame destroy contract (V17):** `kframe_obj_destroy` **asserts
`mapped_count == 0`**.  A frame object can therefore never be destroyed while a
PTE references it — a stale PTE cannot outlive its frame.  In the tests, a clean
`SYS_HANDLE_CLOSE` of a frame is itself proof it was unmapped first.

## Relationship to untyped / retype / revoke

Frames come from `SYS_UNTYPED_RETYPE(KOBJ_FRAME)` (Fase 18).  A mapping holds an
**independent** reference on the frame, so:

- `SYS_CAP_REVOKE` is **cap-scoped** and never force-unmaps: revoking a frame's
  derived handle while it is mapped kills the derived cap but leaves the mapping
  (and the frame) live (T137 — this closes the Fase-18 T128 gap).
- The frame is destroyed only after the mapping is removed **and** the last cap
  is closed; `SYS_UNTYPED_RESET` then succeeds (`child_count == 0`), tying the VM
  and authority layers together.

## Relationship to CSpace

The self-VSpace cap is minted into a CSpace slot (`IRIS_CPTR_TEST_VSPACE`) via
`SYS_PROC_CSPACE_MINT` through the caller's own process cap; `SYS_FRAME_MAP`
resolves it by CPtr.  Rights reduce monotonically at the mint (a read-only
VSpace cap cannot install a PTE — T132/T138).

## PTE flags and user/kernel isolation

`kframe_map_page` builds PTE flags from the map flags: `PAGE_PRESENT | PAGE_USER`
always, `PAGE_NX` unless exec, `PAGE_WRITABLE` if writable.  Rights gate the
flags: a writable map needs `RIGHT_WRITE` on the frame (V19).  The VA validator
confines every user mapping to the user private window, so ring 3 cannot install
a PTE in kernel space (V20).

**Documented gap:** ring 3 has no safe way to read raw PTE flags, and triggering
a write-protection or NX `#PF` would fault the test process (no fault-handling
endpoint yet — a future phase).  So write/NX enforcement is asserted at the
authority layer (rights checks reject the disallowed map) rather than by
faulting.  The hardware-level enforcement (`PAGE_WRITABLE`/`PAGE_NX` bits) is set
correctly in `kframe_map_page` and covered structurally by the host paging code.

## Instrumentation (additive, ABI-safe)

Fase 19 adds a fifth `SYS_SCHED_INFO` tier, written only when the caller passes
`buf_size >= 160` (a caller passing 136..159 gets the exact 136-byte Fase-18
snapshot — same additive rule as every prior tier):

```text
offset 136: uint32_t vspace_live         KVSpace objects live
offset 140: uint32_t live_mapping_count  KFrameMapping nodes live (across all VSpaces)
offset 144: uint32_t map_success_count   successful maps, cumulative
offset 148: uint32_t unmap_success_count explicit unmaps, cumulative
offset 152: uint32_t tlb_invalidate_count local invlpg, cumulative
```

`live_mapping_count` is the anchor: it returns to baseline after every unmap and
every VSpace teardown, so a leaked mapping or double free is immediately visible.

## Audit matrix

| Area | Current behavior | Invariant | Coverage | Risk |
|------|------------------|-----------|----------|------|
| VSpace authority | `cspace_resolve_vspace`, `RIGHT_WRITE`, no fallback | V1, V4 | T132 | none |
| Frame authority | dual resolve, READ/WRITE per flags | V2, V19 | T132, T138 | none |
| Wrong-type cap | typed resolve rejects | V3 | T132, T134 | none |
| VA validation | align + user window + canonical | V5, V6, V7 | T134, T138 | none |
| Occupied VA | `paging_virt_to_phys_in` → BUSY | V8 | T134, T135 | none |
| Map failure atomicity | validate-before-mutate | V9, V11 | T134 | intermediate PT pages retained-not-leaked (teardown-reclaimed) |
| mapped_count discipline | +1 on map, −1 on every removal | V10, V13, V17 | T133, T135, T137 | none |
| Unmap exactness | by (frame,va); NOT_FOUND/INVALID_ARG | V12, V14 | T133, T135 | none |
| VSpace cleanup | invalidate sweeps all mappings | V15, V18 | T136 | none |
| Process death | reap → invalidate → destroy PT | V16 | T136 | none |
| Mapped-frame revoke | cap-scoped; no force-unmap | V17, V18 | T137 | revoke ≠ unmap (documented) |
| User/kernel isolation | VA window confinement | V6, V20 | T134, T138 | PTE-flag introspection gap (documented) |
| TLB | local invlpg on unmap | V21 | T133 | SMP shootdown absent (documented) |

## Invariants (V1–V22)

V1 map needs VSpace authority; V2 map needs frame/VMO authority; V3 no
wrong-type cap; V4 no insufficient rights; V5 no non-canonical VA; V6 no kernel
VA from userland; V7 no unaligned VA; V8 no silent overwrite of an occupied VA;
V9 map failure atomic (no partial PTE visible); V10 `mapped_count` +1 per
success; V11 `mapped_count` unchanged on failure; V12 unmap removes exactly the
requested mapping; V13 unmap −1 per success; V14 absent unmap clean/idempotent;
V15 VSpace cleanup removes all mappings; V16 process death cleans its VSpace
without leaks; V17 no frame destroy with `mapped_count > 0`; V18 no stale PTE
survives cleanup; V19 PTE rights reflect cap rights; V20 user/kernel isolation
preserved; V21 local TLB invalidate where required; V22 SMP shootdown gap
documented, not hidden.

## Tests T132–T139

| Test | Scenario | Invariants | Failure paths |
|------|----------|-----------|---------------|
| T132 | self-VSpace cap: valid map, wrong-type, missing rights, no fallback | V1,V3,V4,V20 | wrong-type vspace, RO vspace |
| T133 | map writable, write/read pattern, unmap, counters + frame baseline | V2,V10,V12,V13,V21 | — |
| T134 | every map failure mode; atomic + valid-after-fail | V5,V6,V7,V9,V11 | unaligned, kernel VA, W^X, wrong types, RO frame, empty slot |
| T135 | duplicate/overlap/remap contract; unmap ordering | V8,V10,V12,V13,V14 | dup/overlap BUSY, absent unmap |
| T136 | child VSpace + bootstrap mappings swept on kill/exit | V15,V16,V17,V18 | — |
| T137 | revoke a mapped frame's cap; frame survives; unmap→close | V17,V18 | — |
| T138 | RO map ok / RW denied; W^X; kernel VA | V4,V6,V19,V20 | RO writable, W^X, kernel VA |
| T139 | fixed-seed map/derive/revoke/unmap stress + forced failures | V9,V10,V13,V15,V17,V18 | unaligned, occupied |

## TLB / SMP notes

- **Current local behavior:** `paging_unmap_in` issues one `invlpg` on the
  current CPU per PTE removal; map installs a fresh PTE (no stale entry, no flush
  needed).  `tlb_invalidate_count` makes local invalidation observable (V21).
- **Unsafe future assumptions:** no cross-CPU TLB shootdown exists.  On SMP, a
  PTE removed on one CPU could remain cached in another CPU's TLB — a stale
  translation.  The mapping list, `mapped_count`, and `invlpg` are all local.
- **Required before SMP:** a shootdown IPI on unmap / VSpace invalidate for any
  page reachable from more than one CPU; per-CPU accounting is not required (the
  counters are already atomic).

## Remaining gaps

- Intermediate page-table pages allocated during a failed deep map are reclaimed
  only at teardown, not individually.
- Ring 3 cannot introspect raw PTE flags or safely fault on a write/NX
  violation; enforcement is asserted at the authority layer + host paging code.
  A fault-handling endpoint would let a future test observe the `#PF` directly.
- No SMP TLB shootdown (single-CPU only).

## Validation

`make`, `make test-unit` (10200/10200), `make smoke-runtime` (135/135),
`make ENABLE_RUNTIME_SELFTESTS=1 smoke-runtime-selftests` (135/135) — all green,
no flakes across repeated runs.
