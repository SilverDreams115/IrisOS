# IRIS — CSpace Capability Model (Fase S1)

Consolidates the A1 contract (`a1-authority-namespace-endgame.md`) with the S1
model. Normative for all new authority.

## Namespaces

- **CPtr (< 1024)**: the CSpace is the CANONICAL namespace for persistent,
  delegable authority. Radix traversal over CNodes (powers of 2,
  `ctzll(slot_count)` bits per level); slot 0 = CPTR_NULL.
- **handle (≥ 1024)**: an EPHEMERAL per-process materialization (working set).
  Never a second canonical namespace. `ACCESS_DENIED` in the CSpace is a hard
  stop — there is no fallback in either direction.
- Sanctioned bridge: `SYS_CSPACE_RESOLVE` / `SYS_CNODE_FETCH` (CSpace →
  handle). The list of handle producers is closed (A1); Fase S1 added none and
  retired three (the create syscalls).

## Authority birth (S1)

All NEW authority in the migrated family appears in CSpace:
`SYS_UNTYPED_RETYPE2` publishes the caps directly into slots of a destination
CNode (0 = the caller's root). Invariants S19/S20/S21: no migrated object is
born from a quota, from a handle, or outside CSpace.

Additional S1 convention: `SYS_CNODE_DELETE(0, slot)` operates on the caller's
own root CNode (discarding one's own authority amplifies nothing); it mirrors
RETYPE2's destination 0.

## Derivation, badges, revoke

No contract change in S1: mint/derive reduce rights monotonically; badges are
stamped only by the minting authority (a badged cap is never re-badged);
revoke is transitive over the handle derivation tree; copies in CNodes are
independent refs (T127). The full CDT over CSpace is delivered in the native
CDT phase (see the ledger and `cspace-cdt-mdb.md`).

## Rules for new code

1. A new object type must be CSpace-invocable from day one (a dual resolver,
   never `handle_table_get_object` directly).
2. Adding handle-first productive paths for canonical objects is forbidden
   (ledger: handle table FROZEN for new producers).
3. Adding dual resolution to new operations is forbidden: S1+ operations
   (e.g. the RETYPE2 destination) accept a cptr-or-handle only through the
   already-existing sanctioned bridge; the final destination is CSpace-only.
4. Legacy helpers that need handles are migrated; no new conversions are added.
