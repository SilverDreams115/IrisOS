# IRIS Memory Invariants (Fase 6.4)

This document is the authoritative reference for the memory safety invariants of the
IRIS kernel as of Fase 6.4.  Each invariant names where it is enforced and where it is
tested.  Future phases must not regress any of these invariants.

---

## Invariant Table

| # | Invariant | Implementation | Tests | Risk if broken |
|---|-----------|----------------|-------|----------------|
| I-01 | No user mapping uses raw `paging_map_checked_in` outside `kframe.c` (capability layer) | `kframe.c:kframe_map_page` is the only user-visible call site | FR-23..FR-32, grep: no call in `syscall_vm.c`, `task_lifecycle.c`, `usercopy.c`, `#PF` path | Uncapped user PTEs, capability bypass |
| I-02 | Every user page at boot has a KFrame (`bootstrap_kframe_map`) | `task_lifecycle.c`: `bootstrap_kframe_map` loop for text and stack | FR-42..FR-50 | Uncounted bootstrap pages, stale PTEs at teardown |
| I-03 | Every VMO-mapped user page has a KFrame (`kframe_alloc_vmo_page`) | `syscall_vm.c:sys_vmo_map`, `sys_vmo_map_into` | FR-51..FR-62 | VMO physical pages freed while PTEs live |
| I-04 | Every user-visible mapping is in `KVSpace.mappings` | `kframe_map_page` inserts; `kframe_unmap_page` / `kvspace_unmap_page` / `kvspace_invalidate` remove | FR-38..FR-39, FR-54..FR-59, FR-66, FR-69 | mapping_count divergence, leaks |
| I-05 | Each successful `kframe_map_page` increments `mapping_count` by 1 | `kframe.c:128` | FR-26, FR-44, FR-68..FR-69 | count divergence |
| I-06 | Each successful unmap decrements `mapping_count` by 1 | `kframe.c:179`, `kvspace.c:99` | FR-28, FR-54, FR-59, FR-68..FR-69 | count divergence |
| I-07 | `kvspace_invalidate` removes all mappings; `mapping_count == 0` after | `kvspace.c:kvspace_invalidate` | FR-33, FR-39..FR-40, FR-48, FR-62 | stale PTEs, leaked nodes |
| I-08 | `KFrame.mapped_count` increments on `kframe_map_page` success | `kframe.c:132` | FR-26, FR-45, FR-68 | wrong destroy guard |
| I-09 | `KFrame.mapped_count` decrements on any successful unmap path | `kframe.c:194`, `kvspace.c:72,113` | FR-28, FR-54, FR-63, FR-68 | destroy guard fires early/late |
| I-10 | `kframe_obj_destroy` asserts `mapped_count == 0` | `kframe.c:21-23` | FR-28, FR-63, FR-67 | use-after-free of physical page |
| I-11 | `mapped_count` is decremented BEFORE `kobject_release` on every unmap path | `kframe.c:194-195` (fixed Fase 6.4), `kvspace.c:72-73`, `kvspace.c:113-114`, `kvspace.c:25-26` | FR-63 (regression test for the Fase 6.4 fix) | `kframe_obj_destroy` panics when mapping retain is last |
| I-12 | `rollback_vmo_maps` leaves no stale PTEs after partial map failure | `syscall_vm.c:rollback_vmo_maps` calls `kvspace_unmap_page` per page | FR-66 | leaked PTEs on OOM error path |
| I-13 | `rollback_vmo_maps` leaves `mapping_count` at the pre-map value | same | FR-66 | count divergence after partial failure |
| I-14 | `kvspace_invalidate` on a VSpace with zero mappings is a safe no-op | `kvspace.c:66` — empty loop body | FR-41 (empty VSpace variant) | crash on double invalidate |
| I-15 | Duplicate `kframe_map_page` for the same VA returns `IRIS_ERR_BUSY`; no PTE or node added | `kframe.c:105-109` | FR-27 | silent VA alias |
| I-16 | `kframe_map_page` on an invalidated VSpace returns `IRIS_ERR_BAD_HANDLE` | `kframe.c:99-103` | FR-32 | map into dead VSpace |
| I-17 | `kframe_unmap_page` for a VA occupied by a different frame returns `IRIS_ERR_INVALID_ARG` | `kframe.c:172-176` | FR-30 | cross-frame unmap |
| I-18 | VMO physical pages are not freed while any `KFrameMapping` remains alive | `kframe_obj_destroy` releases `vmo_owner` retain only when `refcount == 0` (after unmap) | FR-57..FR-58, FR-67 | use-after-free |
| I-19 | `kvspace_obj_destroy` safety net handles active mappings (no prior `kvspace_invalidate`) | `kvspace.c:kvspace_obj_destroy` — full sweep loop | FR-67 | leak on unusual teardown path |
| I-20 | No demand paging exists in the kernel (`#PF` ring-3 never allocates pages) | No `kprocess_resolve_demand_fault`; `#PF` handler returns fault to userland | FR-41 | silent demand reintroduction |
| I-21 | `usercopy` never allocates user pages | `kernel/core/usercopy.c` — no PMM calls | (grep) | demand paging via usercopy |
| I-22 | W^X: `kframe_map_page` rejects `MAP_WRITABLE | MAP_EXEC` | `kframe.c:88` | executable writable pages |
| I-23 | `kframe_map_page` rejects `flags > 3` (bits beyond W|X) | `kframe.c:84` | FR-61 | undefined flag leak |

---

## Ordering Invariant (Critical — Fixed in Fase 6.4)

**I-11** is the most subtle invariant.  Every unmap path must follow this order:

```
1. Remove KFrameMapping node from vs->mappings (under lock)
2. Unmap PTE (paging_unmap_in)
3. Release lock
4. Free KFrameMapping slab node
5. atomic_fetch_sub(&f->mapped_count, 1)   ← MUST come before kobject_release
6. kobject_release(&f->base)               ← may trigger kframe_obj_destroy
```

If step 5 and step 6 are swapped, `kframe_obj_destroy` is called while `mapped_count == 1`,
triggering the IRIS_ASSERT and aborting.  This bug existed in `kframe_unmap_page` prior to
Fase 6.4.  All kvspace paths (`kvspace_unmap_page`, `kvspace_invalidate`, `kvspace_obj_destroy`)
had the correct order; only `kframe_unmap_page` was wrong.

The fix (one-line swap in `kframe.c:193-194`) is tested by FR-63, which exercises the exact
scenario: map a frame, release the alloc retain, then call `kframe_unmap_page` with only the
mapping retain remaining.

---

## Failure Path Guarantees

| Operation | Failure mode | What must hold after failure |
|-----------|-------------|------------------------------|
| `kframe_map_page` — kslab OOM | `IRIS_ERR_NO_MEMORY` | No PTE; `mapping_count` unchanged; `mapped_count` unchanged |
| `kframe_map_page` — paging OOM | `IRIS_ERR_NO_MEMORY` | KFrameMapping node freed; same guarantees as above |
| `kframe_map_page` — duplicate VA | `IRIS_ERR_BUSY` | No second PTE; counts unchanged |
| `kframe_map_page` — invalidated VSpace | `IRIS_ERR_BAD_HANDLE` | KFrameMapping node freed; no PTE |
| `rollback_vmo_maps` (mid-loop) | called explicitly | All successfully-mapped pages unmapped; `mapping_count` restored; no stale PTEs |
| `kvspace_unmap_page` — VA not found | `IRIS_ERR_NOT_FOUND` | VSpace and frame state unchanged |
| `kvspace_unmap_page` — invalidated VSpace | `IRIS_ERR_BAD_HANDLE` | VSpace and frame state unchanged |

---

## Known Remaining Debt

| Item | Risk | Phase |
|------|------|-------|
| SMP TLB shootdown | Concurrent unmap on multi-core could leave stale TLB entries | Fase SMP |
| KChannel migration to KEndpoint | IPC still uses KChannel (handle_id_t path) | Fase 7 |
| `handle_id_t` removal | Dual-path cap resolution | Fase 7+ |
| Userland pager | Kernel-side demand paging is gone; no userland fault handler yet | Post-Fase 7 |
| Device frames (IOMMU) | No IOMMU protection for DMA | Post-Fase 7 |
| Large frames (2 MiB, 1 GiB) | Only 4 KiB pages supported | Post-Fase 7 |
| Formal verification | Invariants documented but not machine-verified | Long-term |
| External fuzzing | No AFL/libFuzzer harness for syscall_vm paths | Long-term |

---

## Test Coverage Summary (Fase 6.4)

| Suite | Count | FR range |
|-------|-------|----------|
| Fase 5 — alloc/destroy/CSpace | 22 | FR-1..FR-22 |
| Fase 5.1 — paging stub + lifecycle | 18 | FR-23..FR-40 |
| Fase 6.1 — demand paging regression | 1 | FR-41 |
| Fase 6.2 — bootstrap KFrame maps | 9 | FR-42..FR-50 |
| Fase 6.3 — VMO-to-Frame migration | 12 | FR-51..FR-62 |
| Fase 6.4 — stress / invariant audit | 7 | FR-63..FR-69 |
| **Total** | **69** | — |

Total assertions across all suites: **10274** (including 1000-cycle loop in FR-68).
