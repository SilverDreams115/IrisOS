#include <iris/nc/ktcb.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * Fase S2 D2 — KTCB object identity ops.
 *
 * The KTCB IS `struct task` (KObject at offset 0).  These ops manage the
 * OBJECT lifetime only; execution teardown (queues, resources, registry) is
 * done by task_lifecycle.c before the scheduler drops its execution reference.
 * The destructor runs when the last reference (execution + all caps) drops.
 */

static _Atomic uint32_t ktcb_live;       /* objects that exist (any lifetime) */
static _Atomic uint32_t ktcb_hwm;
static _Atomic uint32_t ktcb_retyped;    /* objects created */
static _Atomic uint32_t ktcb_destroyed;  /* final destructors run */

uint32_t ktcb_live_count(void) {
    return atomic_load_explicit(&ktcb_live, memory_order_relaxed);
}

void ktcb_stats(uint32_t *live, uint32_t *hwm,
                uint32_t *retyped, uint32_t *destroyed) {
    if (live)      *live      = atomic_load_explicit(&ktcb_live,      memory_order_relaxed);
    if (hwm)       *hwm       = atomic_load_explicit(&ktcb_hwm,       memory_order_relaxed);
    if (retyped)   *retyped   = atomic_load_explicit(&ktcb_retyped,   memory_order_relaxed);
    if (destroyed) *destroyed = atomic_load_explicit(&ktcb_destroyed, memory_order_relaxed);
}

static void ktcb_live_inc(void) {
    uint32_t n = atomic_fetch_add_explicit(&ktcb_live, 1u, memory_order_relaxed) + 1u;
    atomic_fetch_add_explicit(&ktcb_retyped, 1u, memory_order_relaxed);
    uint32_t hw = atomic_load_explicit(&ktcb_hwm, memory_order_relaxed);
    while (n > hw &&
           !atomic_compare_exchange_weak_explicit(&ktcb_hwm, &hw, n,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
}

/* Final destructor — implemented in task_lifecycle.c (it owns the backing
 * pool + registry + resource-freeing).  Frees the backing slot for reuse. */
void task_backing_free_on_destroy(struct task *t);

static void ktcb_obj_destroy(struct KObject *obj) {
    struct task *t = (struct task *)obj;   /* KObject at offset 0 */
    atomic_fetch_sub_explicit(&ktcb_live, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&ktcb_destroyed, 1u, memory_order_relaxed);
    task_backing_free_on_destroy(t);
}

static const struct KObjectOps ktcb_ops = { .destroy = ktcb_obj_destroy };

void ktcb_object_init(struct task *t) {
    if (!t) return;
    kobject_init(&t->base, KOBJ_TCB, &ktcb_ops);  /* refcount = 1 (execution ref) */
    irq_spinlock_init(&t->obj_lock);
    t->configured = 1;   /* pool birth: execution state built by the creator */
    ktcb_live_inc();
}

/* ── Fase S2 Etapa 0 — Untyped-born (canonical) KTCB ───────────────────── */

/* Destructor for the untyped-born TCB: the storage IS the retyped region —
 * return the zeroed block to its parent untyped (child_count--, parent ref
 * released).  Nothing else to unwind: an unconfigured TCB never held a
 * kstack, a registry slot, a process link or a sched_ctx (SC_BIND refuses
 * unconfigured targets precisely so no reference can leak here). */
static void ktcb_obj_destroy_ut(struct KObject *obj) {
    struct task *t = (struct task *)obj;   /* KObject at offset 0 */
    atomic_fetch_sub_explicit(&ktcb_live, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&ktcb_destroyed, 1u, memory_order_relaxed);
    kuntyped_release_child(t, sizeof(struct task));
}

static const struct KObjectOps ktcb_ops_ut = { .destroy = ktcb_obj_destroy_ut };

struct task *ktcb_alloc_at(void *mem) {
    struct task *t = (struct task *)mem;
    if (!t) return 0;
    /* Block arrives zero-filled from kuntyped_alloc_children_atomic; set only
     * the non-zero identity fields.  NOTE: TASK_READY == 0, so the state MUST
     * be set explicitly — an inactive TCB is never runnable. */
    kobject_init(&t->base, KOBJ_TCB, &ktcb_ops_ut);  /* refcount = 1 (creator) */
    irq_spinlock_init(&t->obj_lock);
    t->state      = TASK_SUSPENDED;         /* inactive until TCB_CONFIGURE */
    t->ring       = TASK_RING3;
    t->priority   = TASK_PRIORITY_DEFAULT;
    t->reg_slot   = -1;                     /* no scheduler identity */
    t->configured = 0;                      /* execution gate: stays closed */
    ktcb_live_inc();
    return t;
}
