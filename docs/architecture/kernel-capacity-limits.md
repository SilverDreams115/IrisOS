# Kernel Capacity Limits & the kslab Contract

Status: **DOCUMENTED and instrumented** (Phase 29; updated at Stage 7 close).
Companion to `resource-ownership-accounting.md`.

This document separated two kinds of ceiling that were previously conflated.
**Only one of the two still exists.**

- **Resource-policy quotas** — per-process (per-domain) budgets.  **All
  retired** (Phase S1, Stage 7, Stage 7-mem): there is no resource domain, and
  what an allocation may spend is the `Untyped` it names.  The history is in
  `resource-ownership-accounting.md`.
- **Implementation capacity** — global, fixed-size kernel structures whose
  exhaustion is a system condition, not a per-domain charge.  The kernel object
  slab (`kslab`) is the main one, and since Stage 7 it is the only ceiling of
  either kind that the kernel sets for itself outside `TASK_MAX`.

---

## The kslab capacity contract

The kernel object slab is a single contiguous physical region reserved from the
PMM at boot.  The typed kernel object headers that are still fabricated rather
than retyped (KVSpace, the root task's KCNode, KFrameMapping, KVMO metadata,
the boot control capabilities, …) allocate from it, so the rest of the PMM can
be handed to userspace as untyped/frame caps.  Which files may do so, and how
many times, is frozen in `scripts/purity_allowlist.txt` — 14 files, 17
permitted `kslab_alloc` occurrences — and `make check-purity` fails on the
fifteenth.

| Property | Value / behaviour |
|----------|-------------------|
| **Sizing** | 16 MB (4096 pages), reserved once at boot via `pmm_alloc_pages(4096)`. 3% of the 512 MB guest. |
| **Reservation** | permanent for the kernel's lifetime; carved before any userland object exists. |
| **Alignment** | each allocation is aligned to its power-of-two size class. |
| **Classes** | 11 power-of-two classes, 32 B … 32 KB (`KSLAB_MIN_LOG2`..`KSLAB_MAX_LOG2`).  The largest was sized for `struct KProcess` and its embedded handle table; both are deleted, and the class is now headroom. |
| **Allocation** | bump-pointer for fresh blocks; per-class free-lists for reuse of freed blocks. |
| **`used`** | the bump pointer (bytes consumed from the arena). It is **bump-only** — freed blocks return to per-class free-lists for reuse and never lower the bump pointer — so `used` **is** the high-water of arena consumption. |
| **Exhaustion** | when `aligned + block > total`, `kslab_alloc` increments `kslab_fail` and returns `NULL`. The caller returns `IRIS_ERR_NO_MEMORY` and rolls back atomically — **no corruption, no wedge**. |
| **Diagnostics** | `SYS_UNTYPED_QUERY` kind `GLOBAL` exposes `kslab_used_bytes`, `kslab_total_bytes` and the failed-allocation counter.  (`SYS_RESOURCE_INFO` carried them until Stage 7-mem retired it; they moved rather than went, because they were never per-process facts.) |
| **Recovery** | freed blocks are reusable within their size class; the arena is never returned to the PMM. |
| **Growth** | fixed at boot; growing it means changing the boot reservation. Deriving it from build config / available RAM within safe bounds is a possible future refinement. |

### Why 16 MB (not a magic number)

The arena sizing predates Stages 6–7 and is now a statement about the ROOT
TASK and the kernel's own objects, not about spawned processes.  A spawned
child's root `KCNode`, `KVSpace`, TCB, PML4 and every paging level are child
blocks of an Untyped its creator named (Stage 6 Steps 3–4, Stage 6-pure
Steps 1/4/5), so none of it touches this arena: more than 64 live children cost
the kslab nothing, which T304 demonstrates.  What is left here is the root
task's slab objects, the boot Untyped headers and the subsystems the purity
allowlist still freezes.  16 MB is generous for that and is kept as headroom
rather than re-derived downward, because shrinking it would be a capacity
change with no correctness argument behind it.  Growing the arena was never a
substitute for correct ownership, which is the argument that outlived the
quotas: the answer to "who pays" is now a capability, and this arena is what
pays for the objects created before any capability exists.

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
| `TASK_MAX` | 256 | implementation capacity — the scheduler's thread registry | thread configure/resume fails |
| `KCNODE_DEFAULT_SLOTS` | 256 | per-CNode capacity | slot mint fails |
| `KVMO_MAX_PAGES` | 16384 (64 MB/VMO) | per-object capacity | VMO create rejected |
| PCID pool | 1–4094 | hardware capacity | address-space retype fails cleanly — reachable since the live-process ceiling retired, and handled |
| kernel object slab | 16 MB | global implementation capacity | `NULL` → `IRIS_ERR_NO_MEMORY` |
| `KPROCESS_MAX_LIVE` | **retired (Stage 7 Step 3)** | — | the creator's Untyped runs out |
| `HANDLE_TABLE_MAX` | **removed (Stage 4)** | — | there is no handle table |

Every resource-policy quota is gone: `KPROCESS_NOTIFICATION_QUOTA` in Phase S1,
`KPROCESS_PHYS_PAGES_LIMIT` in Stage 7 Step 2, `KPROCESS_VMO_QUOTA` in Stage
7-mem with the owner relation it counted against.  What a task may spend is the
`Untyped` it holds, reported by `SYS_UNTYPED_INFO` / `SYS_UNTYPED_QUERY`; the
history of the domain model is in `resource-ownership-accounting.md`.
