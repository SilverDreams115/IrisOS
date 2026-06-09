#include <iris/nc/kreply.h>
#include <iris/nc/kobject.h>
#include <iris/kslab.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kreply_live;

/*
 * kreply_obj_close — fired when active_refs reaches 0 (last handle closed).
 *
 * If the caller is still blocked (r->caller != NULL), the server dropped
 * the reply cap without calling SYS_REPLY.  Wake the caller with an error
 * signal via ipc_ep_closed.  kreply_cancel_caller does the same thing.
 */
static void kreply_obj_close(struct KObject *obj) {
    struct KReply *r    = (struct KReply *)obj;
    uint64_t       flags = irq_spinlock_lock(&r->lock);
    struct task   *caller = r->caller;
    r->caller             = 0;
    irq_spinlock_unlock(&r->lock, flags);

    if (caller) {
        /* sys_ep_call wake-up path reads ipc_ep_closed and returns IRIS_ERR_CLOSED. */
        caller->ipc_ep_closed = 1;
        task_wakeup(caller);
    }
}

static void kreply_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kreply_live, 1u, memory_order_relaxed);
    kslab_free((struct KReply *)obj, (uint32_t)sizeof(struct KReply));
}

static const struct KObjectOps kreply_ops = {
    .close   = kreply_obj_close,
    .destroy = kreply_obj_destroy,
};

struct KReply *kreply_alloc(struct task *caller) {
    struct KReply *r = kslab_alloc((uint32_t)sizeof(struct KReply));
    if (!r) return 0;
    kobject_init(&r->base, KOBJ_REPLY, &kreply_ops);
    irq_spinlock_init(&r->lock);
    r->caller = caller;
    atomic_fetch_add_explicit(&kreply_live, 1u, memory_order_relaxed);
    return r;
}

void kreply_cancel_caller(struct KReply *r) {
    if (!r) return;
    uint64_t     flags  = irq_spinlock_lock(&r->lock);
    struct task *caller = r->caller;
    r->caller           = 0;
    irq_spinlock_unlock(&r->lock, flags);

    if (caller) {
        caller->ipc_ep_closed = 1;
        /* caller->pending_kreply is managed by the teardown path; do not touch here. */
        task_wakeup(caller);
    }
}
