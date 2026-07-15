#include <iris/nc/kreply.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kreply_live;

/* Fase 18/S1 — live KReply object count (additive diagnostics). */
uint32_t kreply_live_count(void) {
    return atomic_load_explicit(&kreply_live, memory_order_relaxed);
}

/*
 * kreply_obj_close — fired when active_refs reaches 0 (last capability
 * dropped: CNode slot deleted / handle closed / owning process torn down).
 *
 * If a caller is still bound (r->caller != NULL), the server lost its reply
 * authority without invoking SYS_REPLY.  Wake the caller with an error
 * signal via ipc_ep_closed.  kreply_cancel_caller does the same thing.
 */
static void kreply_obj_close(struct KObject *obj) {
    struct KReply *r    = (struct KReply *)obj;
    uint64_t       flags = irq_spinlock_lock(&r->lock);
    struct task   *caller = r->caller;
    r->caller             = 0;
    r->staged             = 0;
    irq_spinlock_unlock(&r->lock, flags);

    if (caller) {
        /* sys_ep_call wake-up path reads ipc_ep_closed and returns IRIS_ERR_CLOSED. */
        caller->ipc_ep_closed = 1;
        task_wakeup(caller);
    }
}

/* Fase S1: the ONLY KReply storage is untyped-backed — payload returns to the
 * source KUntyped region (kslab is never involved). */
static void kreply_obj_destroy_ut(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kreply_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, sizeof(struct KReply));
}

static const struct KObjectOps kreply_ops_ut = {
    .close   = kreply_obj_close,
    .destroy = kreply_obj_destroy_ut,
};

struct KReply *kreply_alloc_at(void *mem) {
    if (!mem) return 0;
    struct KReply *r = (struct KReply *)mem;
    kobject_init(&r->base, KOBJ_REPLY, &kreply_ops_ut);
    irq_spinlock_init(&r->lock);
    /* caller/staged already zero (kuntyped_alloc_children_atomic zeroes) */
    atomic_fetch_add_explicit(&kreply_live, 1u, memory_order_relaxed);
    return r;
}

iris_error_t kreply_stage(struct KReply *r) {
    if (!r) return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&r->lock);
    if (r->staged || r->caller) {
        irq_spinlock_unlock(&r->lock, flags);
        return IRIS_ERR_BUSY;
    }
    r->staged = 1;
    irq_spinlock_unlock(&r->lock, flags);
    return IRIS_OK;
}

void kreply_unstage(struct KReply *r) {
    if (!r) return;
    uint64_t flags = irq_spinlock_lock(&r->lock);
    if (r->staged && !r->caller)
        r->staged = 0;
    irq_spinlock_unlock(&r->lock, flags);
}

iris_error_t kreply_bind_caller(struct KReply *r, struct task *caller) {
    if (!r || !caller) return IRIS_ERR_INVALID_ARG;
    uint64_t flags = irq_spinlock_lock(&r->lock);
    if (!r->staged || r->caller) {
        irq_spinlock_unlock(&r->lock, flags);
        return IRIS_ERR_BUSY;
    }
    r->staged = 0;
    r->caller = caller;
    irq_spinlock_unlock(&r->lock, flags);
    return IRIS_OK;
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
