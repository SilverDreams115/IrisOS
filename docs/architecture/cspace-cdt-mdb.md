# IRIS — Native CSpace CDT/MDB (Fase S3, normative)

Capability derivation model tied to CNode slots. Implements charter
§2.1/A9-A10 (traceable derivation + recursive cross-process revoke) and
Stage 1 of the [roadmap](sel4-convergence-roadmap.md).

## 1. Exact definitions

1. **Capability** = the content of an occupied CSpace slot: `(object, rights,
   badge, MDB node)`. Authority IS the slot; two slots to the same object are
   two distinct authorities.
2. **Slot** = one `struct KCSlot` entry inside a `struct KCNode`'s inline
   array. Empty ⇔ `object == NULL` ⇔ empty MDB node.
3. **Internal slot identity** = the pair (`KCNode*`, index), materialized as a
   direct `struct KCSlot *` pointer (the array lives inside the CNode's
   storage and is stable for the CNode's whole life). Each slot carries a
   back-pointer `mdb_cnode` to its owning CNode. No identity is derived from a
   PID, a global index, or user-space addresses.
4. **Per-slot MDB metadata** (intrusive, zero allocation per operation):
   `mdb_parent`, `mdb_first_child`, `mdb_next_sib`, `mdb_prev_sib` (slot
   pointers), `mdb_cnode` (owning CNode) and `mdb_flags` (`MDB_LEGACY_ROOT`).
   It is an explicit parent/first-child/sibling tree with a doubly-linked
   sibling list (O(1) unlink).
5. **Root** = a node with `mdb_parent == NULL`. Every root today is either a
   `LEGACY_ROOT` (installed from a non-CSpace origin: handle, bootstrap kernel
   object, staged IPC delivery) or a **promoted** one (orphaned by the delete
   of a root — counted in `mdb_orphan_promotions`). Canonical caps
   (copy/mint/retype with a slot source) ALWAYS have a parent.
6. **Descendant** = reachable from a node by walking down `first_child/
   next_sib`. The relationship crosses CNodes and processes: the links are
   slot pointers; the owning KProcess is irrelevant.
7. **Copy** = derive with `RIGHT_SAME_RIGHTS`: a new slot with the same
   object, the same effective rights and an inherited badge; an MDB child of
   the source.
8. **Mint** = derive with reduction: `effective = src & requested` (collapse
   to NONE → INVALID_ARG); badge per the single central rule §3; an MDB child
   of the source. Requires `RIGHT_DUPLICATE` on the source.
9. **Move** = transplant the capability AND its node: the destination slot
   inherits object/rights/badge/parent/children/sibling-position; every
   child's `mdb_parent` is re-pointed; the source slot is left completely
   empty. It creates no derivation and does not change net refcounts (a
   balanced internal transfer).
10. **Delete** = remove ONLY the selected capability. Its children are
    **reparented to the grandparent** (spliced into the deleted node's
    parent's child list), preserving the revocation authority of every
    surviving ancestor. If the deleted node was a root, each child is promoted
    to a root (there is no surviving ancestor — the sole justification) and
    counted.
11. **Revoke** = remove the ENTIRE descendant subtree of the invoked slot,
    keeping the invoked slot. Deterministic order: deepest-leftmost leaf first
    (iterative post-order, O(1) state). Crosses CNodes and processes. The
    invoked node's siblings and their descendants are NOT touched.
12. **Intermediate-node delete** — repair rule: see (10); the preserved
    invariant is "if A was an ancestor of C before deleting B (A≠B), A is
    still an ancestor of C afterwards".
13. **Subtree destruction** (revoke): for each victim slot, under lock: unlink
    from the tree + clear the slot; outside the lock: release the object's
    references (active + lifecycle). The object's close/destruction (waking
    blocked tasks, returning storage to the Untyped) happens via its normal
    lifecycle when the references drop — AUTHORITY removal never waits on
    storage.
14. **Untyped**: `RETYPE2` with a CPtr source registers each created cap as a
    child of the source untyped's SLOT. With a handle source (legacy) the caps
    are born as `LEGACY_ROOT` (metric `mdb_legacy_roots`; boundary I.1
    frozen). Revoking an untyped's slot removes all its retyped-cap
    descendance; the objects die when their references drain and their
    destructors return the storage (child_count--). `UNTYPED_RESET` still
    requires `child_count == 0`: child_count is the truth of the OBJECT
    LIFECYCLE (an object can stay alive through handles or blocked tasks with
    no slot at all); the CDT is the truth of AUTHORITY. Reset can never reuse
    storage with live objects because the destructor is the only thing that
    decrements child_count.
15. **Object lifecycle**: the MDB only moves references (per-slot
    retain/release). The last reference triggers the object's destructor; an
    executing TCB keeps the scheduler's execution reference, so revoking all
    its authority does NOT free its backing (it becomes authority-less →
    terminates → destructor). Authority, functional close, destruction and
    storage release are four distinct events (charter O3-O5).
16. **Process teardown**: `handle_table_close_all` drops the root CNode; the
    CNode close deletes each slot with the DELETE primitive, so descendants
    living in other CNodes survive, reparented to surviving ancestors; no link
    points at the dead CNode (all its slots leave the graph before the storage
    can be released).
17. **Locking**: a single global MDB lock (`mdb_lock`, irq-spinlock) protects
    ALL derivation links and all slot-occupancy mutations. Fixed order:
    `mdb_lock → cn->lock` (never the reverse). Resolver readers (kcnode_fetch /
    cspace walk) keep using only `cn->lock`: they do not read MDB links.
    FORBIDDEN to run destructive callbacks (kobject_active_release /
    kobject_release) while holding `mdb_lock` — every release happens after
    releasing the lock (CNode destructors re-enter the MDB). The lock does not
    sleep. This is the S3 strategy (uniprocessor); its per-CNode refinement is
    a prerequisite of SMP (Stage 9), not of this stage — but correctness no
    longer depends on "there is no preemption": it depends on the lock.
18. **Complexity**: copy/mint/install O(1); move O(direct children); delete
    O(direct children) (splice); revoke O(subtree nodes) with O(subtree) lock
    acquisitions (one per victim — short IRQ-off windows); validator
    O(slots in the set²), tests only.
19. **Structural limits**: no dynamic allocation (nodes live in the slots);
    depth bounded only by the number of live slots; the validator imposes a
    sanity cap (2^20) against cycles.
20. **Temporary divergences from seL4** (ledger):
    - LEGACY roots (handle origin) — retired with Stages 2-4;
    - handle-only `SYS_CAP_DERIVE/SYS_CAP_REVOKE` still exist (the handle
      table's parallel tree, frozen) — Stage 3;
    - IPC cap-transfer installs a LEGACY_ROOT (handle source) — Stage 2;
    - no badged-CNode guards and no CDT over nested Untyped→Untyped beyond
      alloc_parent;
    - delete reparents (seL4-MDB does it implicitly via list order);
      equivalent revocation semantics, documented here.

## 2. Canonical primitives (kcnode.c — the sole slot mutators)

```text
kcnode_slot_install_linked(cn, idx, obj, rights, badge, parent_cn, parent_idx, exclusive, legacy)
kcnode_slot_derive(src_cn, src_idx, dst_cn, dst_idx, requested, req_badge)  [dst exclusive]
kcnode_slot_move(src_cn, src_idx, dst_cn, dst_idx)                          [dst exclusive]
kcnode_slot_delete(cn, idx)          — reparent children, release refs outside the lock
kcnode_slot_revoke(cn, idx)          — post-order subtree, keeps the invoked slot
kcnode_swap → two moves via a logical temporary slot (same CNode)
kcnode_obj_close → slot_delete of every occupied slot
```

No syscall or TU touches `cn->slots[]` directly. `kcnode_mint*` remain
wrappers of `install_linked(legacy=1)` for the existing legacy paths
(bootstrap, proc_cspace_mint, receive-slot, CNODE_MOVE).

## 3. Central badge rule (a single function)

`mdb_badge_derive(src_badge, requested, obj_type)`:
- `requested == 0` → inherit `src_badge` (0 stays 0);
- `src_badge != 0 && requested != src_badge` → `ACCESS_DENIED` (never
  re-badge);
- `src_badge == 0 && requested != 0` → only `KOBJ_ENDPOINT` /
  `KOBJ_NOTIFICATION` (`INVALID_ARG` for the rest).

Used by ALL derivation (SYS_CSPACE_MINT, SYS_CSPACE_MINT_INTO and the legacy
proc_cspace_mint, which delegates to it).

## 4. Syscall surface (Fase S3)

| Syscall | Semantics |
|---|---|
| `SYS_CSPACE_MINT (114)` | copy/mint slot→slot within the caller's own CSpace. arg0 = src CPtr (CSpace only); arg1 = dest CNode CPtr (0 = root) \| dest slot << 32; arg2 = rights (RIGHT_SAME_RIGHTS ⇒ copy) \| badge << 32. Exclusive install. |
| `SYS_CSPACE_REVOKE (115)` | revokes the descendants of the slot in arg0 (own CPtr); the slot survives. Returns the number of revoked nodes. |
| `SYS_CSPACE_MINT_INTO (116)` | cross-process mint: arg0 = target process (dual, RIGHT_WRITE — existing target pattern); arg1 = destination slot in its root CNode; arg2 = src CPtr (caller's CSpace only); arg3 = rights \| badge << 32. Exclusive install; MDB child of the caller's source slot. |

`SYS_CNODE_DELETE` keeps its number and gains the delete-with-reparent
semantics. `SYS_CNODE_SWAP` repairs links. `SYS_CNODE_MOVE` (handle→slot)
still produces a `LEGACY_ROOT`.

## 5. Structural invariants (validator `kcnode_mdb_validate`)

B.3-1..14 of the stage contract: empty slot ⇔ empty metadata; no cycles; at
most one parent; bidirectionally coherent sibling lists;
`parent.first_child` reaches exactly its children; siblings survive a
foreign revoke; move preserves descendance; delete ≠ revoke; slot reuse
inherits no metadata; no link to a destroyed CNode (guaranteed by close);
a failure ⇒ the graph is intact. The validator runs over a closed set of
CNodes (host tests and model-based fuzzing); it adds no cost to the
productive path.

## 6. Atomicity

Every operation follows `validate → prepare → commit under mdb_lock →
lifecycle effects outside the lock`. A failure before the commit mutates
nothing (exclusive installs re-check occupancy under the lock). Revoke is a
sequence of individually atomic leaf deletes in deterministic order — a cut
partway leaves a smaller valid subtree, never dangling links. RETYPE2's
publication rollback deletes exactly the freshly installed leaf nodes (with
no children by construction).
