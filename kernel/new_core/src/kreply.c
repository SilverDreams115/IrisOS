#include <iris/nc/kreply.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kschedctx.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kreply_live;

/* Phase 18/S1 — live KReply object count (additive diagnostics). */
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

/* Phase S1: the ONLY KReply storage is untyped-backed — payload returns to the
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

/* ── Stage 8-mcs: scheduling context donation ───────────────────────────── */

void kreply_donate_on_call(struct KReply *r, struct task *sender,
                           struct task *receiver) {
    if (!r || !sender || !receiver) return;
    /* Only a PASSIVE receiver borrows.  A receiver already holding a donation
     * from an unanswered call keeps it, so a second client can never displace
     * the first client's time. */
    if (receiver->sched_ctx || !sender->sched_ctx) return;

    struct KSchedContext *sc = sender->sched_ctx;
    kschedctx_flush_run(sc);        /* close the client's run before lending */
    sender->sched_ctx   = 0;
    receiver->sched_ctx = sc;
    kreply_note_donation(r, sc, receiver);
}

void kreply_note_donation(struct KReply *r, struct KSchedContext *sc,
                          struct task *to) {
    if (!r) return;
    uint64_t flags = irq_spinlock_lock(&r->lock);
    r->donated_sc = sc;
    r->donated_to = to;
    irq_spinlock_unlock(&r->lock, flags);
}

/*
 * Give the lent scheduling context back.
 *
 * One function for every way a binding can end — a normal reply, an endpoint
 * that closed under a blocked caller, a caller that died — because the failure
 * this guards against is the SC being returned on some paths and not others.
 * A donation that leaks leaves the server holding a stranger's budget for
 * ever and the client unable to run at all; a donation returned twice puts one
 * SC on two threads.  Both are silent, so there is exactly one path.
 *
 * `back_to` is passed rather than read from r->caller because the caller has
 * usually been detached by the time this runs.  NULL means the client is gone
 * and the SC has nowhere to go: it is taken off the server anyway, so the
 * server stops running on time that no longer belongs to anybody, and the
 * object is released with the dying thread that owned it.
 */
void kreply_return_donation(struct KReply *r, struct task *back_to) {
    if (!r) return;
    uint64_t flags = irq_spinlock_lock(&r->lock);
    struct KSchedContext *sc = r->donated_sc;
    struct task          *to = r->donated_to;
    r->donated_sc = 0;
    r->donated_to = 0;
    irq_spinlock_unlock(&r->lock, flags);

    if (!sc) return;
    /* Close the server's accounting run before the SC leaves it, so the time
     * it actually used earns its replenishment against the period it was used
     * in rather than the next holder's. */
    kschedctx_flush_run(sc);
    if (to && to->sched_ctx == sc) to->sched_ctx = 0;
    if (back_to && !back_to->sched_ctx) back_to->sched_ctx = sc;
}

void kreply_cancel_caller(struct KReply *r) {
    if (!r) return;
    uint64_t     flags  = irq_spinlock_lock(&r->lock);
    struct task *caller = r->caller;
    r->caller           = 0;
    irq_spinlock_unlock(&r->lock, flags);

    /* Whatever ended the binding, the lent time goes home first. */
    kreply_return_donation(r, caller);

    if (caller) {
        caller->ipc_ep_closed = 1;
        /* caller->pending_kreply is managed by the teardown path; do not touch here. */
        task_wakeup(caller);
    }
}
