# Kernel Capacity Limits & the kslab Contract

Status: **DOCUMENTED and instrumented** (Fase 29).  Companion to
`resource-ownership-accounting.md`.

This document separates two kinds of ceiling that were previously conflated:

- **Resource-policy quotas** — per-process (per-domain) budgets, chargeable and
  observable, covered in `resource-ownership-accounting.md`.
- **Implementation capacity** — global, fixed-size kernel structures whose
  exhaustion is a system condition, not a per-domain charge.  The kernel object
  slab (`kslab`) is the main one.

---

## The kslab capacity contract

The kernel object slab is a single contiguous physical region reserved from the
PMM at boot.  All typed kernel object headers (KProcess, KVSpace, root KCNode,
page-table nodes, KEndpoint, KNotification, KFrameMapping, …) allocate from it,
so the rest of the PMM can be handed to userspace as untyped/frame caps.

| Property | Value / behaviour |
|----------|-------------------|
| **Sizing** | 16 MB (4096 pages), reserved once at boot via `pmm_alloc_pages(4096)`. 3% of the 512 MB guest. |
| **Reservation** | permanent for the kernel's lifetime; carved before any userland object exists. |
| **Alignment** | each allocation is aligned to its power-of-two size class. |
| **Classes** | 11 power-of-two classes, 32 B … 32 KB (`KSLAB_MIN_LOG2`..`KSLAB_MAX_LOG2`); the largest covers a `struct KProcess` with its embedded handle table. |
| **Allocation** | bump-pointer for fresh blocks; per-class free-lists for reuse of freed blocks. |
| **`used`** | the bump pointer (bytes consumed from the arena). It is **bump-only** — freed blocks return to per-class free-lists for reuse and never lower the bump pointer — so `used` **is** the high-water of arena consumption. |
| **Exhaustion** | when `aligned + block > total`, `kslab_alloc` increments `kslab_fail` and returns `NULL`. The caller returns `IRIS_ERR_NO_MEMORY` and rolls back atomically — **no corruption, no wedge**. |
| **Diagnostics** | `SYS_RESOURCE_INFO` exposes `kslab_used_bytes`, `kslab_total_bytes`, `kslab_hwm_bytes` (== used) and `kslab_alloc_failures`. |
| **Recovery** | freed blocks are reusable within their size class; the arena is never returned to the PMM. |
| **Growth** | fixed at boot; growing it means changing the boot reservation. Deriving it from build config / available RAM within safe bounds is a possible future refinement. |

### Why 16 MB (not a magic number)

Each spawned process consumes several KB–32 KB of kernel objects: a `KProcess`,
a 256-slot root `KCNode`, a `KVSpace`, a handful of page-table nodes, and its
handle table.  Supporting the phase's 32–64 concurrent-child target
(`KPROCESS_MAX_LIVE` = 64 total processes) plus the pager, the supervisor and
the core services requires roughly 64 × ~32 KB ≈ 2 MB of live objects, with
headroom for churn and free-list fragmentation.  16 MB gives comfortable margin
while remaining a trivial fraction of guest RAM.  It is a **capacity** bound,
deliberately distinct from the per-process VMO quota (restored to 32 once
children pay their own VMOs) — growing the arena is not a substitute for
correct ownership, and correct ownership is not a substitute for adequate
capacity.

### Exhaustion is tested deterministically

The exhaustion **path** — `kslab_alloc` returning `NULL` and the caller rolling
back cleanly with no corruption — is proven in the host unit suite via
fault injection: `kslab_fail_after(0)` forces the next allocation to fail, and
`test_kendpoint` / `test_kreply` / `test_kframe` assert that object creation is
atomic (no partial object, no leak, `IRIS_ERR_NO_MEMORY`).  Runtime T248
observes the capacity contract live (`used ≤ total`, zero failures under normal
load, bump-monotone `used`).  Exhausting the full 16 MB in every smoke run is
impractical and unnecessary given the path coverage.

---

## Other capacity limits

| Constant | Value | Class | Failure |
|----------|-------|-------|---------|
| `KPROCESS_MAX_LIVE` | 64 | implementation capacity (bounded by `TASK_MAX`) | atomic reserve fails → allocation rejected |
| `TASK_MAX` | 256 | implementation capacity | task create fails |
| `HANDLE_TABLE_MAX` | 256 | per-process capacity | `IRIS_ERR_TABLE_FULL` |
| `KCNODE_DEFAULT_SLOTS` | 256 | per-CNode capacity | slot mint fails |
| `KVMO_MAX_PAGES` | 16384 (64 MB/VMO) | per-object capacity | VMO create rejected |
| PCID pool | 1–4094 | hardware capacity | (cannot exhaust at 64 processes) |
| kernel object slab | 16 MB | global implementation capacity | `NULL` → `IRIS_ERR_NO_MEMORY` |

Resource-policy quotas (`KPROCESS_VMO_QUOTA`, `KPROCESS_NOTIFICATION_QUOTA`,
`KPROCESS_PHYS_PAGES_LIMIT`) are documented in
`resource-ownership-accounting.md`; they are per-domain budgets, not global
capacity.
