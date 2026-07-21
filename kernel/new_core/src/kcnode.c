/*
 * kcnode.c — capability nodes + native MDB/CDT (Fase S3).
 *
 * The capability IS the slot: object/rights/badge plus an intrusive
 * derivation node (parent / first-child / doubly-linked siblings).  These
 * primitives are the ONLY slot mutators in the kernel; every syscall and
 * every internal path (bootstrap, retype publish, IPC receive-slot, process
 * teardown) goes through them.  Contract: docs/architecture/cspace-cdt-mdb.md.
 *
 * Locking (S3 contract):
 *   - mdb_lock (global, IRQ-off) guards ALL derivation links and every slot
 *     occupancy transition.  Order: mdb_lock → cn->lock, never inverted.
 *   - cn->lock alone still protects reads of object/rights/badge (the
 *     resolver / kcnode_fetch never look at MDB links).
 *   - kobject_active_release / kobject_release are NEVER called under
 *     mdb_lock: destroying a CNode re-enters these primitives (its close
 *     deletes every slot).  Every mutator collects the victim object and
 *     releases it after unlocking.
 */
#include <iris/nc/kcnode.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/rights.h>
#include <iris/kslab.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kcnode_live;

/* Fase 18 — live KCNode object count (additive diagnostics). */
uint32_t kcnode_live_count(void) {
    return atomic_load_explicit(&kcnode_live, memory_order_relaxed);
}

/* ── Fase S2/S3 — derivation instrumentation ────────────────────────────── */

static _Atomic uint32_t cdt_derivation_count;   /* derived installs (copy/mint/retype-child) */
static _Atomic uint32_t cdt_derivation_hwm;     /* max live derived nodes */
static _Atomic uint32_t cdt_derived_live;
static _Atomic uint32_t cdt_revoke_count;       /* revoke invocations */
static _Atomic uint32_t cdt_delete_count;       /* slot deletes (occupied) */
static _Atomic uint32_t cdt_cross_cnode_desc;   /* derived installs across CNodes */
static _Atomic uint32_t cdt_ipc_transfer_count; /* receive-slot deliveries */
static _Atomic uint32_t legacy_handle_deriv_migrated;

static _Atomic uint32_t mdb_nodes_live;
static _Atomic uint32_t mdb_nodes_hwm;
static _Atomic uint32_t mdb_legacy_roots;       /* live LEGACY_ROOT nodes */
static _Atomic uint32_t mdb_orphan_promotions;  /* children promoted to root */
static _Atomic uint32_t mdb_reparents;          /* children adopted by grandparent */
static _Atomic uint32_t mdb_revoked_nodes;      /* caps destroyed by revoke */
static _Atomic uint32_t mdb_moves;
static _Atomic uint32_t mdb_max_depth;

void kcnode_cdt_note_legacy_migrated_derivation(void) {
    atomic_fetch_add_explicit(&legacy_handle_deriv_migrated, 1u, memory_order_relaxed);
}

void kcnode_cdt_note_ipc_transfer(void) {
    atomic_fetch_add_explicit(&cdt_ipc_transfer_count, 1u, memory_order_relaxed);
}

void kcnode_cdt_stats(uint32_t *deriv, uint32_t *deriv_hwm, uint32_t *revoke,
                      uint32_t *del, uint32_t *cross, uint32_t *ipc,
                      uint32_t *legacy_migrated) {
    if (deriv)     *deriv     = atomic_load_explicit(&cdt_derivation_count, memory_order_relaxed);
    if (deriv_hwm) *deriv_hwm = atomic_load_explicit(&cdt_derivation_hwm,   memory_order_relaxed);
    if (revoke)    *revoke    = atomic_load_explicit(&cdt_revoke_count,     memory_order_relaxed);
    if (del)       *del       = atomic_load_explicit(&cdt_delete_count,     memory_order_relaxed);
    if (cross)     *cross     = atomic_load_explicit(&cdt_cross_cnode_desc, memory_order_relaxed);
    if (ipc)       *ipc       = atomic_load_explicit(&cdt_ipc_transfer_count, memory_order_relaxed);
    if (legacy_migrated)
        *legacy_migrated = atomic_load_explicit(&legacy_handle_deriv_migrated, memory_order_relaxed);
}

void kcnode_mdb_stats(uint32_t *nodes_live, uint32_t *nodes_hwm,
                      uint32_t *legacy_roots, uint32_t *orphan_promotions,
                      uint32_t *reparents, uint32_t *revoked_nodes,
                      uint32_t *moves, uint32_t *max_depth) {
    if (nodes_live)        *nodes_live        = atomic_load_explicit(&mdb_nodes_live, memory_order_relaxed);
    if (nodes_hwm)         *nodes_hwm         = atomic_load_explicit(&mdb_nodes_hwm, memory_order_relaxed);
    if (legacy_roots)      *legacy_roots      = atomic_load_explicit(&mdb_legacy_roots, memory_order_relaxed);
    if (orphan_promotions) *orphan_promotions = atomic_load_explicit(&mdb_orphan_promotions, memory_order_relaxed);
    if (reparents)         *reparents         = atomic_load_explicit(&mdb_reparents, memory_order_relaxed);
    if (revoked_nodes)     *revoked_nodes     = atomic_load_explicit(&mdb_revoked_nodes, memory_order_relaxed);
    if (moves)             *moves             = atomic_load_explicit(&mdb_moves, memory_order_relaxed);
    if (max_depth)         *max_depth         = atomic_load_explicit(&mdb_max_depth, memory_order_relaxed);
}

/* ── MDB core (all link surgery under mdb_lock) ─────────────────────────── */

/* Static zero-init is a valid unlocked state (atomic_flag clear). */
static irq_spinlock_t mdb_lock;

static void mdb_note_max(_Atomic uint32_t *hwm, uint32_t v) {
    uint32_t cur = atomic_load_explicit(hwm, memory_order_relaxed);
    while (v > cur &&
           !atomic_compare_exchange_weak_explicit(hwm, &cur, v,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
}

static void mdb_node_count_inc(void) {
    uint32_t n = atomic_fetch_add_explicit(&mdb_nodes_live, 1u, memory_order_relaxed) + 1u;
    mdb_note_max(&mdb_nodes_hwm, n);
}

/* Depth of a node (root = 0).  mdb_lock held. */
static uint32_t mdb_depth_of(struct KCSlot *s) {
    uint32_t d = 0;
    for (struct KCSlot *p = s->mdb_parent; p; p = p->mdb_parent) d++;
    return d;
}

/* Link `child` as a new child of `parent` (prepend).  mdb_lock held. */
static void mdb_link_child(struct KCSlot *parent, struct KCSlot *child) {
    child->mdb_parent   = parent;
    child->mdb_prev_sib = 0;
    child->mdb_next_sib = parent->mdb_first_child;
    if (parent->mdb_first_child)
        parent->mdb_first_child->mdb_prev_sib = child;
    parent->mdb_first_child = child;
}

/* Unlink `s` from its parent's child list (or from nothing if root).
 * Does NOT touch s's own children.  mdb_lock held. */
static void mdb_unlink_from_parent(struct KCSlot *s) {
    if (s->mdb_prev_sib) s->mdb_prev_sib->mdb_next_sib = s->mdb_next_sib;
    else if (s->mdb_parent) s->mdb_parent->mdb_first_child = s->mdb_next_sib;
    if (s->mdb_next_sib) s->mdb_next_sib->mdb_prev_sib = s->mdb_prev_sib;
    s->mdb_prev_sib = 0;
    s->mdb_next_sib = 0;
}

/* Clear every MDB field of a slot (must already be unlinked and childless).
 * mdb_lock held. */
static void mdb_clear_node(struct KCSlot *s) {
    s->mdb_parent      = 0;
    s->mdb_first_child = 0;
    s->mdb_next_sib    = 0;
    s->mdb_prev_sib    = 0;
    s->mdb_cnode       = 0;
    s->mdb_flags       = 0;
}

/*
 * Detach `s` from the graph with DELETE semantics: children are spliced
 * into s's parent (grandparent adoption); if s is a root, each child is
 * promoted to an independent root (counted).  mdb_lock held.  Accounting
 * for the node itself (nodes_live--, legacy_roots--) happens here.
 */
static void mdb_detach_reparent(struct KCSlot *s) {
    struct KCSlot *parent = s->mdb_parent;
    struct KCSlot *child  = s->mdb_first_child;

    if (child) {
        if (parent) {
            /* Splice the whole child list into parent's children. */
            uint32_t n = 0;
            struct KCSlot *last = 0;
            for (struct KCSlot *c = child; c; c = c->mdb_next_sib) {
                c->mdb_parent = parent;
                last = c; n++;
            }
            last->mdb_next_sib = parent->mdb_first_child;
            if (parent->mdb_first_child)
                parent->mdb_first_child->mdb_prev_sib = last;
            parent->mdb_first_child = child;
            /* `child` becomes the new list head; its prev must be NULL
             * (it already is: it was s's first child). */
            atomic_fetch_add_explicit(&mdb_reparents, n, memory_order_relaxed);
        } else {
            /* Root deletion: no surviving ancestor — promote children.
             * A promoted child stops being a "derived live" node. */
            struct KCSlot *c = child;
            while (c) {
                struct KCSlot *next = c->mdb_next_sib;
                c->mdb_parent   = 0;
                c->mdb_next_sib = 0;
                c->mdb_prev_sib = 0;
                atomic_fetch_add_explicit(&mdb_orphan_promotions, 1u, memory_order_relaxed);
                atomic_fetch_sub_explicit(&cdt_derived_live, 1u, memory_order_relaxed);
                c = next;
            }
        }
        s->mdb_first_child = 0;
    }

    if (s->mdb_parent)
        atomic_fetch_sub_explicit(&cdt_derived_live, 1u, memory_order_relaxed);
    mdb_unlink_from_parent(s);
    if (s->mdb_flags & MDB_FLAG_LEGACY_ROOT)
        atomic_fetch_sub_explicit(&mdb_legacy_roots, 1u, memory_order_relaxed);
    atomic_fetch_sub_explicit(&mdb_nodes_live, 1u, memory_order_relaxed);
    mdb_clear_node(s);
}

/* ── central badge rule (the ONLY badge-derivation logic) ───────────────── */

iris_error_t mdb_badge_derive(uint64_t src_badge, uint64_t requested,
                              uint32_t obj_type, uint64_t *out_badge) {
    if (!out_badge) return IRIS_ERR_INVALID_ARG;
    if (requested == 0u) {                    /* inherit (0 stays 0) */
        *out_badge = src_badge;
        return IRIS_OK;
    }
    if (src_badge != 0u && src_badge != requested)
        return IRIS_ERR_ACCESS_DENIED;        /* a badged cap is never re-badged */
    if (src_badge == 0u &&
        obj_type != (uint32_t)KOBJ_ENDPOINT &&
        obj_type != (uint32_t)KOBJ_NOTIFICATION)
        return IRIS_ERR_INVALID_ARG;          /* fresh badge: identity types only */
    *out_badge = requested;
    return IRIS_OK;
}

/* ── object lifecycle callbacks ─────────────────────────────────────────── */

static void kcnode_obj_close(struct KObject *obj) {
    struct KCNode *cn = (struct KCNode *)obj;
    /* Teardown uses DELETE semantics per slot: descendants living in other
     * CNodes survive, reparented to surviving ancestors (G.2/G.3).  The
     * primitive releases object refs outside the MDB lock, so a cascading
     * CNode destruction re-enters safely. */
    for (uint32_t i = 0u; i < cn->slot_count; i++)
        (void)kcnode_slot_delete(cn, i);
}

static void kcnode_obj_destroy(struct KObject *obj) {
    struct KCNode *cn = (struct KCNode *)obj;
    kcnode_obj_close(obj);
    atomic_fetch_sub_explicit(&kcnode_live, 1u, memory_order_relaxed);
    kslab_free(cn, KCNODE_ALLOC_SIZE(cn->slot_count));
}

static const struct KObjectOps kcnode_ops = {
    .close   = kcnode_obj_close,
    .destroy = kcnode_obj_destroy,
};

/* ── Untyped-backed variant (Ph79) ──────────────────────────────── */

static void kcnode_obj_destroy_ut(struct KObject *obj) {
    struct KCNode *cn = (struct KCNode *)obj;
    kcnode_obj_close(obj);
    atomic_fetch_sub_explicit(&kcnode_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, KCNODE_ALLOC_SIZE(cn->slot_count));
}

static const struct KObjectOps kcnode_ops_ut = {
    .close   = kcnode_obj_close,
    .destroy = kcnode_obj_destroy_ut,
};

struct KCNode *kcnode_alloc_at(void *mem, uint32_t num_slots) {
    if (!mem || num_slots == 0u || num_slots > KCNODE_MAX_SLOTS) return 0;
    if (num_slots & (num_slots - 1u)) return 0; /* must be power-of-2 */
    struct KCNode *cn = (struct KCNode *)mem;
    kobject_init(&cn->base, KOBJ_CNODE, &kcnode_ops_ut);
    irq_spinlock_init(&cn->lock);
    cn->slot_count = num_slots;
    cn->slots = (struct KCSlot *)(cn + 1); /* inline array after header */
    /* slots[] already zeroed by kuntyped_alloc_child (empty ⇔ MDB empty) */
    atomic_fetch_add_explicit(&kcnode_live, 1u, memory_order_relaxed);
    return cn;
}

struct KCNode *kcnode_alloc(uint32_t num_slots) {
    if (num_slots == 0u || num_slots > KCNODE_MAX_SLOTS) return 0;
    if (num_slots & (num_slots - 1u)) return 0; /* must be power-of-2 */
    struct KCNode *cn = kslab_alloc(KCNODE_ALLOC_SIZE(num_slots));
    if (!cn) return 0;
    kobject_init(&cn->base, KOBJ_CNODE, &kcnode_ops);
    irq_spinlock_init(&cn->lock);
    cn->slot_count = num_slots;
    cn->slots = (struct KCSlot *)(cn + 1); /* inline array after header */
    /* slots[] zeroed by kslab_alloc (empty ⇔ MDB empty) */
    atomic_fetch_add_explicit(&kcnode_live, 1u, memory_order_relaxed);
    return cn;
}

void kcnode_close(struct KCNode *cn) {
    if (!cn) return;
    kobject_release(&cn->base);
}

/* ── canonical slot primitives ──────────────────────────────────────────── */

iris_error_t kcnode_slot_install_linked(struct KCNode *cn, uint32_t slot_idx,
                                        struct KObject *obj,
                                        iris_rights_t rights, uint64_t badge,
                                        struct KCNode *parent_cn,
                                        uint32_t parent_idx,
                                        int exclusive, int legacy) {
    if (!cn || !obj || rights == RIGHT_NONE) return IRIS_ERR_INVALID_ARG;
    if (parent_cn && parent_idx >= parent_cn->slot_count) return IRIS_ERR_INVALID_ARG;

    /* Overwrite semantics (legacy kcnode_mint): clear the old occupant first
     * with full delete-with-reparent semantics.  Uniprocessor, non-preemptive
     * syscall context: no mutator can slip between the delete and the
     * exclusive install below (and if one ever could, the install fails
     * cleanly with ALREADY_EXISTS instead of corrupting the graph). */
    if (!exclusive) {
        if (slot_idx >= cn->slot_count) return IRIS_ERR_INVALID_ARG;
        iris_error_t de = kcnode_slot_delete(cn, slot_idx);
        if (de != IRIS_OK) return de;
    }

    kobject_retain(obj);
    kobject_active_retain(obj);

    uint64_t mf = irq_spinlock_lock(&mdb_lock);

    if (slot_idx >= cn->slot_count) {
        irq_spinlock_unlock(&mdb_lock, mf);
        kobject_active_release(obj);
        kobject_release(obj);
        return IRIS_ERR_INVALID_ARG;
    }
    struct KCSlot *s = &cn->slots[slot_idx];
    struct KCSlot *parent = 0;
    if (parent_cn) {
        parent = &parent_cn->slots[parent_idx];
        if (!parent->object || parent == s) {
            irq_spinlock_unlock(&mdb_lock, mf);
            kobject_active_release(obj);
            kobject_release(obj);
            return IRIS_ERR_INVALID_ARG;
        }
    }

    uint64_t cf = irq_spinlock_lock(&cn->lock);
    if (s->object) {
        irq_spinlock_unlock(&cn->lock, cf);
        irq_spinlock_unlock(&mdb_lock, mf);
        kobject_active_release(obj);
        kobject_release(obj);
        return IRIS_ERR_ALREADY_EXISTS;
    }
    s->object = obj;
    s->rights = rights;
    s->badge  = badge;
    irq_spinlock_unlock(&cn->lock, cf);

    s->mdb_cnode = cn;
    if (parent) {
        mdb_link_child(parent, s);
        s->mdb_flags = 0;
        uint32_t d = mdb_depth_of(s);
        mdb_note_max(&mdb_max_depth, d);
        atomic_fetch_add_explicit(&cdt_derivation_count, 1u, memory_order_relaxed);
        uint32_t dl = atomic_fetch_add_explicit(&cdt_derived_live, 1u, memory_order_relaxed) + 1u;
        mdb_note_max(&cdt_derivation_hwm, dl);
        if (parent_cn != cn)
            atomic_fetch_add_explicit(&cdt_cross_cnode_desc, 1u, memory_order_relaxed);
    } else {
        s->mdb_parent = 0;
        s->mdb_flags  = legacy ? MDB_FLAG_LEGACY_ROOT : 0u;
        if (legacy)
            atomic_fetch_add_explicit(&mdb_legacy_roots, 1u, memory_order_relaxed);
    }
    mdb_node_count_inc();

    irq_spinlock_unlock(&mdb_lock, mf);
    return IRIS_OK;
}

iris_error_t kcnode_slot_derive(struct KCNode *src_cn, uint32_t src_idx,
                                struct KCNode *dst_cn, uint32_t dst_idx,
                                iris_rights_t requested, uint64_t req_badge) {
    if (!src_cn || !dst_cn) return IRIS_ERR_INVALID_ARG;
    if (src_idx >= src_cn->slot_count || dst_idx >= dst_cn->slot_count)
        return IRIS_ERR_INVALID_ARG;
    if (src_cn == dst_cn && src_idx == dst_idx) return IRIS_ERR_INVALID_ARG;

    /* Read the source (content lock) — validate everything BEFORE touching
     * any state (C.1/C.2: an error leaves zero changes). */
    struct KObject *obj;
    iris_rights_t   src_rights;
    uint64_t        src_badge;
    {
        uint64_t cf = irq_spinlock_lock(&src_cn->lock);
        struct KCSlot *src = &src_cn->slots[src_idx];
        if (!src->object) {
            irq_spinlock_unlock(&src_cn->lock, cf);
            return IRIS_ERR_NOT_FOUND;
        }
        obj        = src->object;
        src_rights = src->rights;
        src_badge  = src->badge;
        irq_spinlock_unlock(&src_cn->lock, cf);
    }

    if (!rights_check(src_rights, RIGHT_DUPLICATE)) return IRIS_ERR_ACCESS_DENIED;
    iris_rights_t effective = rights_reduce(src_rights, requested);
    if (effective == RIGHT_NONE) return IRIS_ERR_INVALID_ARG;

    uint64_t eff_badge;
    iris_error_t be = mdb_badge_derive(src_badge, req_badge,
                                       (uint32_t)obj->type, &eff_badge);
    if (be != IRIS_OK) return be;

    /* Install as a child of the source slot.  The source's occupancy is
     * re-verified under mdb_lock by install_linked (parent must be live). */
    return kcnode_slot_install_linked(dst_cn, dst_idx, obj, effective,
                                      eff_badge, src_cn, src_idx,
                                      /*exclusive=*/1, /*legacy=*/0);
}

/*
 * mdb_relocate — move node links + content from slot `from` to empty slot
 * `to` (same or different CNode).  ALL incident links (parent's child ptr,
 * siblings, children's parent) are repaired.  mdb_lock held; content moves
 * under the respective cn locks.  `to` must be link-free and content-free.
 */
static void mdb_relocate(struct KCSlot *from, struct KCNode *from_cn,
                         struct KCSlot *to, struct KCNode *to_cn) {
    /* Content transfer (no net refcount change — the slot's refs move). */
    uint64_t cf = irq_spinlock_lock(&to_cn->lock);
    to->object = from->object;
    to->rights = from->rights;
    to->badge  = from->badge;
    irq_spinlock_unlock(&to_cn->lock, cf);
    cf = irq_spinlock_lock(&from_cn->lock);
    from->object = 0;
    from->rights = RIGHT_NONE;
    from->badge  = 0;
    irq_spinlock_unlock(&from_cn->lock, cf);

    /* Link transfer. */
    to->mdb_parent      = from->mdb_parent;
    to->mdb_first_child = from->mdb_first_child;
    to->mdb_next_sib    = from->mdb_next_sib;
    to->mdb_prev_sib    = from->mdb_prev_sib;
    to->mdb_flags       = from->mdb_flags;
    to->mdb_cnode       = to_cn;

    if (to->mdb_prev_sib) to->mdb_prev_sib->mdb_next_sib = to;
    else if (to->mdb_parent) to->mdb_parent->mdb_first_child = to;
    if (to->mdb_next_sib) to->mdb_next_sib->mdb_prev_sib = to;
    for (struct KCSlot *c = to->mdb_first_child; c; c = c->mdb_next_sib)
        c->mdb_parent = to;

    mdb_clear_node(from);
}

iris_error_t kcnode_slot_move(struct KCNode *src_cn, uint32_t src_idx,
                              struct KCNode *dst_cn, uint32_t dst_idx) {
    if (!src_cn || !dst_cn) return IRIS_ERR_INVALID_ARG;
    if (src_idx >= src_cn->slot_count || dst_idx >= dst_cn->slot_count)
        return IRIS_ERR_INVALID_ARG;
    if (src_cn == dst_cn && src_idx == dst_idx) return IRIS_ERR_INVALID_ARG;

    uint64_t mf = irq_spinlock_lock(&mdb_lock);
    struct KCSlot *src = &src_cn->slots[src_idx];
    struct KCSlot *dst = &dst_cn->slots[dst_idx];
    if (!src->object) {
        irq_spinlock_unlock(&mdb_lock, mf);
        return IRIS_ERR_NOT_FOUND;
    }
    if (dst->object) {
        irq_spinlock_unlock(&mdb_lock, mf);
        return IRIS_ERR_ALREADY_EXISTS;
    }
    mdb_relocate(src, src_cn, dst, dst_cn);
    atomic_fetch_add_explicit(&mdb_moves, 1u, memory_order_relaxed);
    irq_spinlock_unlock(&mdb_lock, mf);
    return IRIS_OK;
}

iris_error_t kcnode_slot_delete(struct KCNode *cn, uint32_t slot_idx) {
    if (!cn) return IRIS_ERR_INVALID_ARG;

    struct KObject *old = 0;

    uint64_t mf = irq_spinlock_lock(&mdb_lock);
    if (slot_idx >= cn->slot_count) {
        irq_spinlock_unlock(&mdb_lock, mf);
        return IRIS_ERR_INVALID_ARG;
    }
    struct KCSlot *s = &cn->slots[slot_idx];
    if (s->object) {
        uint64_t cf = irq_spinlock_lock(&cn->lock);
        old = s->object;
        s->object = 0;
        s->rights = RIGHT_NONE;
        s->badge  = 0;
        irq_spinlock_unlock(&cn->lock, cf);

        /* Every occupied slot is in the graph (B.3-2); mdb_cnode is the
         * witness (guards against a pre-MDB zeroed legacy state). */
        if (s->mdb_cnode)
            mdb_detach_reparent(s);
        atomic_fetch_add_explicit(&cdt_delete_count, 1u, memory_order_relaxed);
    }
    irq_spinlock_unlock(&mdb_lock, mf);

    if (old) {
        kobject_active_release(old);
        kobject_release(old);
    }
    return IRIS_OK;
}

iris_error_t kcnode_slot_revoke(struct KCNode *cn, uint32_t slot_idx,
                                uint32_t *out_revoked) {
    if (!cn || slot_idx >= cn->slot_count) return IRIS_ERR_INVALID_ARG;

    uint32_t revoked = 0;
    atomic_fetch_add_explicit(&cdt_revoke_count, 1u, memory_order_relaxed);

    for (;;) {
        struct KObject *victim_obj = 0;

        uint64_t mf = irq_spinlock_lock(&mdb_lock);
        struct KCSlot *root = &cn->slots[slot_idx];
        if (!root->object) {
            /* Invoked slot must exist (revoke keeps it; empty = NOT_FOUND
             * only when nothing was revoked yet — a mid-revoke vanish is
             * impossible on this uniprocessor path). */
            irq_spinlock_unlock(&mdb_lock, mf);
            if (revoked == 0) return IRIS_ERR_NOT_FOUND;
            break;
        }
        struct KCSlot *v = root->mdb_first_child;
        if (!v) {
            irq_spinlock_unlock(&mdb_lock, mf);
            break;                      /* subtree exhausted; root survives */
        }
        /* Deterministic order: deepest-leftmost leaf first. */
        while (v->mdb_first_child) v = v->mdb_first_child;

        struct KCNode *v_cn = v->mdb_cnode;
        uint64_t cf = irq_spinlock_lock(&v_cn->lock);
        victim_obj = v->object;
        v->object  = 0;
        v->rights  = RIGHT_NONE;
        v->badge   = 0;
        irq_spinlock_unlock(&v_cn->lock, cf);

        mdb_unlink_from_parent(v);
        if (v->mdb_flags & MDB_FLAG_LEGACY_ROOT)
            atomic_fetch_sub_explicit(&mdb_legacy_roots, 1u, memory_order_relaxed);
        atomic_fetch_sub_explicit(&mdb_nodes_live, 1u, memory_order_relaxed);
        atomic_fetch_sub_explicit(&cdt_derived_live, 1u, memory_order_relaxed);
        mdb_clear_node(v);
        atomic_fetch_add_explicit(&mdb_revoked_nodes, 1u, memory_order_relaxed);
        irq_spinlock_unlock(&mdb_lock, mf);

        /* Lifecycle effects strictly outside the MDB lock: this release may
         * close endpoints (waking blocked tasks) or cascade into destroying
         * a CNode whose close re-enters these primitives. */
        if (victim_obj) {
            kobject_active_release(victim_obj);
            kobject_release(victim_obj);
        }
        revoked++;
    }

    if (out_revoked) *out_revoked = revoked;
    return IRIS_OK;
}

/* ── legacy-compatible wrappers (all installs are LEGACY roots) ─────────── */

iris_error_t kcnode_mint(struct KCNode *cn, uint32_t slot_idx,
                          struct KObject *obj, iris_rights_t rights) {
    return kcnode_slot_install_linked(cn, slot_idx, obj, rights, 0, 0, 0,
                                      /*exclusive=*/0, /*legacy=*/1);
}

iris_error_t kcnode_mint_badged(struct KCNode *cn, uint32_t slot_idx,
                                struct KObject *obj, iris_rights_t rights,
                                uint64_t badge) {
    return kcnode_slot_install_linked(cn, slot_idx, obj, rights, badge, 0, 0,
                                      /*exclusive=*/0, /*legacy=*/1);
}

iris_error_t kcnode_mint_excl_badged(struct KCNode *cn, uint32_t slot_idx,
                                     struct KObject *obj,
                                     iris_rights_t rights, uint64_t badge) {
    return kcnode_slot_install_linked(cn, slot_idx, obj, rights, badge, 0, 0,
                                      /*exclusive=*/1, /*legacy=*/1);
}

iris_error_t kcnode_mint_excl(struct KCNode *cn, uint32_t slot_idx,
                              struct KObject *obj, iris_rights_t rights) {
    return kcnode_mint_excl_badged(cn, slot_idx, obj, rights, 0);
}

iris_error_t kcnode_fetch_badged(struct KCNode *cn, uint32_t slot_idx,
                                 struct KObject **out_obj,
                                 iris_rights_t *out_rights,
                                 uint64_t *out_badge) {
    if (!cn || !out_obj || !out_rights) return IRIS_ERR_INVALID_ARG;

    uint64_t flags = irq_spinlock_lock(&cn->lock);

    if (slot_idx >= cn->slot_count) {
        irq_spinlock_unlock(&cn->lock, flags);
        return IRIS_ERR_INVALID_ARG;
    }

    struct KObject *obj = cn->slots[slot_idx].object;
    if (!obj) {
        irq_spinlock_unlock(&cn->lock, flags);
        return IRIS_ERR_NOT_FOUND;
    }

    kobject_retain(obj);
    kobject_active_retain(obj);
    *out_obj    = obj;
    *out_rights = cn->slots[slot_idx].rights;
    if (out_badge)
        *out_badge = cn->slots[slot_idx].badge;

    irq_spinlock_unlock(&cn->lock, flags);
    return IRIS_OK;
}

iris_error_t kcnode_fetch(struct KCNode *cn, uint32_t slot_idx,
                           struct KObject **out_obj, iris_rights_t *out_rights) {
    return kcnode_fetch_badged(cn, slot_idx, out_obj, out_rights, 0);
}

iris_error_t kcnode_swap(struct KCNode *cn, uint32_t slot_a, uint32_t slot_b) {
    if (!cn || slot_a == slot_b) return IRIS_ERR_INVALID_ARG;

    uint64_t mf = irq_spinlock_lock(&mdb_lock);
    if (slot_a >= cn->slot_count || slot_b >= cn->slot_count) {
        irq_spinlock_unlock(&mdb_lock, mf);
        return IRIS_ERR_INVALID_ARG;
    }
    struct KCSlot *a = &cn->slots[slot_a];
    struct KCSlot *b = &cn->slots[slot_b];

    /* Swap via a stack-resident temporary node inside ONE critical section:
     * relocate is total (parent child-ptr, siblings, children parents), so
     * even parent↔child swaps end with all edges preserved.  The temporary
     * never leaves this function and no reader can observe it (readers only
     * see slot content under cn->lock; content moves keep the invariantes). */
    struct KCSlot tmp;
    tmp.object = 0; tmp.rights = RIGHT_NONE; tmp.badge = 0;
    tmp.mdb_parent = 0; tmp.mdb_first_child = 0;
    tmp.mdb_next_sib = 0; tmp.mdb_prev_sib = 0;
    tmp.mdb_cnode = 0; tmp.mdb_flags = 0;

    int a_occ = a->object != 0;
    int b_occ = b->object != 0;
    if (a_occ) mdb_relocate(a, cn, &tmp, cn);
    if (b_occ) mdb_relocate(b, cn, a, cn);
    if (a_occ) mdb_relocate(&tmp, cn, b, cn);

    irq_spinlock_unlock(&mdb_lock, mf);
    return IRIS_OK;
}

iris_error_t kcnode_delete(struct KCNode *cn, uint32_t slot_idx) {
    return kcnode_slot_delete(cn, slot_idx);
}

/* ── MDB validator (tests / diagnostics; closed CNode set) ─────────────── */

static int mdb_slot_in_set(struct KCSlot *s, struct KCNode **set, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        struct KCNode *cn = set[i];
        if (s >= cn->slots && s < cn->slots + cn->slot_count) return 1;
    }
    return 0;
}

uint32_t kcnode_mdb_validate(struct KCNode **set, uint32_t n,
                             struct mdb_validate_report *rep) {
    struct mdb_validate_report r = {0, 0, 0, 0, 0};
    if (!set) { if (rep) *rep = r; return 0; }

    uint64_t mf = irq_spinlock_lock(&mdb_lock);
    for (uint32_t i = 0; i < n; i++) {
        struct KCNode *cn = set[i];
        for (uint32_t j = 0; j < cn->slot_count; j++) {
            struct KCSlot *s = &cn->slots[j];
            if (!s->object) {
                /* B.3-1: empty slot ⇔ empty metadata. */
                if (s->mdb_parent || s->mdb_first_child || s->mdb_next_sib ||
                    s->mdb_prev_sib || s->mdb_cnode || s->mdb_flags)
                    r.errors++;
                continue;
            }
            r.nodes++;
            /* Owning-CNode witness. */
            if (s->mdb_cnode != cn) r.errors++;
            /* No cycles / bounded depth (B.3-3/4); sanity cap 2^20. */
            {
                uint32_t d = 0;
                struct KCSlot *p = s->mdb_parent;
                while (p && d < (1u << 20)) {
                    if (p == s) { r.errors++; break; }
                    p = p->mdb_parent; d++;
                }
                if (d >= (1u << 20)) r.errors++;
                if (d > r.max_depth) r.max_depth = d;
            }
            if (!s->mdb_parent) {
                r.roots++;
                if (s->mdb_flags & MDB_FLAG_LEGACY_ROOT) r.legacy_roots++;
                /* A root belongs to no sibling list. */
                if (s->mdb_next_sib || s->mdb_prev_sib) r.errors++;
            } else {
                struct KCSlot *p = s->mdb_parent;
                if (!p->object) r.errors++;              /* parent must be live */
                if (mdb_slot_in_set(p, set, n)) {
                    /* parent's child list must contain s exactly once */
                    uint32_t seen = 0;
                    for (struct KCSlot *c = p->mdb_first_child; c; c = c->mdb_next_sib)
                        if (c == s) seen++;
                    if (seen != 1u) r.errors++;
                }
                /* sibling list bidirectionality */
                if (s->mdb_next_sib && s->mdb_next_sib->mdb_prev_sib != s) r.errors++;
                if (s->mdb_prev_sib && s->mdb_prev_sib->mdb_next_sib != s) r.errors++;
                if (!s->mdb_prev_sib && p->mdb_first_child != s) r.errors++;
            }
            /* children's parent pointers must point back at s */
            for (struct KCSlot *c = s->mdb_first_child; c; c = c->mdb_next_sib) {
                if (c->mdb_parent != s) r.errors++;
                if (!c->object) r.errors++;              /* no empty slot linked */
            }
        }
    }
    irq_spinlock_unlock(&mdb_lock, mf);

    if (rep) *rep = r;
    return r.errors;
}
