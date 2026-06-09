#include <iris/nc/kcnode.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/kslab.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kcnode_live;

static void kcnode_obj_close(struct KObject *obj) {
    struct KCNode *cn = (struct KCNode *)obj;
    uint64_t flags = irq_spinlock_lock(&cn->lock);
    for (uint32_t i = 0u; i < cn->slot_count; i++) {
        if (cn->slots[i].object) {
            struct KObject *old = cn->slots[i].object;
            cn->slots[i].object = 0;
            cn->slots[i].rights = RIGHT_NONE;
            irq_spinlock_unlock(&cn->lock, flags);
            kobject_active_release(old);
            kobject_release(old);
            flags = irq_spinlock_lock(&cn->lock);
        }
    }
    irq_spinlock_unlock(&cn->lock, flags);
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
    /* slots[] already zeroed by kuntyped_alloc_child */
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
    /* slots[] zeroed by kslab_alloc */
    atomic_fetch_add_explicit(&kcnode_live, 1u, memory_order_relaxed);
    return cn;
}

void kcnode_close(struct KCNode *cn) {
    if (!cn) return;
    kobject_release(&cn->base);
}

iris_error_t kcnode_mint(struct KCNode *cn, uint32_t slot_idx,
                          struct KObject *obj, iris_rights_t rights) {
    if (!cn || !obj || rights == RIGHT_NONE) return IRIS_ERR_INVALID_ARG;

    kobject_retain(obj);
    kobject_active_retain(obj);

    uint64_t flags = irq_spinlock_lock(&cn->lock);

    if (slot_idx >= cn->slot_count) {
        irq_spinlock_unlock(&cn->lock, flags);
        kobject_active_release(obj);
        kobject_release(obj);
        return IRIS_ERR_INVALID_ARG;
    }

    struct KObject *old        = cn->slots[slot_idx].object;
    cn->slots[slot_idx].object = obj;
    cn->slots[slot_idx].rights = rights;

    irq_spinlock_unlock(&cn->lock, flags);

    if (old) {
        kobject_active_release(old);
        kobject_release(old);
    }
    return IRIS_OK;
}

iris_error_t kcnode_fetch(struct KCNode *cn, uint32_t slot_idx,
                           struct KObject **out_obj, iris_rights_t *out_rights) {
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

    irq_spinlock_unlock(&cn->lock, flags);
    return IRIS_OK;
}

iris_error_t kcnode_swap(struct KCNode *cn, uint32_t slot_a, uint32_t slot_b) {
    if (!cn || slot_a == slot_b) return IRIS_ERR_INVALID_ARG;

    uint64_t flags = irq_spinlock_lock(&cn->lock);

    if (slot_a >= cn->slot_count || slot_b >= cn->slot_count) {
        irq_spinlock_unlock(&cn->lock, flags);
        return IRIS_ERR_INVALID_ARG;
    }

    struct KCSlot tmp    = cn->slots[slot_a];
    cn->slots[slot_a]    = cn->slots[slot_b];
    cn->slots[slot_b]    = tmp;

    irq_spinlock_unlock(&cn->lock, flags);
    return IRIS_OK;
}

iris_error_t kcnode_delete(struct KCNode *cn, uint32_t slot_idx) {
    if (!cn) return IRIS_ERR_INVALID_ARG;

    uint64_t flags = irq_spinlock_lock(&cn->lock);

    if (slot_idx >= cn->slot_count) {
        irq_spinlock_unlock(&cn->lock, flags);
        return IRIS_ERR_INVALID_ARG;
    }

    struct KObject *old = cn->slots[slot_idx].object;
    cn->slots[slot_idx].object = 0;
    cn->slots[slot_idx].rights = RIGHT_NONE;
    irq_spinlock_unlock(&cn->lock, flags);

    if (old) {
        kobject_active_release(old);
        kobject_release(old);
    }
    return IRIS_OK;
}
