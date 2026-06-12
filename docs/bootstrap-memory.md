# Bootstrap Memory Layout (IRIS)

## Overview

At boot, the kernel must map the userboot binary and its initial stack into the
first user-space process before any user-space process exists to make syscalls.
These mappings historically bypassed the normal seL4-style capability path
(`KUntyped → retype → KFrame → SYS_FRAME_MAP`) and were called
**bootstrap direct maps**.

**Fase 6.2** migrated these bootstrap user mappings to be KFrame-backed,
closing the main tracking gap documented in Fase 6.1.

---

## Bootstrap Memory After Fase 6.2

### Demand paging

Demand paging was eliminated in **Fase 6**.  `#PF` ring-3 never allocates
physical pages.  `kprocess_resolve_demand_fault` does not exist.

### Runtime user mappings

Runtime user mappings require the explicit Frame/VMO path:
- `KFrame` objects created from `KUntyped` via `SYS_UNTYPED_RETYPE`.
- Mapped via `SYS_FRAME_MAP` (`kframe_map_page`).
- VMO maps via `sys_vmo_map` / `sys_vmo_map_into` (eager, no demand).

### Bootstrap Frame-backed mappings

Bootstrap user pages now go through `bootstrap_kframe_map` (a kernel-internal
helper, not a syscall path) which:

1. Calls `kframe_alloc(paddr, 4096, NULL)` — creates a `KFrame` with no
   `KUntyped` parent (physical pages are owned externally by task struct
   fields).
2. Calls `kframe_map_page(f, proc->vspace, user_va, flags)` — installs the PTE
   **and** registers the mapping in `KVSpace.mappings[]`.
3. Returns the KFrame with the alloc retain held by the caller.
4. The alloc retain is stored in `KProcess.bootstrap_frames[]` via
   `kprocess_register_bootstrap_frame`.

Both the userboot **text** pages (r--x) and the initial **stack** pages (rw-nx)
are now mapped this way.

---

## Bootstrap Maps Created at Boot

### 1. Userboot text pages (r--x)  — **KFrame-backed**

**File**: `kernel/core/scheduler/task_lifecycle.c`  
**Function**: `task_create_user_impl` → `bootstrap_kframe_map` loop  
**VA range**: `USER_TEXT_BASE` … `USER_TEXT_BASE + ub_pages * 4096`  
**PA source**: `ub_copy_phys` — a fresh PMM allocation, page-aligned copy of
the userboot initrd binary  
**Flags**: `MAP_EXEC` (r--x, NX clear)  
**KFrame**: one KFrame per page; alloc retain in `proc->bootstrap_frames[]`  
**KVSpace slot**: one `KFrameMapping` slot per page in `vs->mappings[]`

### 2. Userboot initial user stack (rw-nx)  — **KFrame-backed**

**File**: `kernel/core/scheduler/task_lifecycle.c`  
**Function**: `task_create_user_impl` → `bootstrap_kframe_map` loop  
**VA range**: `USER_STACK_BASE + USER_STACK_GUARD_PAGES*4096` … `USER_STACK_TOP`  
**PA source**: `ustack_phys` — a fresh `pmm_alloc_pages` block  
**Flags**: `MAP_WRITABLE` (rw-nx)  
**KFrame**: one KFrame per page; alloc retain in `proc->bootstrap_frames[]`  
**KVSpace slot**: one `KFrameMapping` slot per page in `vs->mappings[]`

### 3. Kernel stacks (kernel-space only)  — **kernel-only, valid**

**File**: `kernel/core/scheduler/kstack.c`  
**Function**: `kstack_alloc` → `paging_map_checked_in`  
**VA range**: kernel virtual address space (not user-visible)  
**Rationale**: Kernel stacks are kernel-internal objects; there is no
user-facing capability for them.  This is correct and not a transitional
pattern.

---

## KVSpace Creation Timing

`task_create_user_impl` now creates the KVSpace **before** the bootstrap map
loops so that `kframe_map_page` can register back-refs immediately.  Previously
the KVSpace was created in `kernel_main.c` after `task_spawn_user` returned.
`kernel_main.c` now only publishes the existing `proc->vspace` in root CNode
slot `BOOT_CPTR_VSPACE` (slot 2).

---

## Lifecycle and Teardown

### Physical memory

Physical page lifetime is tracked by `task` struct fields (`ustack_phys`,
`utext_phys`).  `free_user_stack_pages` and `free_user_text_pages` free the
PMM blocks on teardown paths (unchanged from Fase 6.1).

### Mapping records

`kvspace_invalidate(p->vspace)` sweeps all `vs->mappings[]` slots:
- calls `paging_unmap_in` per entry (removes PTE, issues invlpg)
- decrements `frame->mapped_count` to 0
- releases the per-mapping retain (`kobject_release`)

### Bootstrap alloc retains

After `kvspace_invalidate`, `kprocess_release_bootstrap_frames(p)` releases
each KFrame alloc retain stored in `p->bootstrap_frames[]`.  Since
`mapped_count == 0` at that point, `kframe_obj_destroy` fires without the
assert triggering.  `kframe_obj_destroy` calls `kslab_free` — physical memory
is **not** freed here (managed externally by task struct fields).

### Order in `kprocess_reap_address_space`

```
kvspace_invalidate(p->vspace)          ← mapped_count → 0 for all bootstrap frames
kprocess_release_bootstrap_frames(p)   ← alloc retains released → kframe_obj_destroy
paging_destroy_user_space(cr3)         ← PT structure pages freed
```

### Failure rollback in `task_create_user_impl`

If `bootstrap_kframe_map` fails mid-loop:
- Already-created KFrames are in `proc->bootstrap_frames[]`.
- `goto fail_copy` / `goto fail` triggers `kprocess_reap_address_space(proc)`,
  which calls `kvspace_invalidate` + `kprocess_release_bootstrap_frames`.
- Physical pages (contiguous PMM blocks) are freed by `free_phys_pages_range`.
- No double-free: KFrame objects do not own physical memory (no pmm_owned flag
  on `kframe_alloc(..., NULL)`).

---

## Invariants

| Invariant | Status |
|-----------|--------|
| No demand paging | ✓ eliminated in Fase 6 |
| No `#PF` ring-3 allocation | ✓ confirmed |
| Bootstrap user pages have KFrame | ✓ Fase 6.2 |
| Bootstrap KFrames registered in `proc->bootstrap_frames[]` | ✓ |
| KFrame mappings in `KVSpace.mappings[]` | ✓ |
| `mapped_count` reflects bootstrap mappings | ✓ |
| `kvspace_invalidate` auto-unmaps bootstrap mappings | ✓ |
| `mapping_count` reaches 0 on teardown | ✓ |
| No stale PTEs after teardown | ✓ |
| No leaked KFrame alloc retains | ✓ |
| VMO maps via KFrame | ✓ Fase 6.3 — sys_vmo_map / sys_vmo_map_into rewritten |
| T001–T017 pass | ✓ iris_test 17/17 |
| FR-1..FR-62 pass | ✓ 2143/2143 unit tests |

---

## Fase 6.3 Changes

**Fase 6.3** completed VMO-to-Frame capability migration.

### sys_vmo_map / sys_vmo_map_into rewritten

Both syscalls now eagerly install KFrame-backed PTEs:

- Sparse VMOs (named "demand" before Fase V1): each page allocated via `pmm_alloc_page` if not yet present,
  then wrapped in a `KFrame` created by `kframe_alloc_vmo_page(phys, v)`.
  The VMO retain in `f->vmo_owner` defers `kvmo_destroy` until after all frames
  for that VMO's pages are released.
- MMIO/wrap VMOs: each page wrapped via `kframe_alloc(phys + off, 4096, NULL)`.
  No PMM ownership; `kframe_obj_destroy` skips `pmm_free_page`.

`sys_vmo_unmap` now calls `kvspace_unmap_page` per page, which removes the
`KFrameMapping` node, unmaps the PTE, and releases the frame retain.

### KVmoMapping removed

`struct KVmoMapping`, `KProcess.vmo_mappings`, `vmo_mapping_count`,
`kprocess_register_vmo_map`, `kprocess_unregister_vmo_map`, and
`kprocess_clear_vmo_mappings` are fully removed.  All VMO-mapped pages are now
tracked in `KVSpace.mappings` and cleaned by `kvspace_invalidate`.

### KVSpace.mappings is now a dynamic singly-linked list

`KVSPACE_MAPPING_SLOTS` (fixed 32-slot array) was replaced with a singly-linked
list of slab-allocated `KFrameMapping` nodes.  There is no slot limit for runtime
VMO mappings.  Bootstrap frames continue to use `proc->bootstrap_frames[]` (limit
`KPROCESS_BOOTSTRAP_FRAME_MAX = 32`).

### sys_process_create allocates KVSpace

Child processes created via `sys_process_create` now receive a `KVSpace` backed
by their new CR3.  This is required for `sys_vmo_map_into` to install KFrame-backed
PTEs into the child.

### kvspace_unmap_page added

New function in `kvspace.c` that finds a mapping node by VA, removes it from the
list, removes the PTE, and releases the frame retain — the inverse of
`kframe_map_page`.

---

## Verification (Fase 6.3)

- `make` — 0 warnings, 0 errors
- `make test-unit` — 2143/2143 tests pass (FR-1..FR-62)
- `make smoke-runtime` — all smoke checks green
- `ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests` — all selftest checks green
- `iris_test` — 17/17
- `kprocess_resolve_demand_fault` — does not exist (confirmed by grep)

---

## Fase 6.4 Changes (Memory stress / invariant audit)

### Bug fixed: `kframe_unmap_page` ordering

`kframe_unmap_page` previously called `kobject_release(&f->base)` **before**
`atomic_fetch_sub(&f->mapped_count, 1)`.  When the mapping retain was the last retain
on the frame, `kobject_release` triggered `kframe_obj_destroy`, which IRIS_ASSERTs
`mapped_count == 0` — but `mapped_count` was still 1 at that point, causing a panic.

All `kvspace.c` paths (`kvspace_unmap_page`, `kvspace_invalidate`, `kvspace_obj_destroy`)
already had the correct order.  The fix is a one-line swap in `kframe.c`.

### Failure injection infrastructure added

`tests/kernel/stubs.c` now exposes:
- `kslab_fail_after(n)` / `kslab_clear_fail()` — simulate slab OOM at call N.
- `paging_force_fail_next()` / `paging_clear_force_fail()` — simulate paging OOM after
  kslab_alloc succeeds (exercises the KFrameMapping node cleanup path).

### New stress tests (FR-63..FR-69)

| FR | What it tests |
|----|--------------|
| FR-63 | `kframe_unmap_page` with mapping retain as last retain (regression for fixed bug) |
| FR-64 | kslab failure inside `kframe_map_page` → no PTE, counts unchanged |
| FR-65 | paging failure after kslab success → KFrameMapping freed, no PTE |
| FR-66 | Multi-page rollback simulation (mirrors `rollback_vmo_maps`) |
| FR-67 | `kvspace_obj_destroy` safety net with active mappings |
| FR-68 | 1000-cycle map/unmap stress on a single VA |
| FR-69 | mapping_count consistency with interleaved non-sequential unmap |

### Verification (Fase 6.4)

- `make` — 0 warnings, 0 errors
- `make test-unit` — 10274/10274 tests pass (FR-1..FR-69)
- `make smoke-runtime` — healthy runtime signature observed
- `ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests` — healthy runtime signature observed
