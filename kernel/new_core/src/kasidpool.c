/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/nc/kasidpool.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>

static _Atomic uint32_t kasidpool_live;

uint32_t kasidpool_live_count(void) {
    return atomic_load_explicit(&kasidpool_live, memory_order_relaxed);
}

/*
 * The identifier space is carved once, forward, and never rewound — the same
 * shape as an Untyped's bump.  A pool that is destroyed does NOT return its
 * range: the identifiers it handed out may still be stamped into CR3 values
 * the hardware has cached, and re-issuing one to a different walk is a stale
 * TLB entry with somebody else's translations in it.  Bounded by the space
 * itself (4093/IRIS_ASID_POOL_SIZE pools), which is a fact about x86 rather
 * than a policy.
 */
static _Atomic uint32_t kasid_next = KASID_FIRST;

iris_error_t kasidpool_carve_range(uint16_t *out_first, uint16_t *out_count) {
    if (!out_first || !out_count) return IRIS_ERR_INVALID_ARG;
    uint32_t first = atomic_fetch_add_explicit(&kasid_next, KASID_POOL_SIZE,
                                               memory_order_relaxed);
    if (first + KASID_POOL_SIZE - 1u > KASID_LAST) {
        atomic_fetch_sub_explicit(&kasid_next, KASID_POOL_SIZE,
                                  memory_order_relaxed);
        return IRIS_ERR_NO_MEMORY;
    }
    *out_first = (uint16_t)first;
    *out_count = (uint16_t)KASID_POOL_SIZE;
    return IRIS_OK;
}

static void kasidpool_obj_close(struct KObject *obj) { (void)obj; }

static void kasidpool_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kasidpool_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, sizeof(struct KAsidPool));
}

static const struct KObjectOps kasidpool_ops = {
    .close   = kasidpool_obj_close,
    .destroy = kasidpool_obj_destroy,
};

struct KAsidPool *kasidpool_alloc_at(void *mem, uint16_t first, uint16_t count) {
    if (!mem || !count) return 0;
    struct KAsidPool *p = (struct KAsidPool *)mem;
    p->first = first;
    p->count = count;
    for (uint32_t i = 0; i < KASID_POOL_WORDS; i++) p->used[i] = 0;
    kobject_init(&p->base, KOBJ_ASID_POOL, &kasidpool_ops);
    spinlock_init(&p->lock);
    atomic_fetch_add_explicit(&kasidpool_live, 1u, memory_order_relaxed);
    return p;
}

uint16_t kasidpool_take(struct KAsidPool *p) {
    if (!p) return 0;
    uint16_t id = 0;
    spinlock_lock(&p->lock);
    for (uint32_t i = 0; i < p->count && !id; i++) {
        if (p->used[i / 64u] & (1ULL << (i % 64u))) continue;
        p->used[i / 64u] |= (1ULL << (i % 64u));
        id = (uint16_t)(p->first + i);
    }
    spinlock_unlock(&p->lock);
    return id;
}

void kasidpool_put(struct KAsidPool *p, uint16_t id) {
    if (!p || !id) return;
    if (id < p->first || id >= p->first + p->count) return;
    uint32_t i = (uint32_t)(id - p->first);
    spinlock_lock(&p->lock);
    p->used[i / 64u] &= ~(1ULL << (i % 64u));
    spinlock_unlock(&p->lock);
}
