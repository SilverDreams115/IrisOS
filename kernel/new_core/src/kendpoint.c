/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/nc/kendpoint.h>
#include <iris/nc/kfault.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kuntyped.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kendpoint_live;

/* Phase 18 — live KEndpoint object count (additive diagnostics). */
uint32_t kendpoint_live_count(void) {
    return atomic_load_explicit(&kendpoint_live, memory_order_relaxed);
}

static void kendpoint_obj_close(struct KObject *obj) {
    struct KEndpoint *ep = (struct KEndpoint *)obj;
    uint64_t flags = irq_spinlock_lock(&ep->lock);
    ep->closed = 1;

    /* Wake all blocked tasks; release any staged caps from blocking senders.
     * A1.10: the staging ref is dropped but the source SLOT is NOT consumed —
     * nothing was delivered, so the sender keeps its cap and wakes with
     * IRIS_ERR_CLOSED.
     *
     * Phase S4 (Step 2): ep_cap_src_cn is deliberately LEFT SET here.  Its
     * refs must be dropped outside this lock — releasing the last ref on a
     * CNode runs a destructor that tears down every slot recursively, and
     * this walk holds ep->lock.  Ownership passes to the woken sender, which
     * aborts its own staging right after task_yield() returns. */
    /*
     * Ledger A-46 — the kills happen AFTER this lock, not under it.
     *
     * A fault caller is killed rather than woken (A-22 below), and
     * `task_kill_external` on a thread that is not on a processor runs the
     * WHOLE teardown right here: it releases the thread's CSpace root, and
     * the last reference on a CNode runs a destructor that empties every slot
     * — taking `mdb_lock`, which is rank 1, while this walk holds `ep->lock`,
     * which is rank 2.  That is the hierarchy inverted, and
     * `scripts/check_lock_order.py` cannot see it because the destructor is
     * reached through `ops->destroy`, a function pointer, which §9.1 says
     * outright that the analysis does not follow.
     *
     * A thread queued on an endpoint is off-CPU by definition, so this is not
     * a corner: it is what the fault-caller branch does every time.  And the
     * work is unbounded (D-12) with interrupts off, on the lock every IPC on
     * this endpoint needs.
     *
     * Stage 2 wrote the rule down — "releasing the last ref on a CNode runs a
     * destructor that tears down every slot, which must not happen under
     * `ep->lock`" — and applied it to the staged capability three lines
     * above while this branch broke it.
     *
     * The deferral costs nothing: these threads are leaving the queue anyway,
     * so `ep_next` is free to chain them, and the reference the queue holds
     * on each (A-44) is what keeps them alive until the kill.
     */
    struct task *kill_head = 0;

    struct task *t = ep->queue_head;
    while (t) {
        struct task *nxt = t->ep_next;
        if (t->ep_cap_obj) {
            kobject_release(t->ep_cap_obj);
            t->ep_cap_obj    = 0;
            t->ep_cap_rights = 0;
            t->ep_cap_badge  = 0;
        }
        t->ep_next       = 0;
        t->blocking_ep   = 0;
        t->ipc_ep_closed = 1;
        /*
         * Ledger A-22: a FAULT caller queued here has no syscall to return
         * CLOSED to — waking it resumes it at the instruction that faulted,
         * which faults again into an endpoint that is now closed.  Its
         * handler is gone, so this is the "no handler" case arriving late,
         * and it gets the same answer.
         */
        if (t->ep_fault_call) {
            t->ep_fault_call = 0u;
            t->ep_call_mode  = 0u;
            t->ep_next       = kill_head;   /* off the queue; onto the list */
            kill_head        = t;           /* keeps A-44's reference */
        } else {
            task_wakeup(t);
            kobject_release(&t->base);      /* A-44: the queue's */
        }
        t = nxt;
    }
    ep->queue_head = 0;
    ep->queue_tail = 0;
    ep->ep_state   = EP_STATE_IDLE;

    irq_spinlock_unlock(&ep->lock, flags);

    /* Outside the lock: each of these can tear a whole address space down. */
    while (kill_head) {
        struct task *k = kill_head;
        kill_head = k->ep_next;
        k->ep_next = 0;
        kfault_resolve(k, /*killed=*/1);
        task_kill_external(k);
        kobject_release(&k->base);          /* A-44: the queue's */
    }
}

/* ── Untyped-backed variant (Ph78; Phase S1: the ONLY variant) ─────
 * The kslab-backed kendpoint_alloc is retired: every KEndpoint payload
 * lives inside the KUntyped region it was retyped from (S2/S14). */

static void kendpoint_obj_destroy_ut(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kendpoint_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, sizeof(struct KEndpoint));
}

static const struct KObjectOps kendpoint_ops_ut = {
    .close   = kendpoint_obj_close,
    .destroy = kendpoint_obj_destroy_ut,
};

struct KEndpoint *kendpoint_alloc_at(void *mem) {
    if (!mem) return 0;
    struct KEndpoint *ep = (struct KEndpoint *)mem;
    kobject_init(&ep->base, KOBJ_ENDPOINT, &kendpoint_ops_ut);
    irq_spinlock_init(&ep->lock);
    /* ep_state, closed, queue_head/tail already zero (kuntyped_bump_alloc zeroes) */
    atomic_fetch_add_explicit(&kendpoint_live, 1u, memory_order_relaxed);
    return ep;
}

void kendpoint_close(struct KEndpoint *ep) {
    if (!ep) return;
    kobject_release(&ep->base);
}

/*
 * kendpoint_cancel_waiter — remove task t from the endpoint queue it is
 * blocked on.  Called from task_cancel_blocked_waits on forcible kill.
 * Releases any staged cap (ep_cap_obj) WITHOUT consuming the source
 * handle (A1.10: nothing was delivered; the handle is torn down with the
 * process, or stays valid if only this thread dies).  Idempotent — a
 * second call finds blocking_ep == NULL and returns.  Does NOT change
 * t->state.
 */
void kendpoint_cancel_waiter(struct task *t) {
    if (!t) return;

    struct KEndpoint *ep = t->blocking_ep;
    if (!ep) return;

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    /* A-44: whether this call is the one that TOOK it off the queue.  The walk
     * below finds nothing when a rendezvous on another core got there first,
     * and only the remover may give the queue's reference back. */
    int removed = 0;

    if (ep->queue_head == t) {
        ep->queue_head = t->ep_next;
        if (!ep->queue_head) {
            ep->queue_tail = 0;
            ep->ep_state   = EP_STATE_IDLE;
        }
        removed = 1;
    } else {
        struct task *prev = ep->queue_head;
        while (prev && prev->ep_next != t)
            prev = prev->ep_next;
        if (prev) {
            prev->ep_next = t->ep_next;
            if (ep->queue_tail == t)
                ep->queue_tail = prev;
            if (!ep->queue_head)
                ep->ep_state = EP_STATE_IDLE;
            removed = 1;
        }
    }

    t->ep_next     = 0;
    t->blocking_ep = 0;

    /* Release staged cap now that the send is being cancelled.  A1.10 / S4:
     * the source SLOT is NOT consumed — no delivery happened; only the CNode
     * refs taken by peek are dropped, outside the lock. */
    struct KObject *staged_cap = t->ep_cap_obj;
    struct KCNode  *staged_cn  = t->ep_cap_src_cn;
    t->ep_cap_obj     = 0;
    t->ep_cap_rights  = 0;
    t->ep_cap_badge   = 0;
    t->ep_cap_src_cn  = 0;
    t->ep_cap_src_idx = 0;
    /* A-22: the fault this thread was delivering dies with the thread — the
     * cancel path only runs on a forcible kill.  Cleared so a recycled TCB
     * cannot inherit a call mode it never made. */
    t->ep_fault_call  = 0u;

    irq_spinlock_unlock(&ep->lock, flags);

    if (staged_cap)
        kobject_release(staged_cap);
    if (staged_cn) {
        kobject_active_release(&staged_cn->base);
        kobject_release(&staged_cn->base);
    }

    /*
     * A-44 — last, and only if this call is what took it off the queue.
     *
     * This runs from the thread's own teardown, so the reference being given
     * back here cannot be the one keeping it alive; the execution reference
     * outlives it.  What matters is that it is given back exactly once: a
     * rendezvous that dequeued it first owns it instead, and releasing here
     * as well would drop a count nobody took.
     */
    if (removed) {
        kobject_release(&t->base);
    }
}
