# Fase 6.3 Final Report — VMO-to-Frame Capability Migration

## 1. Goal

Migrate `sys_vmo_map` / `sys_vmo_map_into` from raw `paging_map_checked_in` + per-process
`KVmoMapping` tracking to the full KFrame capability path — the same path used by bootstrap
mappings since Fase 6.2.  After this migration every user-space page, without exception, is
owned by a `KFrame` capability whose lifetime is tracked by reference counts.

## 2. Motivation

After Fase 6.2 bootstrap frames were KFrame-backed but runtime VMO maps were not.  The two
tracking systems were inconsistent:

- Bootstrap frames: KVSpace.mappings linked list → auto-unmapped by `kvspace_invalidate`.
- VMO frames: KProcess.vmo_mappings list → manually cleared by `kprocess_clear_vmo_mappings`.

The split meant that `kvspace_invalidate` only saw half the active mappings, `mapped_count`
values were wrong for VMO-backed frames, and a PMM page could in principle be freed while a
live PTE still referenced it if the teardown ordering was wrong.

## 3. Deleted Code

| Symbol / File | Reason |
|---------------|--------|
| `struct KVmoMapping` | Replaced by `KFrameMapping` in KVSpace |
| `KProcess.vmo_mappings` | All mappings now in KVSpace |
| `KProcess.vmo_mapping_count` | Field removed |
| `kprocess_register_vmo_map` | Callers now call `kframe_map_page` |
| `kprocess_unregister_vmo_map` | Callers now call `kvspace_unmap_page` |
| `kprocess_clear_vmo_mappings` | Superseded by `kvspace_invalidate` |
| `KPROCESS_VMO_MAP_MAX` | No fixed slot limit |
| `KVSPACE_MAPPING_SLOTS` | Replaced by dynamic linked list |
| `rollback_user_maps` (syscall_priv.h) | Replaced by `rollback_vmo_maps` |
| Diagnostic `T`/`U` serial writes in user_trampoline.S | Removed (polluted klog drain output) |

## 4. New Code

| Symbol | Location | Purpose |
|--------|----------|---------|
| `kframe_alloc_vmo_page(paddr, vmo)` | kframe.c | Creates KFrame with `vmo_owner` retain |
| `KFrame.vmo_owner` | kframe.h | Holds VMO alive until frame is destroyed |
| `kvspace_unmap_page(vs, va)` | kvspace.c | Removes one mapping node, PTE, and frame retain |
| `rollback_vmo_maps(vs, start, end)` | syscall_vm.c | Error-path unmap loop using `kvspace_unmap_page` |
| `kvmo_make_stub()` | tests/kernel/stubs.c | Minimal KVmo for unit tests (no PMM) |

## 5. Changed Code

### kernel/new_core/include/iris/nc/kframe.h
- Added `struct KVmo *vmo_owner` field.
- Added `kframe_alloc_vmo_page` declaration.

### kernel/new_core/include/iris/nc/kvspace.h
- Removed `KVSPACE_MAPPING_SLOTS`.
- Changed `mappings[KVSPACE_MAPPING_SLOTS]` fixed array to `struct KFrameMapping *mappings`
  singly-linked list head.
- Added `next` pointer to `struct KFrameMapping`.
- Added `kvspace_unmap_page` declaration.

### kernel/new_core/include/iris/nc/kprocess.h
- Removed `KPROCESS_VMO_MAP_MAX`, `struct KVmoMapping`, `vmo_mappings`, `vmo_mapping_count`.
- Added `#define KPROCESS_BOOTSTRAP_FRAME_MAX 32u`.
- Removed `kprocess_register_vmo_map` / `kprocess_unregister_vmo_map` declarations.

### kernel/new_core/src/kvspace.c
- `mappings` list is now a singly-linked list of slab-allocated `KFrameMapping` nodes.
- `kvspace_invalidate` takes the list under lock, processes it outside (lock-free traversal).
- `kvspace_obj_destroy` safety-net loop updated for linked list.
- `kvspace_unmap_page` added.

### kernel/new_core/src/kframe.c
- `kframe_alloc` sets `vmo_owner = NULL`.
- `kframe_obj_destroy` releases `vmo_owner` via `kobject_release` after `kslab_free`.
- `kframe_alloc_vmo_page` added.
- `kframe_map_page` allocates `KFrameMapping` node before acquiring lock; appends to linked list.
- `kframe_unmap_page` scans list by VA+frame (for use by internal callers; external callers use
  `kvspace_unmap_page`).

### kernel/new_core/src/kprocess.c
- Removed `kprocess_clear_vmo_mappings`, `kprocess_register_vmo_map`,
  `kprocess_unregister_vmo_map`.
- Removed `kprocess_clear_vmo_mappings(p)` call from teardown.
- `kprocess_register_bootstrap_frame` limit uses `KPROCESS_BOOTSTRAP_FRAME_MAX`.

### kernel/core/scheduler/task_lifecycle.c
- Guard changed from `KVSPACE_MAPPING_SLOTS` to `KPROCESS_BOOTSTRAP_FRAME_MAX`.

### kernel/core/syscall/syscall_vm.c
- `sys_vmo_map` (demand path): eagerly allocates PMM pages, creates KFrames via
  `kframe_alloc_vmo_page`, maps via `kframe_map_page`.
- `sys_vmo_map` (MMIO path): creates KFrames via `kframe_alloc(phys, 4096, NULL)`.
- `sys_vmo_map_into`: mirrors `sys_vmo_map` for the child process VSpace.
- `sys_vmo_unmap`: calls `kvspace_unmap_page` per page.
- `rollback_vmo_maps` static helper added (replaces `rollback_user_maps`).
- Added `#include <stddef.h>` for `NULL` (gcc freestanding mode).

### kernel/core/syscall/syscall_proc.c
- `sys_process_create`: calls `kvspace_alloc(proc->cr3)` after creating the page table,
  assigns result to `proc->vspace`.  Without this, `sys_vmo_map_into` would reject the child.

### kernel/core/syscall/syscall_priv.h
- Removed `rollback_user_maps`.
- Added `#include <iris/nc/kvspace.h>` (needed by syscall_proc.c for `kvspace_alloc`).

### kernel/core/phase3_selftest.c
- Removed `kprocess_register_vmo_map` calls and `vmo_mapping_count` check.
- `phase3_process_selftest` now verifies exception channel lifecycle and basic VMO page state.

### tests/kernel/test_kframe.c
- FR-40 rewritten: tests dynamic pool supports 64 pages (no fixed slot limit).
- FR-47 rewritten: `bootstrap_kframe_map` returns NULL for duplicate VA.
- FR-51..FR-62 added: VMO-to-Frame tests using `kvmo_make_stub()`.

## 6. Two Root Causes Found During Smoke-Runtime Debugging

### Bug 1: `sys_process_create` never created a KVSpace

`kprocess_alloc` zero-initialises `proc->vspace`.  `sys_process_create` set `proc->cr3` but
never called `kvspace_alloc`.  When `sys_vmo_map_into` checked `proc->vspace`, it found NULL
and returned `IRIS_ERR_INVALID_ARG`, causing the first `SYS_VMO_MAP_INTO` call (stack VMO) to
fail and userboot to exit.

Fix: added `kvspace_alloc(proc->cr3)` + `proc->vspace = vs` in `sys_process_create`.

### Bug 2: Diagnostic `T`/`U` serial chars interleaved with klog drain output

`user_trampoline.S` emitted `T` (trampoline entered) and `U` (IRETQ) directly to the serial
port on every user-task context switch.  With eager KFrame allocation, boot takes slightly
longer (more PMM calls), causing more context switches during klog drain.  One switch happened
mid-message, splitting `[IRIS][SCHED] running` into `[IRIS]TUTU[SCHED] running` — breaking the
`grep -F "[IRIS][SCHED] running"` smoke-test assertion.

Fix: removed the `T`/`U` serial writes from `user_trampoline.S` (the comment already said
"remove after smoke test diagnosis").

## 7. Rule Compliance

| Rule | Status |
|------|--------|
| No demand paging | ✓ |
| No mapping user pages from #PF | ✓ |
| No usercopy demand allocation | ✓ |
| No raw paging_map_checked_in in user runtime paths | ✓ eliminated |
| No breaking SYS_FRAME_MAP | ✓ |
| No breaking SYS_FRAME_UNMAP | ✓ |
| No breaking KFrame mapped_count | ✓ |
| No breaking KVSpace mapping_count | ✓ |
| No breaking kvspace_invalidate auto-unmap | ✓ now covers VMO pages too |
| No breaking bootstrap KFrame-backed mappings | ✓ |
| No breaking T008 | ✓ T001–T017 pass |
| No breaking FR-41 | ✓ FR-1..FR-62 pass |
| No breaking userboot/init/svcmgr/VFS/kbd/sh | ✓ full runtime boot verified |
| No breaking KBootstrapCap slot 1 | ✓ |
| No breaking KVSpace slot 2 | ✓ |
| No breaking Boot KUntyped slots 16–255 | ✓ |
| No touching KChannel | ✓ |
| No eliminating handle_id_t | ✓ |
| No activating SMP | ✓ |
| No accepting if smoke-runtime fails | ✓ passes |

## 8. Verification Results

| Check | Result |
|-------|--------|
| `make` | 0 errors, 0 warnings |
| `make test-unit` | 2143 passed, 0 failed |
| `make smoke-runtime` | healthy runtime signature observed |
| `ENABLE_RUNTIME_SELFTESTS=1 make smoke-runtime-selftests` | healthy runtime signature observed |

## 9. Architecture After Fase 6.3

Every user-space page in every IRIS process is now owned by a `KFrame` capability:

```
KVmo.pages[i] (phys addr)
    └── KFrame (via kframe_alloc_vmo_page or kframe_alloc)
            └── KFrameMapping (in KVSpace.mappings linked list)
                    └── PTE (installed by paging_map_checked_in)
```

Teardown path (process exit or explicit unmap):
```
kvspace_invalidate / kvspace_unmap_page
    → remove KFrameMapping node, unmap PTE
    → kobject_release(KFrame)  [mapped_count reaches 0 first]
    → kframe_obj_destroy → kobject_release(KVmo)
    → kvmo_destroy → pmm_free_page (if owned)
```

There are no raw `paging_map_checked_in` calls on any user-space mapping path.
The page-fault handler never allocates pages.

## 10. Related Documents

- `docs/vmo-memory.md` — complete VMO memory model reference
- `docs/bootstrap-memory.md` — bootstrap mapping history; updated for Fase 6.3
