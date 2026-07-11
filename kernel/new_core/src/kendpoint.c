#include <iris/nc/kendpoint.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kuntyped.h>
#include <iris/kslab.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kendpoint_live;

/* Fase 18 — live KEndpoint object count (additive diagnostics). */
uint32_t kendpoint_live_count(void) {
    return atomic_load_explicit(&kendpoint_live, memory_order_relaxed);
}

static void kendpoint_obj_close(struct KObject *obj) {
    struct KEndpoint *ep = (struct KEndpoint *)obj;
    uint64_t flags = irq_spinlock_lock(&ep->lock);
    ep->closed = 1;

    /* Wake all blocked tasks; release any staged caps from blocking senders.
     * A1.10: the staging ref is dropped but the source handle is NOT
     * consumed (ep_cap_src_h just clears) — nothing was delivered, so the
     * sender keeps its cap and wakes with IRIS_ERR_CLOSED. */
    struct task *t = ep->queue_head;
    while (t) {
        struct task *nxt = t->ep_next;
        if (t->ep_cap_obj) {
            kobject_release(t->ep_cap_obj);
            t->ep_cap_obj    = 0;
            t->ep_cap_rights = 0;
            t->ep_cap_badge  = 0;
        }
        t->ep_cap_src_h  = 0;
        t->ep_next       = 0;
        t->blocking_ep   = 0;
        t->ipc_ep_closed = 1;
        task_wakeup(t);
        t = nxt;
    }
    ep->queue_head = 0;
    ep->queue_tail = 0;
    ep->ep_state   = EP_STATE_IDLE;

    irq_spinlock_unlock(&ep->lock, flags);
}

static void kendpoint_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kendpoint_live, 1u, memory_order_relaxed);
    kslab_free((struct KEndpoint *)obj, (uint32_t)sizeof(struct KEndpoint));
}

static const struct KObjectOps kendpoint_ops = {
    .close   = kendpoint_obj_close,
    .destroy = kendpoint_obj_destroy,
};

struct KEndpoint *kendpoint_alloc(void) {
    struct KEndpoint *ep = kslab_alloc((uint32_t)sizeof(struct KEndpoint));
    if (!ep) return 0;
    kobject_init(&ep->base, KOBJ_ENDPOINT, &kendpoint_ops);
    irq_spinlock_init(&ep->lock);
    ep->ep_state   = EP_STATE_IDLE;
    ep->closed     = 0;
    ep->queue_head = 0;
    ep->queue_tail = 0;
    atomic_fetch_add_explicit(&kendpoint_live, 1u, memory_order_relaxed);
    return ep;
}

/* ── Untyped-backed variant (Ph78) ──────────────────────────────── */

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

    if (ep->queue_head == t) {
        ep->queue_head = t->ep_next;
        if (!ep->queue_head) {
            ep->queue_tail = 0;
            ep->ep_state   = EP_STATE_IDLE;
        }
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
        }
    }

    t->ep_next     = 0;
    t->blocking_ep = 0;

    /* Release staged cap now that the send is being cancelled.  A1.10:
     * source handle NOT consumed — no delivery happened. */
    struct KObject *staged_cap = t->ep_cap_obj;
    t->ep_cap_obj    = 0;
    t->ep_cap_rights = 0;
    t->ep_cap_badge  = 0;
    t->ep_cap_src_h  = 0;

    irq_spinlock_unlock(&ep->lock, flags);

    if (staged_cap)
        kobject_release(staged_cap);
}
