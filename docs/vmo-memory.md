# VMO Memory Model (Fase 6.3)

## Overview

Virtual Memory Objects (VMOs) are the userspace-visible abstraction for physical memory in IRIS.
Every user page is owned by exactly one VMO; every VMO page that has been mapped into an address
space is backed by exactly one KFrame capability.  This document describes the lifecycle from VMO
creation through mapping, copying, and teardown.

## Object Roles

| Object | Owns | Destroyed when |
|--------|------|----------------|
| KVmo | PMM pages (`pages[]` array) | Last handle closed and all KFrame vmo_owner retains released |
| KFrame | Physical address slot (4 KB) | `mapped_count == 0` and refcount == 0 |
| KVSpace | `KFrameMapping` singly-linked list | `kvspace_invalidate` + final kobject_release |
| KFrameMapping | One KFrame retain + one list node | Removed by `kvspace_unmap_page` or `kvspace_invalidate` |

## VMO Kinds

**Sparse VMO** (`kvmo_create`, `v->sparse == 1`, `v->owned == 1`)  
Backed by PMM pages.  Pages in `v->pages[]` are either 0 (unallocated) or a physical address.
`sys_vmo_map` and `sys_vmo_map_into` allocate any zero entries via `pmm_alloc_page` before
installing PTEs — allocation is **eager at map time**; there is no fault-driven demand paging
(removed in Fase 6.1, guarded by the FR-41 regression test).
`kframe_alloc_vmo_page(phys, v)` creates the KFrame and retains the VMO so
that `kvmo_destroy` (and thus `pmm_free_page`) cannot run while the mapping is live.
(The field was named `demand` until Fase V1; it was renamed because the old
name suggested fault-time paging that does not exist.)

**Wrap/MMIO VMO** (`kvmo_wrap`, `v->sparse == 0`, `v->owned == 0`)  
Wraps a contiguous physical range (e.g. framebuffer).  Pages are not PMM-owned.  Each page is
mapped via `kframe_alloc(phys + off, 4096, NULL)` — no `vmo_owner` set, so the frame's destroy
path never calls `pmm_free_page`.

## Mapping Path (`sys_vmo_map` / `sys_vmo_map_into`)

Both syscalls use the same eager-mapping strategy:

1. Pre-check: walk `[vaddr, vaddr+map_size)` to verify no existing PTEs (returns `IRIS_ERR_BUSY`
   if any page is already mapped).
2. For each page in the VMO:
   - Sparse: if `v->pages[i] == 0`, allocate with `pmm_alloc_page` and zero the page.
   - Call `kframe_alloc_vmo_page(phys, v)` (sparse) or `kframe_alloc(phys, 4096, NULL)` (MMIO).
   - Call `kframe_map_page(f, vs, va, flags)` — this installs the PTE inside `vs->lock`, adds a
     `KFrameMapping` node to `vs->mappings`, retains the frame, and increments `mapped_count`.
   - Call `kobject_release(&f->base)` to drop the alloc retain; the mapping retain remains.
3. On any error, `rollback_vmo_maps(vs, start, mapped_until)` removes the successfully-installed
   pages by calling `kvspace_unmap_page` in a loop.

`sys_vmo_map` maps into the caller's own VSpace (`t->process->vspace`).  
`sys_vmo_map_into` maps into a target process's VSpace (`proc->vspace`), requiring `RIGHT_MANAGE`
on the process handle.

## Unmap Path (`sys_vmo_unmap`)

Calls `kvspace_unmap_page(vs, va)` for each page in the range.  Pages not present are silently
skipped.  `kvspace_unmap_page`:

1. Acquires `vs->lock`, finds the `KFrameMapping` node by VA, removes it from the list.
2. Calls `paging_unmap_in(cr3, va)` to remove the PTE.
3. Releases the lock, then frees the node, decrements `mapped_count`, and calls
   `kobject_release(&f->base)`.  If the frame's refcount reaches zero, `kframe_obj_destroy` runs:
   - ASSERTs `mapped_count == 0`.
   - Frees the `KFrame` slab node.
   - Calls `kobject_release(&vmo->base)` if `vmo_owner` is set.  If this was the last VMO retain,
     `kvmo_destroy` runs and calls `pmm_free_page` for each owned page.

## Address Space Teardown

`kprocess_reap_address_space` calls `kvspace_invalidate(p->vspace)`, which:
1. Atomically removes the whole `mappings` list and zeros `vs->cr3` / `vs->valid` under `vs->lock`.
2. Walks the captured list outside the lock, calling `paging_unmap_in`, freeing each node,
   decrementing `mapped_count`, and releasing each frame retain.

This ordering guarantees that all `mapped_count` values reach zero before any `kframe_obj_destroy`
runs, satisfying the assert in `kframe_obj_destroy`.

## W^X Enforcement

Both `sys_vmo_map` and `sys_vmo_map_into` reject `flags` with both WRITABLE (bit 0) and EXEC
(bit 1) set, returning `IRIS_ERR_INVALID_ARG`.  `kframe_map_page` enforces the same constraint.

## Process Creation

`sys_process_create` allocates a `KProcess`, creates a new page table root via
`paging_create_user_space`, then calls `kvspace_alloc(proc->cr3)` to create a `KVSpace` backed by
that root.  Without this KVSpace, `sys_vmo_map_into` would reject the target process.
`kprocess_reap_address_space` later calls `kvspace_invalidate` + `kobject_release` to tear it down.

## Refcount Summary for a Single Mapped Page

After `sys_vmo_map` successfully maps page `i` of VMO `v` at address `va`:

| Object | Refcount contributors |
|--------|-----------------------|
| KVmo `v` | 1 caller handle + 1 per KFrame vmo_owner |
| KFrame `f` | 1 KFrameMapping retain (alloc retain released by sys_vmo_map) |
| KFrameMapping | embedded in `vs->mappings` list (not ref-counted, owned by KVSpace) |

After `sys_vmo_unmap(va)`:

| Object | Refcount change |
|--------|-----------------|
| KFrame `f` | mapping retain released → refcount 0 → `kframe_obj_destroy` |
| KVmo `v` | vmo_owner released by `kframe_obj_destroy` → may reach 0 → `kvmo_destroy` |

## Critical Ordering Note (Fase 6.4)

`kframe_obj_destroy` asserts `mapped_count == 0`.  Therefore, every path that releases
the mapping retain (i.e. the `kobject_release(&f->base)` that may trigger destroy) **must**
decrement `mapped_count` first.  This ordering is enforced consistently across:

- `kframe_unmap_page` (fixed in Fase 6.4 — was the only path with wrong order)
- `kvspace_unmap_page`
- `kvspace_invalidate`
- `kvspace_obj_destroy`

Invariant I-11 in `docs/memory-invariants.md` documents this formally.

## Stress Coverage (Fase 6.4)

Failure injection tests added in Fase 6.4 verify:

- kslab OOM inside `kframe_map_page` → no PTE, no mapping node (FR-64)
- paging OOM after kslab success → mapping node freed (FR-65)
- Partial multi-page rollback → no stale PTEs, correct counts (FR-66)
- Direct VSpace destroy with active mappings → safety net cleans up (FR-67)
- 1000-cycle map/unmap loop → no accumulation (FR-68)
- Non-sequential interleaved unmap → `mapping_count` consistent (FR-69)

See `docs/memory-invariants.md` for the full invariant list.
