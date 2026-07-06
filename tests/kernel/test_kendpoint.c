#include "framework.h"
#include <iris/nc/kendpoint.h>
#include <iris/nc/kobject.h>
#include <iris/task.h>
#include <iris/paging.h>
#include <stdatomic.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

/* enqueue t at tail of ep's RECV queue */
static void eq_enqueue(struct KEndpoint *ep, struct task *t, int state) {
    t->blocking_ep = ep;
    t->ep_next     = 0;
    if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
    else                { ep->queue_head = t; ep->queue_tail = t; }
    ep->ep_state = state;
}

/* ── test functions ──────────────────────────────────────────────────── */

void test_kendpoint(void) {
    TEST_SUITE("kendpoint");

    /* alloc returns non-null and sets initial state */
    struct KEndpoint *ep = kendpoint_alloc();
    ASSERT_NOT_NULL(ep);
    ASSERT_EQ(ep->ep_state, EP_STATE_IDLE);
    ASSERT_EQ(ep->closed,   0);
    ASSERT_NULL(ep->queue_head);
    ASSERT_NULL(ep->queue_tail);

    /* initial lifecycle refcount is 1 */
    ASSERT_EQ((int)atomic_load(&ep->base.refcount), 1);

    /* kobject_retain increments refcount */
    kobject_retain(&ep->base);
    ASSERT_EQ((int)atomic_load(&ep->base.refcount), 2);

    /* kobject_release decrements; not yet at 0 so no destroy */
    kobject_release(&ep->base);
    ASSERT_EQ((int)atomic_load(&ep->base.refcount), 1);

    /* close on empty endpoint (no waiters): must not crash */
    kendpoint_close(ep);   /* drops last ref → triggers obj_close + destroy */

    /* alloc two more endpoints */
    struct KEndpoint *ep2 = kendpoint_alloc();
    ASSERT_NOT_NULL(ep2);
    struct KEndpoint *ep3 = kendpoint_alloc();
    ASSERT_NOT_NULL(ep3);
    ASSERT_NE(ep2, ep3);
    kendpoint_close(ep2);
    kendpoint_close(ep3);

    /* ── alloc failure injection ────────────────────────────────────── */
    kslab_fail_after(0);
    struct KEndpoint *ep_f = kendpoint_alloc();
    ASSERT_NULL(ep_f);
    kslab_clear_fail();

    /* ── close wakes queued receiver ────────────────────────────────── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t = { 0 };
        eq_enqueue(e, &t, EP_STATE_RECV);

        kobject_active_retain(&e->base);    /* active_refs = 1 */
        kobject_active_release(&e->base);   /* active_refs = 0 → obj_close fires */

        ASSERT_EQ(t.ipc_ep_closed, (uint32_t)1);
        ASSERT_NULL(t.blocking_ep);
        ASSERT_NULL(t.ep_next);
        ASSERT_NULL(e->queue_head);
        ASSERT_NULL(e->queue_tail);
        ASSERT_EQ(e->ep_state, EP_STATE_IDLE);
        ASSERT_EQ(e->closed, 1);

        kobject_release(&e->base);   /* drop alloc ref → destroy */
    }

    /* ── close wakes queued sender ───────────────────────────────────── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t = { 0 };
        eq_enqueue(e, &t, EP_STATE_SEND);

        kobject_active_retain(&e->base);
        kobject_active_release(&e->base);

        ASSERT_EQ(t.ipc_ep_closed, (uint32_t)1);
        ASSERT_NULL(t.blocking_ep);
        ASSERT_NULL(e->queue_head);
        ASSERT_EQ(e->ep_state, EP_STATE_IDLE);

        kobject_release(&e->base);
    }

    /* ── close releases staged cap on queued sender ──────────────────── */
    {
        struct KEndpoint *e   = kendpoint_alloc();
        struct KEndpoint *cap = kendpoint_alloc();
        ASSERT_NOT_NULL(e);
        ASSERT_NOT_NULL(cap);

        /* simulate two-phase staging: get_object retained a ref, and the
         * un-consumed source handle rides in ep_cap_src_h (A1.10) */
        kobject_retain(&cap->base);        /* cap refcount: 1 → 2 */

        struct task t = { 0 };
        t.ep_cap_obj    = &cap->base;
        t.ep_cap_rights = 7u;
        t.ep_cap_src_h  = 0x11223344u;
        eq_enqueue(e, &t, EP_STATE_SEND);

        kobject_active_retain(&e->base);
        kobject_active_release(&e->base);  /* close → kobject_release(cap) → 2→1 */

        ASSERT_NULL(t.ep_cap_obj);
        ASSERT_EQ(t.ep_cap_rights, 0u);
        /* A1.10: close drops ONLY the staging ref — exactly one release
         * (2→1, no double-release) and the source handle is not consumed
         * (src_h cleared, nothing else touched). */
        ASSERT_EQ(t.ep_cap_src_h, 0u);
        ASSERT_EQ((int)atomic_load(&cap->base.refcount), 1);

        kobject_release(&e->base);
        kobject_release(&cap->base);
    }

    /* ── close wakes multiple queued tasks ───────────────────────────── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
        eq_enqueue(e, &t1, EP_STATE_RECV);
        eq_enqueue(e, &t2, EP_STATE_RECV);
        eq_enqueue(e, &t3, EP_STATE_RECV);

        kobject_active_retain(&e->base);
        kobject_active_release(&e->base);

        ASSERT_EQ(t1.ipc_ep_closed, (uint32_t)1);
        ASSERT_EQ(t2.ipc_ep_closed, (uint32_t)1);
        ASSERT_EQ(t3.ipc_ep_closed, (uint32_t)1);
        ASSERT_NULL(e->queue_head);
        ASSERT_NULL(e->queue_tail);

        kobject_release(&e->base);
    }

    /* ── cancel_waiter NULL safety ───────────────────────────────────── */
    kendpoint_cancel_waiter(NULL);   /* must not crash */

    /* ── cancel_waiter removes single entry → empty queue, IDLE state ── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t = { 0 };
        eq_enqueue(e, &t, EP_STATE_RECV);

        kendpoint_cancel_waiter(&t);

        ASSERT_NULL(t.blocking_ep);
        ASSERT_NULL(t.ep_next);
        ASSERT_NULL(e->queue_head);
        ASSERT_NULL(e->queue_tail);
        ASSERT_EQ(e->ep_state, EP_STATE_IDLE);

        kendpoint_close(e);
    }

    /* ── cancel_waiter: releases staged cap, never consumes the source ── */
    {
        struct KEndpoint *e   = kendpoint_alloc();
        struct KEndpoint *cap = kendpoint_alloc();
        ASSERT_NOT_NULL(e);
        ASSERT_NOT_NULL(cap);

        kobject_retain(&cap->base);   /* cap refcount: 1 → 2 */

        struct task t = { 0 };
        t.ep_cap_obj    = &cap->base;
        t.ep_cap_rights = 3u;
        t.ep_cap_src_h  = 0xCAFEu;    /* A1.10: un-consumed source handle */
        eq_enqueue(e, &t, EP_STATE_SEND);

        kendpoint_cancel_waiter(&t);

        ASSERT_NULL(t.ep_cap_obj);
        ASSERT_EQ(t.ep_cap_rights, 0u);
        ASSERT_EQ(t.ep_cap_src_h, 0u);  /* cleared, not consumed */
        ASSERT_EQ((int)atomic_load(&cap->base.refcount), 1); /* cancel released the ref */

        /* A1.10: double cancel is a benign no-op (blocking_ep already NULL):
         * no second release, no queue corruption. */
        kendpoint_cancel_waiter(&t);
        ASSERT_NULL(t.ep_cap_obj);
        ASSERT_EQ((int)atomic_load(&cap->base.refcount), 1);

        kendpoint_close(e);
        kendpoint_close(cap);
    }

    /* ── cancel_waiter: remove HEAD from 3-item queue ───────────────── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
        eq_enqueue(e, &t1, EP_STATE_RECV);
        eq_enqueue(e, &t2, EP_STATE_RECV);
        eq_enqueue(e, &t3, EP_STATE_RECV);
        /* queue: t1 → t2 → t3 */

        kendpoint_cancel_waiter(&t1);

        ASSERT_NULL(t1.blocking_ep);
        ASSERT_NULL(t1.ep_next);
        ASSERT_EQ(e->queue_head, &t2);
        ASSERT_EQ(e->queue_tail, &t3);
        ASSERT_EQ(e->ep_state,   EP_STATE_RECV); /* still items remain */

        /* clean up remaining items before close */
        kendpoint_cancel_waiter(&t2);
        kendpoint_cancel_waiter(&t3);
        kendpoint_close(e);
    }

    /* ── cancel_waiter: remove MIDDLE from 3-item queue ─────────────── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
        eq_enqueue(e, &t1, EP_STATE_RECV);
        eq_enqueue(e, &t2, EP_STATE_RECV);
        eq_enqueue(e, &t3, EP_STATE_RECV);
        /* queue: t1 → t2 → t3 */

        kendpoint_cancel_waiter(&t2);

        ASSERT_NULL(t2.blocking_ep);
        ASSERT_NULL(t2.ep_next);
        ASSERT_EQ(e->queue_head, &t1);
        ASSERT_EQ(e->queue_tail, &t3);
        /* t1 should now link directly to t3 */
        ASSERT_EQ(t1.ep_next, &t3);
        ASSERT_EQ(e->ep_state, EP_STATE_RECV);

        kendpoint_cancel_waiter(&t1);
        kendpoint_cancel_waiter(&t3);
        kendpoint_close(e);
    }

    /* ── cancel_waiter: remove TAIL from 3-item queue ───────────────── */
    {
        struct KEndpoint *e = kendpoint_alloc();
        ASSERT_NOT_NULL(e);

        struct task t1 = { 0 }, t2 = { 0 }, t3 = { 0 };
        eq_enqueue(e, &t1, EP_STATE_RECV);
        eq_enqueue(e, &t2, EP_STATE_RECV);
        eq_enqueue(e, &t3, EP_STATE_RECV);
        /* queue: t1 → t2 → t3 */

        kendpoint_cancel_waiter(&t3);

        ASSERT_NULL(t3.blocking_ep);
        ASSERT_NULL(t3.ep_next);
        ASSERT_EQ(e->queue_head, &t1);
        ASSERT_EQ(e->queue_tail, &t2);
        ASSERT_NULL(t2.ep_next);
        ASSERT_EQ(e->ep_state, EP_STATE_RECV);

        kendpoint_cancel_waiter(&t1);
        kendpoint_cancel_waiter(&t2);
        kendpoint_close(e);
    }

    /* ── alloc_at: initial state matches alloc semantics ─────────────── */
    {
        static struct KEndpoint g_at_mem;   /* static → zero-initialized */
        struct KEndpoint *eat = kendpoint_alloc_at(&g_at_mem);
        ASSERT_NOT_NULL(eat);
        ASSERT_EQ((void *)eat, (void *)&g_at_mem);
        ASSERT_EQ(eat->ep_state, EP_STATE_IDLE);
        ASSERT_EQ(eat->closed,   0);
        ASSERT_NULL(eat->queue_head);
        ASSERT_NULL(eat->queue_tail);
        ASSERT_EQ((int)atomic_load(&eat->base.refcount), 1);
        /* Not releasing — kuntyped_release_child requires a real KUntyped parent.
         * The alloc_at lifecycle path is exercised by test_untyped_cspace. */
    }

    /* ── alloc many and close: no crash = no double-free ─────────────── */
    {
        struct KEndpoint *eps[8];
        for (int i = 0; i < 8; i++) {
            eps[i] = kendpoint_alloc();
            ASSERT_NOT_NULL(eps[i]);
        }
        /* all distinct pointers */
        for (int i = 0; i < 8; i++)
            for (int j = i + 1; j < 8; j++)
                ASSERT_NE(eps[i], eps[j]);
        for (int i = 0; i < 8; i++)
            kendpoint_close(eps[i]);
    }
}
