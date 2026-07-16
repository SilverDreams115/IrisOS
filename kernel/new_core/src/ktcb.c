#include <iris/nc/ktcb.h>
#include <iris/nc/kobject.h>
#include <iris/kslab.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t ktcb_live;
static _Atomic uint32_t ktcb_hwm;
static _Atomic uint32_t ktcb_retyped;
static _Atomic uint32_t ktcb_destroyed;

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

static void ktcb_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&ktcb_live, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&ktcb_destroyed, 1u, memory_order_relaxed);
    kslab_free((struct KTcb *)obj, (uint32_t)sizeof(struct KTcb));
}

static const struct KObjectOps ktcb_ops = { .destroy = ktcb_destroy };

struct KTcb *ktcb_alloc(struct task *t) {
    if (!t) return 0;
    struct KTcb *tcb = kslab_alloc((uint32_t)sizeof(struct KTcb));
    if (!tcb) return 0;
    kobject_init(&tcb->base, KOBJ_TCB, &ktcb_ops);
    irq_spinlock_init(&tcb->lock);
    tcb->task    = t;
    tcb->task_id = t->id;
    ktcb_live_inc();
    return tcb;
}

void ktcb_nullify(struct KTcb *tcb) {
    if (!tcb) return;
    uint64_t flags = irq_spinlock_lock(&tcb->lock);
    tcb->task = 0;
    irq_spinlock_unlock(&tcb->lock, flags);
}

void ktcb_free(struct KTcb *tcb) {
    if (!tcb) return;
    kobject_release(&tcb->base);
}
