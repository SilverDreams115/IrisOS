#include <iris/nc/kschedctx.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kschedctx_live;
static _Atomic uint32_t kschedctx_hwm;
static _Atomic uint32_t kschedctx_retyped;
static _Atomic uint32_t kschedctx_destroyed;

/* Fase 17/S2 — live KSchedContext count + diagnostics.  Exposed via the
 * SYS_SCHED_INFO ext2 tier (T123) and SYS_UNTYPED_QUERY kind 4 (Fase S2). */
uint32_t kschedctx_live_count(void) {
    return atomic_load_explicit(&kschedctx_live, memory_order_relaxed);
}

void kschedctx_stats(uint32_t *live, uint32_t *hwm,
                     uint32_t *retyped, uint32_t *destroyed) {
    if (live)      *live      = atomic_load_explicit(&kschedctx_live,      memory_order_relaxed);
    if (hwm)       *hwm       = atomic_load_explicit(&kschedctx_hwm,       memory_order_relaxed);
    if (retyped)   *retyped   = atomic_load_explicit(&kschedctx_retyped,   memory_order_relaxed);
    if (destroyed) *destroyed = atomic_load_explicit(&kschedctx_destroyed, memory_order_relaxed);
}

static void kschedctx_live_inc(void) {
    uint32_t n = atomic_fetch_add_explicit(&kschedctx_live, 1u, memory_order_relaxed) + 1u;
    atomic_fetch_add_explicit(&kschedctx_retyped, 1u, memory_order_relaxed);
    uint32_t hw = atomic_load_explicit(&kschedctx_hwm, memory_order_relaxed);
    while (n > hw &&
           !atomic_compare_exchange_weak_explicit(&kschedctx_hwm, &hw, n,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
}

static void kschedctx_obj_close(struct KObject *obj) {
    (void)obj;
    /* No tasks to wake: the bound task holds a ref; releasing sched_ctx drops
     * the refcount and triggers destroy. */
}

/* Fase S2: la ÚNICA vía de storage es untyped-backed — payload vuelve a la
 * región fuente (kslab nunca interviene). */
static void kschedctx_obj_destroy_ut(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kschedctx_live, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&kschedctx_destroyed, 1u, memory_order_relaxed);
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
    /* Fase S2 (B2): un SC recién retipado nace SIN configurar y sin ligar —
     * budget inválido explícito hasta SC_CONFIGURE. */
    sc->budget_ticks     = 0;
    sc->period_ticks     = 0;
    sc->remaining_budget = 0;
    sc->bound_task       = 0;
    sc->configured       = 0;
    kschedctx_live_inc();
    return sc;
}

void kschedctx_close(struct KSchedContext *sc) {
    if (!sc) return;
    kobject_release(&sc->base);
}

iris_error_t kschedctx_configure(struct KSchedContext *sc,
                                   uint64_t budget, uint64_t period) {
    /* Fase S2 (corrección I1): budget <= period, estilo MCS.  budget == period
     * es una reserva de CPU completa (100%): válida.  El scheduler decrementa
     * remaining por tick y refila en el límite del período; con budget==period
     * el hilo nunca agota su budget dentro de un período, que es exactamente el
     * comportamiento de una reserva total — no rompe ninguna invariancia. */
    if (!sc || budget == 0 || period == 0 || budget > period)
        return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    sc->budget_ticks     = budget;
    sc->period_ticks     = period;
    sc->remaining_budget = budget;
    sc->configured       = 1;
    irq_spinlock_unlock(&sc->lock, flags);
    return IRIS_OK;
}

/*
 * Fase S2 (B4): one-to-one binding.  Fails IRIS_ERR_BUSY if the SC is already
 * bound to a different task; idempotent if bound to the same task.  Atomic
 * under the SC lock (the caller must ensure the task↔SC pointers are set
 * consistently — sys_thread_set_sc does).
 */
iris_error_t kschedctx_bind(struct KSchedContext *sc, struct task *t) {
    if (!sc || !t) return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    if (sc->bound_task && sc->bound_task != t) {
        irq_spinlock_unlock(&sc->lock, flags);
        return IRIS_ERR_BUSY;
    }
    sc->bound_task = t;
    irq_spinlock_unlock(&sc->lock, flags);
    return IRIS_OK;
}

void kschedctx_unbind(struct KSchedContext *sc, struct task *t) {
    if (!sc) return;
    uint64_t flags = irq_spinlock_lock(&sc->lock);
    if (sc->bound_task == t || t == 0)
        sc->bound_task = 0;
    irq_spinlock_unlock(&sc->lock, flags);
}
