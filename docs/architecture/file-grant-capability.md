# Fase 28.1 — File Grant Capability Enforcement + Pager Multi-target

Status: **IMPLEMENTED and tested end to end** (runtime T231–T238, all green in
the 234/234 suite; host units 10247/10247).  Companion to
`file-backed-memory.md` (Fase 28 Bloque B), `service-pager-integration.md`
(Fase 27), `service-authority-manifest.md` and `service-supervision-model.md`.

This increment closes two residual risks left by Fase 28:

1. The file "grant" was enforced only by the pager's own internal table while
   the pager held a *generic* VFS endpoint cap — a restriction applied by the
   (potentially compromised) service is **not** an authority boundary.
2. The pager did not scale to several concurrent targets because each target
   consumed one of the owner's 16 process notifications.

The governing rule: **a restriction applied only by the compromised service is
not a frontier of authority.**  Fase 28.1 moves the file-authority frontier
into the VFS, where the pager cannot reach it.

---

## Why a pathname is not authority

In Fase 28 the pager read files by sending `(name, offset, len)` to a generic
`vfs.ep` cap.  A pathname in a message is *data*: a compromised pager could put
any name in any message and the VFS would serve it.  The pager's "grant table"
rejected unregistered names, but that check runs **inside the thing we don't
trust**.  Containing a hostile pager requires that the VFS itself refuse to
serve a backing the pager was not explicitly and unforgeably given.

Fase 28.1 therefore makes the file grant a **VFS-issued, VFS-validated
authority** and removes the pager's ability to name files at all.

---

## The construction (no new kernel object)

The unforgeable grant is built from three guarantees the kernel already
provides — no new syscall, no new kernel object:

1. **`IrisMsg.sender_badge` is kernel-stamped** from the invoked capability
   (Fase 9).  A client cannot write it.
2. **A badged endpoint cap can never be re-badged** (`SYS_PROC_CSPACE_MINT`,
   Fase 9): a fresh badge may be minted only from an *unbadged* source, which
   under the Fase 10 grant-tightening rule only a supervisor holds.
3. **Rights reduce monotonically** on every mint/derive.

From these, the VFS classifies every caller by badge and confines it:

| Badge class | Identity | May invoke |
|---|---|---|
| `IRIS_BADGE_FILEGRANT_ADMIN` (0x0F00) | a pager **supervisor** | `GRANT_OPEN`, `GRANT_REVOKE`-by-name, `GRANT_SESSION_RESET` |
| `IRIS_BADGE_FILEGRANT_S(s)` (0x0F10+s) | pager **session s** | `GRANT_STAT/READ_AT/QUERY_IDENTITY/DERIVE/REVOKE` — **only** on grants of session s |
| any other / unbadged | ordinary named-export client | `LIST/STAT/READ_AT/STATUS` (unchanged) |

A **session badge is denied every name-based op** (`LIST/STAT/READ_AT`): the
containment check runs *before* the opcode switch in `vfs_ep_dispatch`, so no
name path is reachable from a session cap, present or future.  A compromised
pager holding only its session cap cannot read, stat, or enumerate any file it
was not granted — no matter what bytes it constructs.

### Grant record (VFS state)

```
struct vfs_grant { used, export_slot, rights, backing_id, generation }
```

held in a `VFS_GRANT_SESSIONS × VFS_GRANTS_PER_SESSION` table addressed **only**
by the kernel-stamped badge.  `backing_id` and `generation` are **VFS-issued**:

- `backing_id = 0xB1000000 | (epoch<<8) | slot` — stable per (instance, export).
- `generation = (epoch<<16) | seq` — the high half is the **service-instance
  epoch** (the svcmgr restart generation of `vfs.ep`); the low half is a
  per-instance revoke sequence.

Every grant operation re-validates the snapshot generation against the export's
*current* generation (`vfs_ep_grant_live`).  A revoked backing, a superseded
generation, or a re-seeded export fails **`IRIS_ERR_CLOSED`** — in the VFS,
regardless of any state the holder keeps.

### Rights

```
VFS_FILE_RIGHT_STAT      GRANT_STAT / size queries
VFS_FILE_RIGHT_READ      GRANT_READ_AT
VFS_FILE_RIGHT_DUPLICATE GRANT_DERIVE (reduced-rights copies)
VFS_FILE_RIGHT_REVOKE    GRANT_REVOKE via the grant itself
```

Monotonic: `GRANT_DERIVE` requesting any right the source lacks is
`ACCESS_DENIED` — denied, not clamped, so the attack is visible.  A STAT-only
grant cannot read; a READ-only grant cannot revoke; a grant without DUPLICATE
cannot be re-derived.

---

## The delegation contract

```
supervisor (ADMIN badge):
    GRANT_OPEN(session, name, rights) → (grant_idx, backing_id, generation)
        — the pathname appears HERE ONLY, presented by the admin.

supervisor → pager:
    the pager's ENTIRE VFS identity is a SESSION-badged, WRITE-only vfs.ep cap
    (slot 4).  The supervisor also hands the pager (grant_idx, backing_id,
    generation) as a backing registration.

pager:
    REGISTER_BACKING cross-checks the declared identity against
    GRANT_QUERY_IDENTITY before trusting it, then reads exclusively via
    GRANT_READ_AT(grant_idx, off, len) — NO pathname, ever.

VFS:
    validates badge + session + grant slot + rights + generation on EVERY op.
```

After `GRANT_OPEN`, the pager never sends a pathname again; the grant index over
its badged session cap is the whole request, and the VFS decides from **its**
table whether to serve it.

---

## Eliminating ambient authority (the pager manifest)

The productive pager's manifest (`services/pager/pager_proto.h`) no longer
contains any nameable VFS authority:

| Cap / grant | Slot | Scope | Reason |
|---|---|---|---|
| control endpoint | 3 | RIGHT_READ | supervisor drives it request/reply |
| **VFS session cap** | 4 | session-badged, WRITE-only | file reads via grants ONLY; no name path, no enumeration |
| **shared fault notification** | 5 | RIGHT_WAIT | one notification for ALL targets (see below) |
| RO cache VMO | 16 | RIGHT_READ\|WRITE | shared page cache |
| private-writable pool VMO | 17 | RIGHT_READ\|WRITE | copy-at-fill pages |
| target proc | 20+i·2 | RIGHT_READ\|MANAGE | fault-info + resume authority for target i |
| target vspace | 21+i·2 | RIGHT_WRITE | map-into authority for target i |

No spawn cap, untyped, device cap, KDEBUG, or generic VFS client cap.

### The test/production authority split (slots 58/59)

Because a badged cap can never be re-badged, one cap cannot be both a positive
admin identity and a fresh-badge mint source.  So init pre-mints the supervisor
(iris_test) **two** caps:

- **slot 58 `IRIS_CPTR_TEST_VFS_DUP`** — the grant **ADMIN** identity
  (`IRIS_BADGE_FILEGRANT_ADMIN`, WRITE-only).  Drives `GRANT_OPEN` /
  `GRANT_REVOKE` / `GRANT_SESSION_RESET`.
- **slot 59 `IRIS_CPTR_TEST_VFS_MINT`** — an **unbadged**
  `WRITE|DUPLICATE|TRANSFER` vfs.ep cap: the mint SOURCE for session-badged
  pager caps.  Invoked directly (badge 0) it is an ordinary named-export
  client — no grant authority rides on unbadged caps.

Neither slot gives the pager anything: the pager gets a *derived*, session-
badged, WRITE-only cap.  The old Fase 28 duplicable-cap leak (a generic VFS cap
in the pager) is closed — the pager's slot 4 is now confined by badge to its
grants.

---

## Revocation is VFS-enforced

`GRANT_REVOKE` (ADMIN by name, or a session grant carrying
`VFS_FILE_RIGHT_REVOKE`) bumps the export generation.  From that instant, every
grant snapshotting the old generation fails **`IRIS_ERR_CLOSED`** at the VFS on
its next use.  The denial holds even if the pager:

- keeps the grant index, the (old) backing id/generation, or an old message;
- manipulates its internal table;
- restarts;
- replays a request byte-for-byte.

The pager's local `REVOKE_BACKING` is now **bookkeeping only** (it bumps a local
generation copy to keep the cache honest); the *authority* revoke is the VFS's,
and a pager that skips its bookkeeping still cannot read the revoked backing —
`GRANT_READ_AT` fails CLOSED at the VFS.

- **Generation N never accesses N+1** (`generation` snapshot mismatch).
- **VFS restart invalidates all prior grants**: a restarted VFS gets a strictly
  newer *epoch* (its svcmgr restart generation) that stamps the high half of
  every generation, and an empty grant table.  No pre-restart grant can ever
  validate against the new instance.
- **Existing mappings follow the Fase 28 contract**: already-installed target
  PTEs are owned by the kernel VSpace and survive a revoke; only *new* faults
  are denied.

---

## Multi-target: one shared fault notification

### The old quota behaviour

`SYS_EXCEPTION_HANDLER` binds a process's fault to a KNotification signalled on
fault; `SYS_NOTIFY_CREATE` charges the notification to its **creator**.  In
Fase 28 each target had its own fault notification, all created by the
supervisor, against the supervisor's `KPROCESS_NOTIFICATION_QUOTA = 16`.  With
~8 already held at baseline, four concurrent targets (one notification each)
exhausted the quota — Fase 28 could only ever run **one** target per pager.

### The new model

**One** KNotification serves **all** targets.  The supervisor registers each
target's exception handler on the same notification with signal bit `(1 << i)`;
the pager holds one `RIGHT_WAIT` cap to it (slot 5).  A **pending-bits
accumulator** in the pager (`g_pending`) makes the wait a *wait-any*: bits that
arrive for other targets while waiting for target *i* are accumulated, never
lost, so interleaved faults from many targets survive any service order.

Ownership is now explicit and correct: **N concurrent targets cost the
supervisor exactly ONE notification**, not N.  A single wakeup can carry several
targets' bits (fewer wakeups than faults is the efficiency win, not a bug).

### Capacity

`PGR_MAX_TARGETS = 16`.  T237 registers and faults **1, 4, 8 and 16** targets
under a single pager, all sharing one notification, with interleaved faults
resolved in scrambled order, targets dying in arbitrary order, and the
supervisor's notification books returning to baseline.

The residual concurrency ceiling is **kernel-object memory**, not notification
quota:

- the kernel object slab (`kslab`) was grown 4 MB → **16 MB** (3% of the 512 MB
  guest): each process consumes several KB–32 KB of kernel objects (KProcess +
  256-slot root KCNode + KVSpace + page tables + handle table);
- `KPROCESS_VMO_QUOTA` was raised 32 → **128**: a *loader* creates each child's
  segment+stack VMOs under **its own** ownership (`kvmo_bind_owner` binds the
  caller), and the quota releases only when the child dies and drops its
  mappings — so a supervisor keeping N children alive holds ~4·N VMOs against
  its own quota.  The old 32 capped a supervisor at ~8 concurrent loaded
  children.

Both are **memory/accounting** bounds, deliberately distinct from the
notification quota Fase 28.1 resolved.  Raising them is the honest fix so the
cap is a policy, not an accident of charging a child's memory to whoever loaded
it.

---

## Invariants (proved by T231–T238)

```
A1  Pathname is not authority.
A2  A file grant identifies exactly one backing.
A3  A file grant includes a generation.
A4  The VFS validates the grant on every operation.
A5  The pager cannot read a backing it was not granted.
A6  Changing a message cannot change which backing is served.
A7  Grant rights are monotonic (derive only shrinks; no recovery).
A8  A stale grant fails clean (CLOSED).
A9  A revoked grant fails even if the pager ignores its internal table.
A10 Generation N never accesses N+1.
A11 A pager restart receives only current grants (SESSION_RESET first).
A12 A VFS restart invalidates prior grants (new epoch + empty table).
A13 A file grant confers no global VFS authority.
A14 The productive pager receives no generic unrestricted cap.
A15 The file-backed cache never uses a stale backing.
A16 Existing mappings follow the Fase 28 contract after revoke.
A17 A new fault after revoke is not resolved from the revoked backing.
A18 Several grants do not mix files.
A19 Several targets do not mix grants.
A20 The notification quota has documented ownership.
A21 The pager supports multiple simultaneously-registered targets.
A22 Fault records of distinct targets do not mix.
A23 Target death frees its notification bit / quota.
A24 A pager restart rebuilds multiplexing without ghosts.
A25 No notification drift.
A26 No endpoint/KReply drift.
A27 No VMO/mapping/frame drift.
A28 No registry ghost.
A29 No CSpace ghost.
A30 No authority amplification.
```

## Tests

| Test | Scenario | Trust boundary | Failure paths |
|---|---|---|---|
| T231 | VFS-enforced grant identity | two files, two grants, distinct VFS-issued identity | wrong idx, bogus idx, session named-stat, no cross-read |
| T232 | arbitrary-name attack denial | hostile pager (session cap direct to VFS) | named read/list/open/revoke/reset, cross-session read |
| T233 | rights monotonicity | STAT-only, READ-only, no-DUP, recovery | every over-right request denied by VFS |
| T234 | revoke + generation replay | revoke, replay same idx, new generation | replay CLOSED, gen N vs N+1 |
| T235 | pager restart with grants | old-instance grant after restart | SESSION_RESET drops stale grant (NOT_FOUND) |
| T236 | VFS restart invalidates grants | epoch-stamped generations | stale-epoch backing denied |
| T237 | multi-target notification scaling | 1/4/8/16 targets, one shared notif | interleaved faults, shared-notif usage, no mix |
| T238 | deterministic file-authority + multi-target stress | seeded round-robin over the whole surface | no unauthorized read, no stale success, no mix, baseline |

All attacks are driven from a **session cap self-minted by the supervisor** —
byte-identical to a compromised pager's cap — sent **directly to the VFS**,
bypassing the pager's helper.  A helper rejecting the request would not count;
only a VFS reply of `ACCESS_DENIED` / `CLOSED` / `NOT_FOUND` does.

---

## Threat model — compromised pager

A pager that is fully hostile holds: its control endpoint, its session-badged
WRITE-only vfs cap, the shared fault notification, per-target proc/vspace
grants, and two VMO grants.  With these it **cannot**:

- read/stat/enumerate any file it was not granted (session badge denied all
  name ops; grant index bound to one backing);
- widen a grant's rights (monotonic derive);
- read a revoked or superseded backing (VFS re-checks generation);
- reach another session's grants (badge selects the session);
- open new grants, revoke by name, or reset a session (admin-only);
- touch any process outside its per-target grants (Fase 27 boundary);
- amplify authority across a restart (SESSION_RESET; fresh session cap only).

Its compromise is bounded by exactly the backings its supervisor granted, and
its death/restart is survivable.
