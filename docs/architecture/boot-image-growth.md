# Fase 28 Bloque A — Boot image growth hardening

Status: ACCEPTED — root cause found and fixed in this phase.  Companion to
`service-pager-integration.md` (Fase 27, which discovered the symptom) and
`file-backed-memory.md` (Fase 28 Bloque B, which the fix unblocks).  Locked
with runtime tests T211–T216.

---

## The symptom (Fase 27)

Adding a tenth-and-beyond initrd image "silently wedged" the boot: the kernel
reached the scheduler, but no userland service ever produced output, and there
was no panic.  It blocked promoting the pager to its own binary, growing the
initrd, and building any backing/file service.

## Root cause

It was **not** a memory, alignment, allocator, page-table, or physical-layout
bug.  The decisive experiment: a **256-byte** dummy blob as the extra image
wedged boot too, and `__data_start` / `__kernel_end` were **byte-identical**
with and without it (`0x283000` / `0x43b000`).  Zero layout shift, same wedge —
so it could not be a placement-sensitive structure.

The cause was an **over-strict boot assertion** in `userboot`:

```c
/* old */
if (svc_initrd_count(bootstrap_cap_h) != (long)SL_CATALOG_COUNT)
    goto fail;   /* fail: SYS_EXIT(1) */
```

`userboot` is the root bootstrap task.  It required the kernel initrd image
count to EQUAL a hardcoded catalog size (`SL_CATALOG_COUNT`), and on ANY
mismatch it took the `fail` path — `SYS_EXIT(1)` — **before loading init**.  So
adding any image (making count ≠ SL_CATALOG_COUNT) caused userboot to exit
immediately; with no init, no services ever started, and the kernel idled
forever.  No deadlock, no corruption — a fail-stop safety check that was both
too strict (exact equality) and invisible (no diagnostic).

### Why it looked like a silent wedge

`klog` is a ring buffer only; kernel `[IRIS]` lines reach the serial console
solely because `svcmgr` drains klog and prints it.  When userboot exits before
`svcmgr` starts, that drain never happens, so the serial log stops after the
last direct-`serial_write` kernel line ("waiting for first userland wave") with
no error — the classic "silent wedge" appearance.  (A temporary
`IRIS_KLOG_SERIAL_MIRROR` build option — a compile-time klog→COM1 mirror, off
by default — made the kernel klog visible during diagnosis and is kept in
`klog.c` for future boot debugging.)

## The fix

Two changes, both in the boot path:

1. **Relax the assertion to `>=`.**  The ring-3 name→index catalog
   (`svc_loader.c`) maps `SL_CATALOG_COUNT` names to fixed indices
   `0..SL_CATALOG_COUNT-1`; the invariant is that the kernel initrd has AT
   LEAST those images (so every named index resolves), not exactly that many.
   The initrd may hold additional images at higher indices (new services,
   backing blobs, test fixtures); those are not named by the ring-3 catalog
   and are loaded by index or other means.

   ```c
   /* new */
   if (svc_initrd_count(bootstrap_cap_h) < (long)SL_CATALOG_COUNT) {
       ub_boot_panic(bootstrap_cap_h, "[USERBOOT] FATAL: initrd catalog "
                     "too small (kernel/ring-3 mismatch); halting boot\n");
       goto fail;
   }
   ```

2. **Make the remaining genuine failure visible.**  A real shortage
   (`count < SL_CATALOG_COUNT`, i.e. a kernel↔ring-3 catalog mismatch that
   would make a named index read garbage) is bootstrap-fatal.  `userboot` holds
   the root `KBootstrapCap` (HW_ACCESS), so it mints a serial `KIoPort` and
   emits a diagnostic line to COM1 before exiting — never a silent dead system
   again.

`SL_CATALOG_COUNT` was bumped to 11 (index 10 = the promoted `pager` binary).

## Boot-growth limit model

| Resource | Limit | Enforcement | Failure behavior |
|---|---|---|---|
| initrd image count | none (kernel `g_initrd[]` is `sizeof`-derived) | n/a | growth is unbounded by construction |
| named catalog images | `SL_CATALOG_COUNT` (11) | `userboot` `>=` check | `< ` → serial diagnostic + halt (explicit) |
| named index resolution | index `< count` | kernel `initrd_get` bounds | out-of-range → `NOT_FOUND` (clean) |
| per-image size | `KVMO_MAX_SIZE` (64 MiB) | `kvmo_size_to_pages` | oversize → `INVALID_ARG` |
| ELF segments per image | `SL_MAX_SEGS` (8) | loader clamp + validation | excess/invalid → `INVALID_ARG`, atomic cleanup |
| bad/malformed image | n/a | loader ELF validation | `INVALID_ARG`, no ghost process/task/VSpace/CSpace |

There is **no fixed-size image array** in the boot path, so there is no "Nth
image" cliff.  The only image-count contract is the `>=` catalog check, which
fails explicitly, never silently.

## Rules for adding initrd images

1. Append to `kernel/core/initrd/initrd.c` `g_initrd[]` (index = position) and
   declare its `_binary_..._start/_end` symbols.
2. Add the Makefile `objcopy` rule and link the `*_bin.o` into `KERNEL_OBJS`.
3. If the image is loadable by name, add it to `svc_loader.c`
   `sl_name_to_index` AND bump `SL_CATALOG_COUNT` (it is part of the named
   catalog, so the boot invariant covers it).  If it is a non-named blob
   (backing/fixture), do NOT bump `SL_CATALOG_COUNT` — it lives at an index
   `>= SL_CATALOG_COUNT` and is reached by index.
4. If ring-3 services enumerate it, respect their own clamps (e.g. VFS exports
   the first `VFS_INITRD_NAME_COUNT` images by name).

## Checkpoints (boot progress)

The kernel emits raw-serial breadcrumbs (`_early_putc`) at: kernel_main (`K`),
serial init (`S`), boot-info (`B`), pmm_init (`P`), paging_init (`G`), paging
done (`g`).  Past that, progress is klog (visible via svcmgr drain, or the
`IRIS_KLOG_SERIAL_MIRROR` build option): SCHED init, bootstrap task, boot
untyped drain, SCHED running.  The first userland serial output is a service's
own COM1 write (console/svcmgr) once init spawns them.  A boot that stops
after "waiting for first userland wave" with no service output means the root
bootstrap task exited before loading init — now accompanied by an explicit
diagnostic.

## Tests (runtime, deterministic)

```text
T211  image-count boundary: count is queryable and >= the named catalog; every
      in-range index yields a live initrd VMO with positive size; out-of-range
      indices are NOT_FOUND, never a wedge.  (That this suite runs at all proves
      boot reached userland with > SL_CATALOG_COUNT images.)
T212  aggregate-size boundary: every image's VMO has a correct, non-overflowing
      size and maps into the caller's VSpace at its true size — no truncation,
      no aliasing between images.
T213  loader launch growth: a valid launch works; the invalid-ELF fixture fails
      INVALID_ARG with full atomicity (no ghost process/task/VSpace/CSpace/
      handle); a valid launch AFTER the failure still works.
T214  failure diagnostics: unknown name → NOT_FOUND, malformed image →
      INVALID_ARG; both explicit, neither blocks; the initrd VMO of the invalid
      image still succeeds (the failure is the loader's, cleanly reported, not
      the initrd layer's).
T215  pager binary promotion: the pager loads by name as its own binary,
      reports exactly its grant manifest (no authority from being standalone),
      and resolves a VMO-backed fault end to end.
T216  seeded boot-growth stress: a matrix of map/query/valid-load/invalid-load
      ops over the grown initrd — every op succeeds or fails explicitly, no
      wedge, no drift.
```

## Fixtures

- `services/pager/` — the pager promoted to its own binary (initrd index 10,
  named "pager", part of `SL_CATALOG_COUNT`).
- `services/bootfix/badelf.bin` — a 256-byte non-ELF blob (initrd index 11,
  named "badelf" for the loader-failure tests; NOT part of `SL_CATALOG_COUNT`).

## Remaining gaps

- The root-task failure diagnostic is emitted by `userboot` via a minted serial
  cap; the kernel does not itself detect "root bootstrap task exited before
  spawning userland" and panic.  That would be a stronger, image-independent
  safety net (any root-task early exit becomes a diagnostic panic) and is a
  candidate for a future kernel-hardening step.
- 32-image scaling is supported by construction (no fixed array); it is
  demonstrated here at 12 images (past the old 10-image cliff) rather than by
  shipping 20 dummy blobs.  Adding more is a mechanical fixture addition.

## Update (Fase 28 Bloque B): initrd at 16 images

Bloque B added four content fixtures (`fbk.dat`, `fbk2.dat`, `elfseg.dat`,
`small.dat`, generated reproducibly by `scripts/gen_fixtures.py`) as initrd
indices 12–15, taking the initrd to **16 images** — exercised live by the file
-backed suite (T217–T230) and the growth suite (T211–T216).  Growth remained a
no-op by construction: no fixed array, `userboot` gates on `count >=
SL_CATALOG_COUNT`, and VFS exports the fixtures by name past its 8-image name
clamp.  The 10-image cliff is decisively behind us.
