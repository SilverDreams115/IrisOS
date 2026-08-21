# Kernel Object Lifetime & Charge/Release Paths

Status: **DOCUMENTED** (Phase 29; updated at Stage 7 close).  Companion to
`resource-ownership-accounting.md` (the retired ownership model, kept as the
record of why the current one looks as it does) and `kernel-capacity-limits.md`
(global capacity).

This document records, per object type, when a resource charge is acquired and
when it is released, so the two always balance.  Since Stage 7 a "charge" is
always the same thing — a child block of an `Untyped` somebody named — because
the per-domain quotas that were the other kind are gone.

## The lifecycle split

Every typed kernel object is a `KObject` with a refcount.  Handles and mappings
hold references; the object is destroyed when the last reference drops.  A
resource **charge** (the memory an object occupies in the `Untyped` it was
retyped from) is separate from a reference:

- a **charge** is acquired at a well-defined creation/allocation point and
  released at a well-defined destruction point;
- a **reference** (handle / mapping / retain) keeps the object *alive* but does
  not, by itself, add a charge.

`Q10`: retain/release never double-count accounting.  `Q11`: destroy releases
exactly one charge per charge acquired.

## Charge/release by object

| Object | Charge acquired | Charge released | Notes |
|--------|-----------------|-----------------|-------|
| ~~KProcess~~ | — | — | **the object is DELETED (Stage 7-proc)**.  A process is threads configured with the same CSpace and the same VSpace; there is nothing to charge, and nothing to destroy |
| TCB / VSpace / root CNode | `RETYPE2` from the budget the spawner named | last capability → destroy → the block returns zero-filled to that Untyped | this is what "creating a process" costs now, and all of it is reclaimed by `SYS_UNTYPED_RESET` |
| KVMO (object) | the header and page array are carved from the `Untyped` named in `SYS_VMO_CREATE` | `kvmo_destroy` returns them | the owner relation and `owned_vmos` are **deleted (Stage 7-mem)**; a VMO's accounting is the budget it came from |
| KVMO (sparse pages) | first touch of each page carves it from that same budget | `kvmo_destroy`, once per allocated page | charged once regardless of how many VSpaces map it |
| KFrameMapping / PTE | `kvspace_map_page` | `kvspace_unmap_page` / VSpace teardown | per-(VSpace, VA) |
| KNotification / KEndpoint / KCNode / KReply / KSchedContext | `RETYPE2` from a named Untyped (Phases S1–S2) | last-reference destroy returns the block | bounded by the budget, never by a quota.  KNotification had an `owned_notifications` quota until **Phase S1**: it is now paid for in Untyped + a CSpace slot, like every other retyped object |

## Death propagation

- **Thread death** (Stage 7): a thread's execution ends and its object is
  destroyed when the last capability to it goes.  It drops its references to
  its CSpace root and its address space; nothing cascades from a "process",
  because there is none.

- **Address-space death** (Stage 7-proc): a `KVSpace` is invalidated by its own
  `close` hook — the moment its **last capability** goes, not when its last
  thread exits.  That drops the mappings and their frame retains and releases
  the bootstrap frames (which live on the KVSpace now, since they are frames
  mapped into that address space).  This is a real semantic change and the
  suite records it: an address space outlives its threads while a capability to
  it lives, so a late map into a dead target's space SUCCEEDS — seL4's shape,
  where a page directory outlives its threads.

- **CSpace death**: a root CNode's `close` empties its slots, which is what
  lets destroy run.  A slot naming its OWN CNode takes no active reference — an
  object reachable only from itself is reachable by nobody — so no pre-emptive
  cycle-breaking is needed (`BC-11..BC-13`).

- **Budget vs holder death** (Q13, re-derived): a VMO's memory belongs to the
  **Untyped it was carved from**, and a *holder* dropping its capability only
  drops one reference; the object survives while any reference (another
  capability, a mapping) remains, and the memory returns to that Untyped only
  at `kvmo_destroy`.  So a loader that created a child's VMO out of the child's
  budget and then dies does not destroy the child's memory, and reclaiming one
  child (`SYS_UNTYPED_RESET` on its budget) does not touch another's.

- **Target death** (Q14): a target of the pager dying drops its mappings; the
  pager is not charged for the cleanup.  The pager's own cache/private VMOs
  come out of the pager's own budget and are released with it (Q15).

## Balance guarantee

For every object type the charge-acquire and charge-release points are paired
one-to-one along all paths, including error rollback (validate → reserve →
allocate → publish → commit; on failure, release the provisional charge and
publish nothing).  `SYS_UNTYPED_QUERY` makes the balance observable — kind
`ONE` per budget, kind `GLOBAL` for the kernel-wide gauges — and T239–T250,
re-derived onto it when `SYS_RESOURCE_INFO` retired, assert usage returns
exactly to baseline after every scenario (Q23) while high-water marks stay
monotone (Q24).

## Phase S1 — Untyped-backed lifetime (Endpoint / Notification / Reply / CNode)

Migrated objects do NOT take quota charges: their "charge" is the Untyped
memory consumed (`child_count` + `used_bytes` of the source Untyped), and their
release is the object's destruction (the block returns zero-filled to the
region). The notification quota was RETIRED in S1.

Full cycle:

```
cap delete            SYS_CNODE_DELETE(0=own root, slot)
                      → releases that slot/handle; the object lives if caps
                        or kernel refs remain (S10)
last capability       active_refs → 0 ⇒ close():
                        Endpoint: closed=1, queues drained, waiters CLOSED
                        Notification: closed=1, waiters CLOSED
                        Reply: caller (if bound) wakes CLOSED; staged=0
object destruction    refcount → 0 ⇒ destroy():
                        kuntyped_release_child: zero the block,
                        child_count-- and release the parent
Untyped reusable      child_count==0 ⇒ SYS_UNTYPED_RESET: used=0,
                        generation++ (reuse witness)
```

Revoke:
- `SYS_CSPACE_REVOKE` cascades over the **native CSpace CDT/MDB** — exact
  descendants, idempotent, does not touch siblings, and recursive across CNodes
  and address spaces.  It is the only derivation tree: the parallel handle tree
  and `SYS_CAP_REVOKE` were deleted in Phase S4 / Stage 3.
- Endpoint: covers blocked senders/receivers/callers, staged caps and dead
  processes via close/cancel (A1.9–A1.11, T255/T258).
- Notification: waiters, pending bits, IRQ binding (the path retains the
  notification), shared pager use (T256, T237).
- Reply: unconsumed (close→caller CLOSED), dead caller (unbind → reusable),
  dead server (its CSpace slots close), consumed (free), stale (empty slot →
  NOT_FOUND) — T257/T258.

Internal kernel references that retain migrated objects (and why there are no
stale pointers after reuse): `sender->pending_kreply` (ref until wake),
`t->ep_reply_obj` (staging ref, released on every recv exit path and on
teardown), IRQ paths → notification (ref until deregistration), EP/notification
queues (dequeued on close/cancel/teardown).
