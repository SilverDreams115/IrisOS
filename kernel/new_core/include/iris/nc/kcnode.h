#ifndef IRIS_NC_KCNODE_H
#define IRIS_NC_KCNODE_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <stdint.h>

/* Maximum and default slot counts.  slot_count MUST be a power-of-2 for
 * CSpace traversal (ctzll-based radix extraction).  KCNODE_MAX_SLOTS raised
 * from 256 to 4096 (= 2^12) to support wide CSpace fanout. */
#define KCNODE_MAX_SLOTS     4096u
#define KCNODE_DEFAULT_SLOTS  256u   /* used by UNTYPED_RETYPE when no count given */

/* Stage 8-cap / D-2 — guard limits.  A CPtr owns the low 31 bits (the top of
 * the value space is the retired handle range, see nc/handle.h), so a guard
 * can never be wider than that and guard+radix must fit inside it. */
#define CSPACE_CPTR_BITS       31u
#define KCNODE_GUARD_BITS_MAX  ((uint8_t)CSPACE_CPTR_BITS)

struct KCNode;

/* Phase S3 — MDB flags (per-slot derivation metadata). */
#define MDB_FLAG_LEGACY_ROOT 0x1u  /* installed from a non-CSpace origin
                                    * (handle / bootstrap / IPC delivery):
                                    * counted debt, see cspace-cdt-mdb.md §1.5 */

struct KCSlot {
    struct KObject *object;
    iris_rights_t   rights;
    uint64_t        badge;   /* Phase 9: per-cap sender identity.  0 =
                              * unbadged.  Set only at mint time by the
                              * minting authority; copy/move/swap preserve
                              * it; it never crosses to other caps. */
    /*
     * Phase S3 — MDB/CDT: intrusive derivation links.  The capability IS the
     * slot; its derivation node lives inside it (zero allocation per op).
     * Valid only while object != NULL; an empty slot has every link NULL and
     * flags 0 (invariant B.3-1).  Links are direct slot pointers — stable for
     * the owning CNode's lifetime; CNode close removes every slot from the
     * graph before the storage can be released, so no link ever points into
     * a destroyed CNode.  All links are guarded by the global mdb_lock
     * (kcnode.c); readers of object/rights/badge keep using cn->lock only.
     */
    struct KCSlot  *mdb_parent;       /* NULL = root (always LEGACY or promoted) */
    struct KCSlot  *mdb_first_child;
    struct KCSlot  *mdb_next_sib;     /* doubly-linked sibling list */
    struct KCSlot  *mdb_prev_sib;
    struct KCNode  *mdb_cnode;        /* owning CNode (set while occupied) */
    uint32_t        mdb_flags;

    /*
     * Stage 8-cap / D-2 — CNode GUARD, meaningful only when `object` is a
     * KOBJ_CNODE.  seL4 puts the guard in the CAPABILITY, not in the CNode
     * object (cap_cnode_cap_get_capCNodeGuard), so two capabilities to the
     * same CNode can carry different guards.  A KCSlot IS the capability in
     * IRIS, so the guard lives here.
     *
     * `guard_bits == 0` (the default for every slot ever installed) means "no
     * guard", which is exactly the pre-guard walk.  That is what makes this
     * additive: nothing in the tree sets a guard until it asks for one.
     *
     * Bit order within one level of a CPtr, MSB..LSB: [guard][index] — the
     * same relative order seL4 resolves in, so a CSpace laid out for seL4
     * addresses the same way here.
     */
    uint64_t        guard;
    uint8_t         guard_bits;
};

/*
 * Variable-capacity capability node.
 *
 * Memory layout (one contiguous allocation):
 *   [struct KCNode header][struct KCSlot slots[slot_count]]
 *
 * The slots pointer always points to (this + 1): the KCSlot array immediately
 * follows the header.  Use KCNODE_ALLOC_SIZE(n) to compute the required bytes.
 *
 * slot_count MUST be a power-of-2 ≥ 1.  Enforced by kcnode_alloc and
 * sys_cnode_create.  This constraint enables O(1) CSpace traversal via
 * ctzll(slot_count) bits per level.
 */
/*
 * `is_root` REMOVED (Stage 7-proc cleanup).
 *
 * It marked a CNode as some process's root CSpace and made the claim
 * exclusive, because teardown was per-process: `kprocess_teardown` emptied a
 * root's slots before dropping its refs, so two processes sharing one root
 * CNode would have had the first one's death empty the second's CSpace.
 *
 * Both halves of that reasoning are gone.  There is no process object and no
 * per-process teardown; a CNode is emptied by its OWN close hook when its last
 * external capability goes.  Threads sharing a CSpace is not a hazard to
 * refuse — it is what a "process" IS.  Nothing had set the flag since
 * SYS_PROCESS_CREATE retired.
 */
struct KCNode {
    struct KObject   base;       /* must be first */
    irq_spinlock_t   lock;
    uint32_t         slot_count;
    struct KCSlot   *slots;      /* inline array immediately after header */
};

/* Total allocation size for a KCNode with n slots */
#define KCNODE_ALLOC_SIZE(n) \
    ((uint32_t)(sizeof(struct KCNode) + (uint32_t)((n) * sizeof(struct KCSlot))))

struct KCNode *kcnode_alloc(uint32_t num_slots);
struct KCNode *kcnode_alloc_at(void *mem, uint32_t num_slots); /* untyped-backed */
void           kcnode_close(struct KCNode *cn);

/*
 * Empty every slot of `cn` with DELETE semantics, exactly as the object's
 * close callback does, but WITHOUT waiting for the last reference to go.
 *
 * Stage 5 Step 3: a CSpace can contain a capability to one of its own CNodes
 * — the root task holds a capability to its own root CNode, the way seL4's
 * root task does — and a slot holds refs on the object it names.  A CNode
 * reachable from itself therefore never reaches zero references on its own,
 * so the close callback that would empty it never runs and the whole subtree
 * outlives the process.  Process teardown calls this first: emptying the slots
 * is what breaks the cycle, and doing it explicitly does not depend on a
 * refcount that the cycle is holding up.
 *
 * Idempotent: deleting an empty slot is a no-op, so the close callback running
 * afterwards finds nothing left to do.
 */
void           kcnode_teardown_slots(struct KCNode *cn);


/* Phase 18: live KCNode object count (additive diagnostics). */
uint32_t       kcnode_live_count(void);
/* Phase S2: CSpace-native derivation (CDT/MDB) instrumentation. */
void           kcnode_cdt_note_legacy_migrated_derivation(void);
void           kcnode_cdt_stats(uint32_t *deriv, uint32_t *deriv_hwm,
                                uint32_t *revoke, uint32_t *del,
                                uint32_t *cross, uint32_t *ipc,
                                uint32_t *legacy_migrated);
iris_error_t   kcnode_mint(struct KCNode *cn, uint32_t slot_idx,
                            struct KObject *obj, iris_rights_t rights);
/* Exclusive mint (Phase 8): fails with IRIS_ERR_ALREADY_EXISTS if the slot
 * is occupied instead of silently replacing the cap.  Used by
 * SYS_PROC_CSPACE_MINT so a spawner cannot clobber a child's slots. */
iris_error_t   kcnode_mint_excl(struct KCNode *cn, uint32_t slot_idx,
                                 struct KObject *obj, iris_rights_t rights);
/* Overwrite mint preserving an explicit badge (Phase 9; MOVE path). */
iris_error_t   kcnode_mint_badged(struct KCNode *cn, uint32_t slot_idx,
                                   struct KObject *obj, iris_rights_t rights,
                                   uint64_t badge);
/* Badged exclusive mint (Phase 9): like kcnode_mint_excl but also records
 * the per-cap badge in the slot. */
iris_error_t   kcnode_mint_excl_badged(struct KCNode *cn, uint32_t slot_idx,
                                        struct KObject *obj,
                                        iris_rights_t rights, uint64_t badge);
iris_error_t   kcnode_fetch(struct KCNode *cn, uint32_t slot_idx,
                             struct KObject **out_obj, iris_rights_t *out_rights);
/* Badge-aware fetch (Phase 9): also returns the slot badge. */
iris_error_t   kcnode_fetch_badged(struct KCNode *cn, uint32_t slot_idx,
                                    struct KObject **out_obj,
                                    iris_rights_t *out_rights,
                                    uint64_t *out_badge);
/* Stage 8-cap: fetch that also reports the capability's GUARD.  Only the
 * CSpace walk needs it, and only when the fetched capability is a CNode it is
 * about to descend into; every other caller uses the badged form. */
iris_error_t   kcnode_fetch_guarded(struct KCNode *cn, uint32_t slot_idx,
                                    struct KObject **out_obj,
                                    iris_rights_t *out_rights,
                                    uint64_t *out_badge,
                                    uint64_t *out_guard,
                                    uint8_t  *out_guard_bits);

/* Stage 8-cap: set the guard on a CNode capability already installed in a
 * slot.  IRIS_ERR_WRONG_TYPE if the slot does not hold a KOBJ_CNODE;
 * IRIS_ERR_INVALID_ARG if the guard does not fit its declared width or the
 * level would exceed the CPtr space.  A guard is capability-local: setting it
 * on one capability does not affect any other capability to the same CNode. */
iris_error_t   kcnode_slot_set_guard(struct KCNode *cn, uint32_t slot_idx,
                                     uint64_t guard, uint8_t guard_bits);
iris_error_t   kcnode_delete(struct KCNode *cn, uint32_t slot_idx);
iris_error_t   kcnode_swap(struct KCNode *cn, uint32_t slot_a, uint32_t slot_b);

/* ── Phase S3: canonical slot primitives (the ONLY slot mutators) ──────────
 * See docs/architecture/cspace-cdt-mdb.md.  Every install/delete/move/revoke
 * of a CSpace slot goes through these; no TU touches cn->slots[] directly.
 */

/* Install obj into (cn, idx) as an MDB node.  parent_cn == NULL → root
 * (legacy != 0 marks MDB_FLAG_LEGACY_ROOT — non-CSpace origin); otherwise
 * the new cap is a child of (parent_cn, parent_idx), which must be occupied.
 * exclusive != 0 → IRIS_ERR_ALREADY_EXISTS on an occupied slot; otherwise
 * the old occupant is deleted first (full delete-with-reparent semantics).
 * Takes its own retain + active_retain on obj.  On error nothing changes. */
/* 1 when `slot_idx` of `cn` holds exactly `obj` — identity, not occupancy.
 * See the definition for why a deferred MDB parent needs this. */
int kcnode_slot_holds(struct KCNode *cn, uint32_t slot_idx,
                      const struct KObject *obj);

iris_error_t kcnode_slot_install_linked(struct KCNode *cn, uint32_t slot_idx,
                                        struct KObject *obj,
                                        iris_rights_t rights, uint64_t badge,
                                        struct KCNode *parent_cn,
                                        uint32_t parent_idx,
                                        int exclusive, int legacy);

/* Derive (copy/mint) from source slot into dest slot: reads object, rights
 * and badge FROM the source, applies rights_reduce(src, requested) (requires
 * RIGHT_DUPLICATE on the source; collapse to NONE → INVALID_ARG) and the
 * central badge rule (mdb_badge_derive), then installs the result as an MDB
 * CHILD of the source.  Exclusive: occupied dest → ALREADY_EXISTS.
 * requested == RIGHT_SAME_RIGHTS is a COPY.  On error nothing changes. */
iris_error_t kcnode_slot_derive(struct KCNode *src_cn, uint32_t src_idx,
                                struct KCNode *dst_cn, uint32_t dst_idx,
                                iris_rights_t requested, uint64_t req_badge);

/* Move the capability AND its MDB node from src to dst (dst must be empty):
 * object/rights/badge/flags transfer, parent + sibling links are repaired,
 * children re-point their parent to dst, src ends fully empty.  No net
 * refcount change.  A failed move leaves both slots untouched. */
iris_error_t kcnode_slot_move(struct KCNode *src_cn, uint32_t src_idx,
                              struct KCNode *dst_cn, uint32_t dst_idx);

/* Delete ONLY this capability.  Children are spliced into the deleted
 * node's parent (grandparent adoption) so every surviving ancestor keeps
 * revocation authority; if the deleted node was a root, children are
 * promoted to roots (counted in mdb_orphan_promotions).  Empty slot → OK
 * (idempotent).  Object refs are released OUTSIDE the MDB lock. */
iris_error_t kcnode_slot_delete(struct KCNode *cn, uint32_t slot_idx);

/* Revoke: delete the ENTIRE descendant subtree of (cn, idx) — across CNodes
 * and processes — in deterministic deepest-leftmost-first order.  The
 * invoked slot itself survives.  Siblings are untouched.  out_revoked
 * (optional) receives the number of capabilities destroyed. */
/*
 * Stage 9-evt / ledger D-8 — revoke at most `budget` capabilities, then stop.
 *
 * `*out_more` says whether the subtree still has descendants, i.e. whether the
 * caller must come back.  Unbounded revoke is the one in-kernel operation a
 * ring-3 principal could make arbitrarily long simply by building a wide
 * derivation tree, and seL4 answers it with a preemption point rather than a
 * faster loop.  So does this: the slice is bounded, the caller re-executes,
 * and the tree is strictly smaller every time — which is what makes the
 * re-execution terminate without needing a cursor.
 *
 * `budget == 0` means unbounded, for the kernel-internal callers that are not
 * running on a principal's behalf and have nowhere to return to.
 */
iris_error_t kcnode_slot_revoke_bounded(struct KCNode *cn, uint32_t slot_idx,
                                        uint32_t budget, uint32_t *out_revoked,
                                        int *out_more);
iris_error_t kcnode_slot_revoke(struct KCNode *cn, uint32_t slot_idx,
                                uint32_t *out_revoked);

/* Central badge-derivation rule (charter/cspace-cdt-mdb.md §3) — the ONLY
 * place badge semantics live.  Returns IRIS_OK and *out_badge, or
 * ACCESS_DENIED (re-badge attempt) / INVALID_ARG (fresh badge on a type
 * that cannot carry one). */
iris_error_t mdb_badge_derive(uint64_t src_badge, uint64_t requested,
                              uint32_t obj_type, uint64_t *out_badge);

/* ── Phase S3: MDB validator (test/diagnostic use; closed CNode set) ────── */
struct mdb_validate_report {
    uint32_t errors;        /* invariant violations found */
    uint32_t nodes;         /* occupied slots in the set */
    uint32_t roots;         /* parentless nodes */
    uint32_t legacy_roots;  /* roots flagged MDB_FLAG_LEGACY_ROOT */
    uint32_t max_depth;     /* deepest node observed */
};
uint32_t kcnode_mdb_validate(struct KCNode **set, uint32_t n,
                             struct mdb_validate_report *rep);

/* Phase S3 — MDB gauges/counters (diagnostics; QUERY kind 4). */
void kcnode_mdb_stats(uint32_t *nodes_live, uint32_t *nodes_hwm,
                      uint32_t *legacy_roots, uint32_t *orphan_promotions,
                      uint32_t *reparents, uint32_t *revoked_nodes,
                      uint32_t *moves, uint32_t *max_depth);
/* IPC receive-slot delivery instrumentation (called by syscall_endpoint.c). */
void kcnode_cdt_note_ipc_transfer(void);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KCNODE_H */
