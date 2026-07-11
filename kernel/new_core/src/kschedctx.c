#include <iris/nc/kschedctx.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/kslab.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kschedctx_live;

/* Fase 17 — live KSchedContext object count (additive instrumentation).
 * Exposed via the SYS_SCHED_INFO ext2 tier so the T123 selftest can prove a
 * scheduling context is destroyed (not leaked, not double-freed) after its
 * bound thread/process dies and is reaped — invariants S8/S9. */
uint32_t kschedctx_live_count(void) {
    return atomic_load_explicit(&kschedctx_live, memory_order_relaxed);
}

static void kschedctx_obj_close(struct KObject *obj) {
    (void)obj;
    /* No tasks to wake: the bound task holds the ref; when the task releases
     * its sched_ctx pointer, refcount drops and destroy is triggered. */
}

static void kschedctx_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kschedctx_live, 1u, memory_order_relaxed);
    kslab_free((struct KSchedContext *)obj, (uint32_t)sizeof(struct KSchedContext));
}

static const struct KObjectOps kschedctx_ops = {
    .close   = kschedctx_obj_close,
    .destroy = kschedctx_obj_destroy,
};

/* ── Untyped-backed variant (Ph79) ──────────────────────────────── */

static void kschedctx_obj_destroy_ut(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kschedctx_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, sizeof(struct KSchedContext));
}

static const struct KObjectOps kschedctx_ops_ut = {
    .close   = kschedctx_obj_close,
    .destroy = kschedctx_obj_destroy_ut,
};

struct KSchedContext *kschedctx_alloc_at(void *mem) {
    if (!mem) return 0;
    struct KSchedContext *sc = (struct KSchedContext *)mem;
    kobject_init(&sc->base, KOBJ_SCHED_CONTEXT, &kschedctx_ops_ut);
    irq_spinlock_init(&sc->lock);
    sc->budget_ticks     = KSCHEDCTX_DEFAULT_BUDGET;
    sc->period_ticks     = KSCHEDCTX_DEFAULT_PERIOD;
    sc->remaining_budget = KSCHEDCTX_DEFAULT_BUDGET;
    atomic_fetch_add_explicit(&kschedctx_live, 1u, memory_order_relaxed);
    return sc;
}

struct KSchedContext *kschedctx_alloc(void) {
    struct KSchedContext *sc = kslab_alloc((uint32_t)sizeof(struct KSchedContext));
    if (!sc) return 0;
    kobject_init(&sc->base, KOBJ_SCHED_CONTEXT, &kschedctx_ops);
    irq_spinlock_init(&sc->lock);
    sc->budget_ticks     = KSCHEDCTX_DEFAULT_BUDGET;
    sc->period_ticks     = KSCHEDCTX_DEFAULT_PERIOD;
    sc->remaining_budget = KSCHEDCTX_DEFAULT_BUDGET;
    atomic_fetch_add_explicit(&kschedctx_live, 1u, memory_order_relaxed);
    return sc;
}

void kschedctx_close(struct KSchedContext *sc) {
    if (!sc) return;
    kobject_release(&sc->base);
}

iris_error_t kschedctx_configure(struct KSchedContext *sc,
                                   uint64_t budget, uint64_t period) {
    if (!sc || budget == 0 || period == 0 || budget >= period)
        return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    sc->budget_ticks     = budget;
    sc->period_ticks     = period;
    sc->remaining_budget = budget;
    irq_spinlock_unlock(&sc->lock, flags);
    return IRIS_OK;
}
