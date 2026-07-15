# Fase 28 Bloque B — File-backed memory (architecture + implementation)

Status: **IMPLEMENTED and tested end to end** (runtime T217–T230, all green in
the 226/226 suite).  Bloque A (boot-growth hardening) unblocked this work; Bloque
B is the complete file-backed memory subsystem — identity + generations, bounded
per-backing grants, validated regions, RO-shared / private-writable modes,
exact EOF/zero-fill, a bounded evicting page cache, revocation, restart, failure
atomicity, and ELF-segment groundwork.  The read-only path needed **no new
kernel syscall** (it composes from Fase 25/26 primitives); the whole subsystem
lives in the ring-3 pager service.  Companion to `boot-image-growth.md`,
`service-pager-integration.md` and `memory-object-vmo-policy.md`.

## Implementation map (where each piece lives)

| Concern | Code |
|---|---|
| Wire contract (ops, structs, modes, error markers) | `services/pager/pager_proto.h` |
| Backing table, region table, cache, resolution | `services/pager/main.c` |
| VFS page-read (`READ_AT` loop, ≤16 reads/page) | `pg_read_file` |
| Page fill (zero → file bytes → unmap) | `pg_fill_page` |
| Bounded RO cache (find / deterministic alloc / evict) | `pg_cache_find`, `pg_cache_alloc` |
| Fault resolution (RO-shared / private / mode reject) | `pg_resolve_region` |
| Registration + atomic validation + W^X | `pg_register_backing`, `pg_register_region` |
| Revocation (generation bump, drop unreferenced) | `pg_revoke_backing` |
| Content fixtures (deterministic patterns) | `services/filebk/*.dat`, `scripts/gen_fixtures.py` |
| VFS export of fixtures by name | `services/vfs/vfs.c` (`vfs_seed_fixture_exports`) |
| End-to-end tests | `services/iris_test/main.c` (T217–T230) |

---

## Architectural principle (B0)

The kernel must NOT understand pathnames, inodes, directories, filesystem
permissions, page-cache policy, or file generations.  It keeps understanding
VMO, frame/page, VSpace, mapping, rights, fault, capability.  The file→page
relationship is **userland policy**, split between the VFS/backing service, the
pager service, and the supervisor, over VMO grants.  No kernel extension in
this block may introduce filesystem policy.

The good news from the audit: **no new kernel mechanism is needed for the
read-only path.**  It composes from existing primitives:
`SYS_VMO_MAP_PAGE` (Fase 26), `SYS_PROCESS_VSPACE` (Fase 25), fault generations
and seq-checked resume (Fase 25), and the VFS `STAT`/`READ_AT` endpoint
protocol (Fase 7).

## B1 — VFS / backing audit

| Surface | Current behavior | Needed contract | Coverage | Risk |
|---|---|---|---|---|
| VFS export table | first `VFS_INITRD_NAME_COUNT` (8) initrd images, mapped into VFS at boot | export additional backing blobs by name | T026/T034 (lookup) | clamp is fixed; add fixtures + names |
| `VFS_EP_OP_STAT` | name → size | backing identity (name) + size; add generation word | endpoint tests | no generation field yet |
| `VFS_EP_OP_READ_AT` | (name, offset, len) → bytes + total size; EOF = 0 bytes | page-fill source; short read = EOF | endpoint tests | 256-byte cap → 16 reads/page |
| `IRIS_IPC_BUF_SIZE` | 256 bytes per reply | fill a 4 KiB page in ≤16 reads | — | loop, not a blocker |
| initrd immutability | read-only, stable | generation constant (=1); "different file" = different name | — | mutable files need real generations later |
| VMO page fill | map a VMO page into the pager's own VSpace, write, unmap | fill a page with file bytes + zero tail | Fase 26 t26_vmo_word pattern | reuse the self-map fill idiom |
| pager protocol | PGR_OP_MAP_RESUME (raw VMO grant) | add PGR_OP_MAP_REGION (file-backed) | Fase 27 T201–T210 | additive op |
| VFS restart | supervised, endpoint survives restart | backing identity stable across restart; generation bump on real change | Fase 24 | initrd generation never changes |

Answers to the mandated questions: a VFS **file grant** = a VFS endpoint cap
(WRITE, to call READ_AT/STAT) scoped by the *set of backing names the pager is
told to serve* — NOT ambient access to every path (F3).  Backing identity =
`(name, size)` today, `(backing_id, generation)` in the general contract.  Only
the pager granted a backing+region may page a target from it; the kernel sees
only VMO/VSpace/frame caps.

## B2 — Backing model

```text
FileBacking {
    backing_id       // stable while valid; initrd: the export name's index
    generation       // initrd: 1 (immutable); mutable backing bumps on change
    file_size        // from VFS STAT
    source_cap       // the VFS endpoint cap used to READ_AT this backing
    access_rights    // READ (initrd is read-only)
    immutable        // 1 for initrd
}
```

Identity is `(backing_id, generation)`, never the path string alone (F2): a
grant names a specific backing, and a page cached under `(backing_id, gen,
page_index)` is invalidated when `generation` changes — so a stale handle or a
new file version never serves an old cached page (F18).  For the immutable
initrd, `generation` is constant and identity reduces to `(name, size)`; the
contract is designed so a mutable backing (a future writable VFS) slots in by
bumping `generation` on write.

## B3 — File region model

```text
FileRegion {
    target            // which target proc/vspace grant (index)
    start_va          // page-aligned user VA
    memory_length     // page-aligned span the region covers in the VSpace
    file_offset       // page-aligned byte offset into the backing
    file_length       // bytes of the backing that are live in this region
    protection        // R / RX / RW
    mapping_mode      // FILE_RO_SHARED | FILE_PRIVATE_WRITABLE | (SHARED_WRITABLE→NOT_SUPPORTED)
    backing_id
    backing_generation
    vmo_grant         // the VMO used as the page vehicle for this region
}
```

Validated on registration (F6–F8, atomically — no partial region):
start_va page-aligned and in `[USER_PRIVATE_BASE, USER_SPACE_TOP)`; memory_length
> 0 and page-aligned; file_offset page-aligned; `file_offset + file_length` no
overflow and `<= file_size`; `file_length <= memory_length`; no overlap with
another region of the same target unless an explicit contract allows it;
protection consistent with the backing rights (a read-only backing forbids a
writable-shared region); the target/vspace/vmo/backing grants all held.

## B4 — Mapping modes

### FILE_RO_SHARED
Read-only backing pages, shareable across targets.  A fault reads the file page
(via cache or VFS READ_AT), fills a VMO page, and maps it **read-only** into the
target at the faulting VA.  The same `(backing_id, gen, page_index)` may reuse a
cached VMO page across targets; per-target mappings are swept independently on
target death (F15).  A read-only backing never yields a writable PTE (F13).

### FILE_PRIVATE_WRITABLE
The page is filled from the file, then handed to the target as a **private
writable** page.  Writes do not propagate to the backing or to other targets
(F14).  Implemented as copy-at-first-fill (a fresh VMO page per target-region,
filled from the file) — no writeback, no COW needed yet.  Two targets on the
same file see identical initial content but diverge on write.

### FILE_SHARED_WRITABLE
In this block: **rejected with `NOT_SUPPORTED`** (F16).  A request for this mode
installs nothing — no region, no cache entry, no VMO/page allocation, no PTE,
no fault-state change.  Faking coherence/writeback is explicitly forbidden; a
real implementation would require dirty tracking and writeback and is deferred.

## B5 — EOF and zero-fill contract

```text
fault page fully inside file_length:
    the whole page is file bytes
last file page (file_length not page-aligned):
    the leading bytes are file bytes; the trailing bytes are ZERO
page in [round_up(file_length), memory_length) (BSS-like tail):
    the whole page is ZERO (if the region permits a zero tail)
fault at or beyond memory_length:
    not resolved (F9)
```

Cases the tests must cover (T220): empty file; file < one page; partial last
page; nonzero file_offset; region ending exactly at EOF; region with a zero
tail; fault beyond the end; an unexpected short read (treated as EOF → the
remainder zero-filled, never a partial PTE, F17); a truncated/changed backing
(generation mismatch → refuse, F18).  The zero tail must never expose prior
page contents (F12): the VMO page is zeroed before the file bytes are copied in
(SYS_VMO_MAP_PAGE already zero-fills a fresh sparse page).

## B6 — Userland page cache (in the pager)

Implemented as a fixed `g_cache[PGR_CACHE_CAP]` (8 slots) backed by the RO cache
VMO grant (slot 16), keyed by `(backing_id, generation, page_off)`:

- valid/invalid + refcount per slot; a region takes one idempotent reference per
  slot it maps (`cache_refmask`), so a page is retained while any live region
  references it (F20/F26);
- hard capacity `PGR_CACHE_CAP`; `pg_cache_alloc` is **deterministic**: reuse the
  lowest invalid slot, else evict the lowest **refcount-0** slot (bumping
  `cache_evict`), else `PGR_ERR_CACHE_FULL` — a full cache of *referenced* pages
  never over-commits (F19/F21).  T228 fills all 8, releases the region, and
  forces exactly 8 evictions by faulting 8 fresh keys — the cache never exceeds
  capacity and always makes progress;
- hit/miss/evict/entries all in `struct pgr_diag`, asserted by T219/T221/T228;
- a stale-generation or revoked-backing key never hits (`pg_cache_find` matches
  the live generation; `pg_revoke_backing` bumps it), F18;
- RO pages are reused across targets (T221 shares one page between two targets);
  private-writable pages are per-fault slots in a separate pool (slot 17), never
  shared (F14);
- eviction only drops the cache entry — the target's already-installed PTE and
  the KFrame behind it survive until the target VSpace is torn down by the
  kernel (F20); a restarted pager rebuilds the cache from its grants (T226).

## B7 — Resolution flow (`PGR_OP_MAP_REGION`)

```text
1. target faults at VA.
2. kernel delivers the fault record to the pager (Fase 20).
3. pager finds the region containing VA (target + region table).
4. pager validates VA and access type against the region (F9, F13).
5. pager computes page_index = (VA - start_va)/PAGE + file_offset/PAGE.
6. pager gets the backing page: cache hit, or VFS READ_AT (≤16 reads) into a
   VMO page it fills (leading = file bytes, trailing = zero), keyed and cached.
7. pager SYS_VMO_MAP_PAGE the (RO or private-writable) page at VA in the
   target VSpace.
8. pager seq-resume (Fase 25 generation check).
9. target continues.
```

Every step has a clean failure path (F38): VFS `NOT_FOUND`/short-read,
generation mismatch, stale grant, occupied VA, `SYS_VMO_MAP_PAGE` denied, stale
seq — none install a partial PTE, none resume incorrectly, temporary refs are
released, the fault record stays consistent, and the target stays suspended or
is killed per the region contract.

## B8 — Authority

The pager receives ONLY: target proc grant, target VSpace grant, target
notification, the authorized file/backing grants (a VFS endpoint cap + the
backing names/regions it may serve), the VMO grants it needs as page vehicles,
and its control endpoint.  It receives NO global VFS access, no all-paths
access, no untyped, no global frame authority, no spawn, no device caps, no
KDEBUG, and no authority over undeclared targets (F3–F5).  A file grant for one
backing implies nothing about any other backing.  The VFS must be able to hand
the pager a bounded cap/handle to exactly the authorized backing.

## Invariants (F1–F40)

The full list is in the phase brief; the implementation must document and test
each.  The load-bearing ones the architecture above pins down: identity +
generation (F1/F2/F18), bounded per-backing grants (F3–F5), region validation
(F6–F8), EOF/zero-fill exactness (F9–F12), mode semantics (F13–F16),
short-read/stale atomicity (F17/F29/F30/F38), bounded cache with safe eviction
(F19–F21), death/restart cleanup (F22–F25), lifetime/revoke (F26–F28), and the
standing no-drift/no-ghost/no-filesystem-policy guarantees (F31–F40).

## Tests T217–T230 (all green)

| Test | What it proves | Maps to |
|---|---|---|
| T217 | backing identity + generation + manifest (exact authority) | B2/B8 |
| T218 | region validation (every field, atomic) + a resolve | B3 |
| T219 | RO multi-page out-of-order resolution, one target | B4/B7 |
| T220 | EOF/zero-fill exactness (partial page, sub-page file, EOF boundary, pure-zero pages) | B5 |
| T221 | shared RO cache hit + independent reference cleanup | B6 |
| T222 | private-writable isolation (write vs. copy-at-fill read) | B4 |
| T223 | shared-writable `NOT_SUPPORTED`, zero side effects | B4 |
| T224 | backing read failure is atomic (no cache/PTE/resume) | B7 |
| T225 | grant revoke (generation bump, stale bind refused, unreferenced page dropped) | B2/B8 |
| T226 | pager restart contract (fresh clean state, rebuilds, resolves) | B6/B8 |
| T227 | multiple files + multiple targets, no cross-file bleed | B3/B8 |
| T228 | cache bounded + evicts unreferenced pages (never over-commits) | B6 |
| T229 | ELF segment groundwork RX/R/RW/BSS + W^X | B3/B4/B5 |
| T230 | deterministic control-plane stress, counters return to zero | all |

The single-target multi-page and two-offset fault probes
(`LP_CMD_FAULT_READ_SEQ` / `LP_CMD_FAULT_READ_OFFS` in `lifecycle_probe`) let one
target drive multi-page and arbitrary-byte-offset resolution without spawning N
one-shot targets — which would exhaust the per-process notification quota
(`KPROCESS_NOTIFICATION_QUOTA = 16`).

## ELF-segment groundwork (T229)

The region model expresses the four ELF segment shapes and enforces W^X at
registration: RX code (`R|X`, RO-shared → mapped RO + `MAP_EXEC`), R rodata
(`R`, RO-shared), RW data (`R|W`, private-writable), and BSS (`R|W`, private,
`file_length = 0` → pure zero-fill).  `pg_register_region` rejects `W&X`
together, any prot bit outside `R|W|X`, and a region with no `R` bit — every
segment is readable, none is both writable and executable (the kernel enforces
W^X on the PTE too).  An RX fault resolves to a read-only **executable** page.
This is the load groundwork an ELF loader would drive to lay out a process image
from a file; the loader itself is a later phase.

## Current limits / remaining work

- **Generations are constant (=1)** for the immutable initrd; the identity +
  generation contract is generation-ready for a future mutable/writable VFS
  (revocation already bumps the generation and invalidates cached pages).
- **Cache is per-pager**, rebuilt from grants on restart (T226); there is no
  shared system page cache yet (deliberately — a later phase).
- **`FILE_SHARED_WRITABLE` is `NOT_SUPPORTED`** — no writeback, no COW, no
  filesystem-backed writable sharing (out of scope by design; rejected with zero
  side effects, T223).
- **The ELF loader is groundwork only** — regions model the segment shapes and
  W^X (T229); wiring an actual ELF program image through the pager is future
  work.

---

## Fase 28.1 addendum — the grant is now VFS-enforced

Fase 28 enforced the file grant inside the pager (it rejected unregistered
names).  That is not an authority boundary for a *compromised* pager.  **Fase
28.1** moves the frontier into the VFS: the pager holds a session-badged,
WRITE-only `vfs.ep` cap (no name path, no enumeration), reads exclusively via
`GRANT_READ_AT(grant_idx, off, len)` — **no pathname anywhere** — and the VFS
validates badge + session + grant + rights + generation on every operation.
`pg_read_file` takes a `grant_idx`, not a name; `pgr_backing_req` carries
`grant_idx` + the VFS-issued `(backing_id, generation)`, cross-checked against
`GRANT_QUERY_IDENTITY` before a backing is trusted.  The local
`REVOKE_BACKING` is now cache bookkeeping only — the authority revoke is the
VFS's (`GRANT_REVOKE` bumps the export generation; stale grants fail CLOSED).
See `file-grant-capability.md` for the full contract and threat model
(T231–T238).
