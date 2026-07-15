# Fase 26 — Memory object / VMO policy expansion

Status: ACCEPTED — implemented in this phase.  Companion to
`user-pager-vm-policy.md` (Fase 25), `vspace-frame-hardening.md` (Fase 19)
and `fault-endpoint-model.md` (Fase 20).  Fase 25 fixed the pager *authority*
contract with a raw frame as the page source; Fase 26 makes the source a
first-class memory object defended by policy.  Locked with runtime tests
T191–T200.

---

## Philosophy

A memory object is not "a thing you can map".  It is a defended source of
pages: a logical range with a stable size, page-granular offsets, monotonic
rights, counted mappings, and demonstrable cleanup.  In a real operating
system the difference between those two framings is the difference between a
kernel that hands out raw physical authority and one where every page a
process sees is traceable to an explicit capability over an explicit object.

Fase 25 proved a pager can resolve a fault with a raw frame.  That is too low
level to build a system on: a frame is one anonymous page with no identity,
no range, no offset, no sharing story.  Fase 26 gives the pager a *memory
object* to page from — the VMO — so that "resolve this fault" becomes "install
page N of this named object here, with these rights", and every part of that
sentence is checked.

The model in one line: **a memory object backs pages with explicit authority,
exact ranges and demonstrable cleanup.**

## Roles and objects

```text
VMO (memory object)   a logical region: fixed size, sparse per-page backing
                      (eager alloc on first touch — NO demand paging), an
                      owner retained for quota, monotonic rights, counted
                      mappings.  Destroyed only when no handle AND no mapping
                      retains it.

pager                 uses a VMO cap as its page source to resolve faults of a
                      specific target; holds a VMO cap + a target VSpace cap
                      (SYS_PROCESS_VSPACE) + fault authority — NO global frame
                      or untyped authority.

target                receives VMO-backed mappings in its VSpace; gains NO
                      authority over the VMO itself unless explicitly granted.

supervisor            creates/derives the VMO and the target VSpace cap, and
                      mints the pager's manifest.
```

## The map-into primitive (NEW, additive)

Before Fase 26 the only ways to install VMO pages were:

- `SYS_VMO_MAP` — map the WHOLE VMO from offset 0 into the CALLER's own VSpace;
- `SYS_VMO_MAP_INTO` — map the WHOLE VMO from offset 0 into a target's VSpace,
  gated on process `RIGHT_MANAGE`.

Neither is page-granular or offset-addressed, and `MAP_INTO` requires the
process MANAGE cap — not the delegated VSpace cap a Fase 25 pager holds.  A
VMO-backed pager needs to install *one* page, at *one* offset of the VMO, at
the *one* faulting VA, authorized by the *VSpace* cap.  That primitive did not
exist; Fase 26 adds it:

```text
SYS_VMO_MAP_PAGE(vmo_cptr, vspace_cptr, target_va, offset_flags)  → 0 or err   [108]
  vmo_cptr:     KOBJ_VMO, RIGHT_READ (+ RIGHT_WRITE for a writable PTE). dual resolver.
  vspace_cptr:  KOBJ_VSPACE, RIGHT_WRITE. dual resolver (Fase 25).
  target_va:    page-aligned VA in [USER_PRIVATE_BASE, USER_SPACE_TOP).
  offset_flags: [1:0]  = flags (bit0 W, bit1 X; W^X enforced)
                [11:2] = reserved, MUST be 0  (rejects an unaligned offset)
                [63:12]= page-aligned byte offset into the VMO
```

It is to `SYS_FRAME_MAP` what a VMO page is to a raw frame: identical authority
shape — the VSpace WRITE cap *is* the map-into-target authority, no process
MANAGE — composing directly with `SYS_PROCESS_VSPACE`.  It maps exactly ONE
page.  Sparse VMO pages are allocated and zeroed on first touch (eager, no
demand paging), charged to the CALLER's page quota exactly as `SYS_VMO_MAP`.
The installed PTE's rights are the meet of the VMO cap rights and the requested
flags; the mapping (and the VMO retain behind it) is swept on target VSpace
teardown exactly as for the whole-VMO path.

This is the ONLY new syscall; no existing number, ABI or semantic changed.

## Rights model

```text
VMO RIGHT_READ       map read-only; query size; be a share source (+DUPLICATE)
VMO RIGHT_WRITE      map writable (a writable PTE is refused without it)
VMO RIGHT_DUPLICATE  derive a reduced cap / SYS_VMO_SHARE source
VMO RIGHT_MANAGE     (unused on the VMO object itself today)
```

Rights are monotonic: `SYS_HANDLE_DUP` / `SYS_VMO_SHARE` can only drop rights,
never regain them (T191, T194).  A PTE never carries rights the VMO cap did not
grant, and never carries rights beyond the requested flags — the map is the
meet.  Critically, the PTE carries the MAPPING's rights, not the cap's ceiling:
a page mapped read-only through a fully-writable VMO cap still
write-protection-faults the target's store at the hardware level (T199).

## Offset / range contract

- The offset field is page-granular by construction (low 12 bits are flags),
  so an "unaligned offset" is a set reserved bit → `INVALID_ARG` (T192, M4).
- The addressed page must lie fully within the VMO: `offset < round_up(size)`,
  else `INVALID_ARG` (M5).  `offset == size` and `offset + page > size` are
  both rejected.
- `target_va` is validated by the standard user-window check (page-aligned,
  canonical, `[USER_PRIVATE_BASE, USER_SPACE_TOP)`): kernel and unaligned VAs
  are `INVALID_ARG` (M6, M7).
- An occupied `target_va` is `BUSY` (M8); no remap-over without an explicit
  unmap.

## Map / cleanup / lifetime contracts

- **Map** — page-granular, offset-addressed, authorized by (VMO READ[/WRITE])
  + (VSpace WRITE).  A denied map installs NOTHING: no PTE, no `mapped_count`
  bump, no VMO ref (T192, T198, M21).
- **Cleanup** — a VMO-backed mapping is a `KFrameMapping` in the target's
  `KVSpace`, exactly like `SYS_VMO_MAP_INTO`.  Each KFrame behind a VMO page
  retains the VMO; target VSpace teardown (`kvspace_invalidate`) sweeps every
  mapping, releases the frames, and drops the VMO retains (M17, M18).
- **Lifetime / revoke** — a VMO is destroyed only when NO handle AND NO mapping
  retains it (M22).  Closing the supervisor's last handle while a mapping is
  live does not destroy the VMO; the object dies only after the last mapping is
  swept (T196).  There is no force-unmap-on-revoke: revoke is cap-scoped
  (dropping a handle drops that reference), and the physical pages live with
  the VMO until it is destroyed — freed exactly once (M22, no double free).
- **Shared** — one VMO mapped into two targets yields two independent
  `KFrameMapping`s over the same physical pages.  A writable share makes one
  target's store visible to the other's later read and to the supervisor; each
  target's teardown sweeps only its own mapping (T195, M27, M28).  (Writable
  sharing is coherent because both mappings point at the same VMO page; there
  is no copy.)

## Pager / target death contracts (inherited + extended)

- **Target death during resolution** — the target VSpace is invalidated with
  the process; a late `SYS_VMO_MAP_PAGE` is `BAD_HANDLE`, a late resume is
  `NOT_FOUND`, the fault record is gone.  The VMO survives (its caps are the
  supervisor's), with its pages intact and reusable (T193, T200 op3, M24).
- **Pager death while a fault is pending** — inherits the Fase 25 contract: the
  target stays suspended-alive with record and generation intact; the VMO stays
  live with no ghost refs; a restarted pager with the same manifest (its page
  source is the VMO — no untyped or global frame authority) completes the
  resolution from the VMO (T197, M25).  Crash-loops are contained by Fase 24
  supervision.

## Interaction with the rest of the system

- **User pager (Fase 25)** — unchanged and reused verbatim.  The pager manifest
  and harness are identical; only the slot-14 "page source" cap changes from a
  KFrame to a KVmo, and a new serve subaction maps a VMO page.  Fault
  generations, seq-checked resume, `SYS_PROCESS_VSPACE` and the dual-resolver
  VSpace argument all carry over untouched.
- **VSpace / frame (Fase 19)** — `SYS_VMO_MAP_PAGE` installs through the same
  `kframe_map_page` path as every other map; all V-invariants (W^X, occupied
  VA, no partial PTE, mapped_count discipline) apply unchanged.  A VMO page
  becomes a `KFrame` (`kframe_alloc_vmo_page`) exactly as in `SYS_VMO_MAP`.
- **Fault endpoint (Fase 20)** — no change.  The write-protection fault that
  proves RO-PTE enforcement (T199) is the same hardware observable as Fase 20.
- **Supervision (Fase 24)** — a VMO-backed pager is a supervised service like
  any other; restart least-authority holds with a VMO source (T197).
- **IPC / scheduler / lifecycle** — no semantic change; pager traffic is
  notification signals plus ordinary syscalls; KReply and live books stay at
  baseline in every test.

## Instrumentation

One additive gauge, no new tier: `kvmo_live_count()` is exposed at
`SYS_SCHED_INFO` **offset 132** — the previously-zero pad half of the Fase 18
authority word (`buf[16]`), already inside the `EXT3` tier.  A reader asking
for `EXT3_BYTES` (136) already receives it; pre-Fase-26 readers that stopped at
offset 132 are unaffected.  This deliberately avoids growing the tier count
(Fase 23 documented that adding an `EXT6` tier hung the boot).  No other
instrumentation was needed: frame/mapping/VSpace live gauges, the fault
counters and per-process records cover the rest.

## Tests (runtime, deterministic)

```text
T191  VMO authority & size: stable READ-gated size; RO derivation cannot map
      writable; wrong-type in either slot; stale (closed) cap fails clean;
      vmo_live tracks create/destroy; rights monotonic.
T192  offset/range validation: unaligned offset, offset==size, offset>size,
      kernel VA, unaligned VA, W^X — all INVALID_ARG with no PTE; a valid map
      then succeeds; occupied VA BUSY; books at baseline.
T193  VMO-backed pager resolves a fault: RO map → target reads the supervisor's
      pattern (data flows VMO→target); W map → target store lands in the VMO;
      target death sweeps the mapping; VMO reusable; delivery/resume counts.
T194  unauthorized VMO pager: RO VMO cap cannot map writable into the target;
      RO VSpace cap cannot install anything; authorized map proves the denials
      were the rights; no ref leak, no fault-state corruption.
T195  shared mappings across two targets: A writes the shared VMO page (visible
      to the supervisor and to B's later read); A dies without disturbing B; B
      dies; independent cleanup; VMO live until the supervisor closes it.
T196  revoke/destroy with active mappings: VMO not destroyed while retained by
      a handle; closing the last handle after the mapping is swept destroys it
      exactly once; a stale cap fails clean; no double free.
T197  pager death/restart with a VMO source: fault survives pager death; VMO
      stays live; restarted instance reports exactly the manifest (no untyped/
      global frame); serving generation resolves from the VMO.  (Fase24↔26.)
T198  partial-failure atomicity: a battery of denied maps (bad offset, kernel
      VA, occupied, RO-writable, stale cap) leaves the space and every book
      unchanged; a valid map before and after proves no corruption.
T199  rights/PTE policy: a RO PTE installed via a WRITABLE VMO cap still
      write-protection-faults the target's store (err P|W|U); occupied remap
      BUSY; no silent write, no escalation.
T200  seeded stress (seed printed only on failure): 6 rounds over the whole
      surface — VMO-backed read/write resolve, pager death + supervisor
      takeover, target death mid-fault, failure injection — with VMO/frame/
      mapping/handle/process books at baseline every round.
```

`lifecycle_probe` gained `LP_CMD_PAGER_SERVE` subaction 4 (map a VMO page via
`SYS_VMO_MAP_PAGE` at the fault, then seq-resume); slot 14 is now the "page
source" cap and holds a KVmo instead of a KFrame.

## Deliberate limits (scope, not gaps)

- **No swap, no copy-on-write, no filesystem-backed paging, no global page
  cache, no overcommit, no shared libraries, no POSIX mmap.**  This phase makes
  the VMO a defended page source; backing-store and sharing policy build on top
  of exactly these primitives.
- **No demand paging.**  Sparse VMO pages are allocated eagerly on first map
  (Fase 6.1 removed fault-driven demand paging; FR-41 regression).  A pager
  resolves faults, but the VMO page itself is materialised at map time, not on
  a second fault.

## Remaining gaps (honest)

- **Whole-VMO `SYS_VMO_MAP_INTO` still needs process MANAGE**; only the new
  page-granular path uses the delegated VSpace cap.  Unifying the two (an
  offset+length range map authorized by a VSpace cap) is a clean follow-up but
  was not needed for the pager.
- **Page quota is charged to the mapper**, matching `SYS_VMO_MAP`; the sparse
  page then lives with the VMO until the VMO is destroyed.  A per-VMO quota
  owner model (charge the VMO owner, release at destroy) is a more honest
  accounting but is a separate, larger change.
- **No writable-share coherence beyond same-VMO-page aliasing** — there is no
  copy, so shared writable pages are trivially coherent, but there is also no
  COW to diverge them; that is the copy-on-write follow-up.
- **No offset+length (multi-page) map-into** — the pager resolves one page per
  fault, so a single-page primitive suffices; a range variant is additive when
  a bulk consumer appears.

## Fase 28 Bloque B — file-backed page vehicle (implemented)

`SYS_VMO_MAP_PAGE` is the sole kernel mechanism the file-backed pager needs.
The pager fills a page by mapping a VMO grant page into its OWN VSpace
(`SYS_VSPACE_SELF` + a scratch VA) writable, zeroing it, copying the file bytes
(leading = file, trailing = zero), then unmapping — and maps that page into the
target at the fault VA read-only, or `MAP_EXEC` for an RX segment, or writable
for a private-writable page.  W^X is enforced by the kernel on the PTE and by
the pager at region registration (no `W&X`).  No new syscall, no kernel
filesystem policy: the file→page relationship is entirely userland policy over
VMO/VSpace/frame caps.  See `file-backed-memory.md`.

---

## Fase 29 addendum — VMO ownership & payer

A VMO's quota (the object and its sparse pages) is charged to an explicit
**owner / payer domain**, selected by capability authority at creation, not to
whoever ran the syscall:

- `SYS_VMO_CREATE(size)` — charges the caller (unchanged 1-arg ABI).
- `SYS_VMO_CREATE_FOR(size, charge_target)` — charges `charge_target`, a
  KProcess the caller holds RIGHT_MANAGE on.  A loader uses this to charge a
  child's image VMOs to the CHILD, so the loader's `owned_vmos` stays flat
  regardless of how many children it launches.

Sparse physical pages are charged **once to the VMO owner** at page allocation
(`kvmo_owner(v)` in `sys_vmo_map` / `sys_vmo_map_into` / `sys_vmo_map_page`) and
released at `kvmo_destroy`.  A shared VMO's pages are paid once by its owner;
extra targets that map it do not re-charge, and unmapping never strands the
charge on the mapper.  See `resource-ownership-accounting.md` (T239–T250).
