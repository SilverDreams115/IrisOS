# Kernel Object Lifetime & Charge/Release Paths

Status: **DOCUMENTED** (Fase 29).  Companion to
`resource-ownership-accounting.md` (the ownership model and per-object table)
and `kernel-capacity-limits.md` (global capacity).

This document records, per object type, when a resource charge is acquired and
when it is released, so the two always balance.

## The lifecycle split

Every typed kernel object is a `KObject` with a refcount.  Handles and mappings
hold references; the object is destroyed when the last reference drops.  A
resource **charge** (a per-domain quota increment) is separate from a reference:

- a **charge** is acquired at a well-defined creation/allocation point and
  released at a well-defined destruction point;
- a **reference** (handle / mapping / retain) keeps the object *alive* but does
  not, by itself, add a charge.

`Q10`: retain/release never double-count accounting.  `Q11`: destroy releases
exactly one charge per charge acquired.

## Charge/release by object

| Object | Charge acquired | Charge released | Notes |
|--------|-----------------|-----------------|-------|
| KProcess | `kprocess_alloc` (atomic live-count reserve) | `kprocess_destroy` | rolled back if over `KPROCESS_MAX_LIVE` |
| KVMO (object) | `kvmo_bind_owner(v, payer)` → `payer->owned_vmos++` | `kvmo_destroy` → `owned_vmos--` | payer = self or a MANAGE'd process (`SYS_VMO_CREATE_FOR`) |
| KVMO (sparse pages) | first allocation of each page → `payer->phys_pages_charged++` (payer = `kvmo_owner(v)`) | `kvmo_destroy` → one `phys_pages_charged--` per allocated page | charged once regardless of how many VSpaces map it |
| KNotification | `knotification_bind_owner` → `owned_notifications++` | `knotification_destroy` → `owned_notifications--` | owner = the creating process |
| KFrameMapping / PTE | `kvspace_map_page` (kslab object) | `kvspace_unmap_page` / VSpace teardown | per-(VSpace, VA); no per-process quota |
| KEndpoint / KCNode / KReply / KSchedContext | kslab object at create | last-reference destroy | bounded by kslab capacity, not a per-domain quota |

## Death propagation

- **Process death** (`kprocess_teardown` → `kprocess_reap_address_space` →
  `kprocess_destroy`): closes the handle table (dropping references to owned
  objects), invalidates the VSpace (dropping mappings and their frame retains),
  releases bootstrap frames, and — as owned objects reach zero references —
  their `kvmo_destroy` / `knotification_destroy` release the process's charges.
  A process's own resources are freed by its own death (Q12).

- **Owner vs holder death** (Q13): a VMO's object + page charge lives with its
  **owner** (kept alive by an owner retain taken in `kvmo_bind_owner`).  A
  *holder* closing its handle only drops one reference; the object survives
  while any reference (another handle, a mapping) remains, and the charge is
  released only at `kvmo_destroy`.  So a loader that created a child's VMO and
  then dies does not destroy the child's memory, and killing one child does not
  touch another's charges.

- **Target death** (Q14): a target of the pager dying drops its mappings; the
  pager is not charged for the cleanup.  The pager's own cache/private VMOs are
  charged to their owner and released when the pager (owner) dies (Q15).

## Balance guarantee

For every object type the charge-acquire and charge-release points are paired
one-to-one along all paths, including error rollback (validate → reserve →
allocate → publish → commit; on failure, release the provisional charge and
publish nothing).  `SYS_RESOURCE_INFO` makes the balance observable; T239–T250
assert usage returns exactly to baseline after every scenario (Q23) while
high-water marks stay monotone (Q24).
