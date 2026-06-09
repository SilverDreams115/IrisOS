#include <iris/nc/ktcb.h>
#include <iris/nc/kobject.h>
#include <iris/kslab.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t ktcb_live;

static void ktcb_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&ktcb_live, 1u, memory_order_relaxed);
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
    atomic_fetch_add_explicit(&ktcb_live, 1u, memory_order_relaxed);
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
