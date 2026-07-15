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

## Fase S1 — Untyped-backed lifetime (Endpoint / Notification / Reply / CNode)

Los objetos migrados NO adquieren cargas de quota: su "charge" es la memoria
Untyped consumida (`child_count` + `used_bytes` del Untyped fuente), y su
release es la destrucción del objeto (el bloque vuelve cero-relleno a la
región).  La quota de notifications fue RETIRADA en S1.

Ciclo completo:

```
cap delete            SYS_CNODE_DELETE(0=own root, slot) / SYS_HANDLE_CLOSE
                      → suelta ese slot/handle; el objeto vive si quedan
                        caps o refs kernel (S10)
last capability       active_refs → 0 ⇒ close():
                        Endpoint: closed=1, colas drenadas, waiters CLOSED
                        Notification: closed=1, waiters CLOSED
                        Reply: caller (si bound) despierta CLOSED; staged=0
object destruction    refcount → 0 ⇒ destroy():
                        kuntyped_release_child: zero del bloque,
                        child_count-- y release del padre
Untyped reusable      child_count==0 ⇒ SYS_UNTYPED_RESET: used=0,
                        generation++ (testigo de reuse)
```

Revoke:
- `SYS_CAP_REVOKE(h)` cascada sobre el árbol de derivación de la handle
  table (descendants exactos, idempotente, no toca siblings).
- Copias minteadas en CNodes son refs independientes (documentado + T127);
  un CDT sobre CSpace es trabajo de la fase CSpace-only (ledger).
- Endpoint: cubre senders/receivers/callers bloqueados, staged caps y
  procesos muertos vía close/cancel (A1.9–A1.11, T255/T258).
- Notification: waiters, pending bits, binding IRQ (la ruta retiene la
  notification), uso compartido de pager (T256, T237).
- Reply: no consumido (close→caller CLOSED), caller muerto (unbind →
  reusable), server muerto (slots del proceso → close), consumido (free),
  stale (slot vacío → NOT_FOUND) — T257/T258.

Referencias kernel internas que retienen objetos migrados (y por qué no hay
punteros stale tras reuse): `sender->pending_kreply` (ref hasta wake),
`t->ep_reply_obj` (staging ref, liberada en todo camino de salida del recv y
en teardown), rutas IRQ → notification (ref hasta des-registro), colas de
EP/notification (desencoladas en close/cancel/teardown).
