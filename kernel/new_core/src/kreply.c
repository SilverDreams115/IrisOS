/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/nc/kreply.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kschedctx.h>
#include <iris/task.h>
#include <iris/nc/kfault.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kreply_live;

/* Phase 18/S1 — live KReply object count (additive diagnostics). */
uint32_t kreply_live_count(void) {
    return atomic_load_explicit(&kreply_live, memory_order_relaxed);
}

/*
 * Ledger A-22 — a FAULT caller cannot be woken with an error.
 *
 * Every other caller returns from a syscall, so "your server dropped the reply
 * authority" is an error code it reads and handles.  A faulting thread has no
 * syscall to return from: waking it resumes it at the instruction that
 * faulted, which faults again, and again, for as long as nothing kills it.
 *
 * So an unanswerable fault is the same situation as no handler at all, and it
 * gets the same answer — the thread is destroyed, and the counter that says
 * how many faults ended in a kill counts it.  Losing reply authority over a
 * blocked thread is a real failure of the handler, and it is reported as one
 * rather than converted into a livelock nobody can see.
 */
static void kreply_abandon_caller(struct task *caller) {
    if (!caller) return;
    if (caller->ep_fault_call) {
        caller->ep_fault_call = 0u;
        kfault_resolve(caller, /*killed=*/1);
        /* Not when the thread is ALREADY being torn down: teardown itself
         * cancels the reply binding, so killing from here would re-enter the
         * path that called us. */
        if (!caller->terminal) task_kill_external(caller);
        return;
    }
    caller->ipc_ep_closed = 1;
    task_wakeup(caller);
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

    /* sys_ep_call wake-up path reads ipc_ep_closed and returns IRIS_ERR_CLOSED;
     * a fault caller has no such path and is killed instead. */
    kreply_abandon_caller(caller);
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

    /*
     * The loan is ONE REFERENCE, and it lives wherever the pointer does.
     *
     * A donation moves `sched_ctx` from lender to borrower without touching
     * the refcount, which is correct exactly while the pointer and the
     * reference stay together.  They come apart when the BORROWER DIES first:
     * its teardown releases the reference and clears its pointer, and this
     * function then handed the pointer back to the lender anyway — a pointer
     * with no reference behind it.  The lender's own teardown released it a
     * second time, and a scheduling context reached refcount 0 while a CSpace
     * slot still named it.  Reading that slot resurrected a destroyed object.
     *
     * So the borrower's pointer is the proof the loan is still outstanding: no
     * pointer, no loan, nothing to return.
     *
     * And when there IS a loan with nowhere to go — the lender is gone, or has
     * acquired a scheduling context of its own while it waited — the reference
     * is RELEASED rather than dropped on the floor.  It used to leak, and the
     * repair was blocked by the host suite: its fixture modelled a scheduling
     * context whose only holder was the donation, which the kernel cannot
     * construct, so the release destroyed an object whose storage never came
     * from an Untyped.  The fixture models the CSpace slot now, and the rule
     * can be enforced.
     */
    if (!to || to->sched_ctx != sc) return;
    to->sched_ctx = 0;
    if (back_to && !back_to->sched_ctx) back_to->sched_ctx = sc;
    else                                kobject_release(&sc->base);
}

void kreply_cancel_caller(struct KReply *r) {
    if (!r) return;
    uint64_t     flags  = irq_spinlock_lock(&r->lock);
    struct task *caller = r->caller;
    r->caller           = 0;
    irq_spinlock_unlock(&r->lock, flags);

    /* Whatever ended the binding, the lent time goes home first. */
    kreply_return_donation(r, caller);

    /* caller->pending_kreply is managed by the teardown path; not touched here. */
    kreply_abandon_caller(caller);
}
