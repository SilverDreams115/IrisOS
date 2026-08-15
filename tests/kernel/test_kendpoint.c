#include "framework.h"
#include <iris/nc/kendpoint.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
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
    struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
    struct KEndpoint *ep2 = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
    ASSERT_NOT_NULL(ep2);
    struct KEndpoint *ep3 = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
    ASSERT_NOT_NULL(ep3);
    ASSERT_NE(ep2, ep3);
    kendpoint_close(ep2);
    kendpoint_close(ep3);

    /* ── alloc failure path (Fase S1: no kslab — placement on NULL block,
     * i.e. the untyped carve failed upstream) ─────────────────────────── */
    struct KEndpoint *ep_f = kendpoint_alloc_at(NULL);
    ASSERT_NULL(ep_f);

    /* ── close wakes queued receiver ────────────────────────────────── */
    {
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
        struct KEndpoint *e   = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        struct KEndpoint *cap = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(e);
        ASSERT_NOT_NULL(cap);

        /* simulate two-phase staging: fetch retained a ref, and the
         * un-consumed source SLOT rides in ep_cap_src_cn/idx (S4/Etapa 2) */
        kobject_retain(&cap->base);        /* cap refcount: 1 → 2 */

        struct task t = { 0 };
        t.ep_cap_obj     = &cap->base;
        t.ep_cap_rights  = 7u;
        t.ep_cap_src_cn  = (struct KCNode *)(uintptr_t)0x11223344u;
        t.ep_cap_src_idx = 7u;
        eq_enqueue(e, &t, EP_STATE_SEND);

        kobject_active_retain(&e->base);
        kobject_active_release(&e->base);  /* close → kobject_release(cap) → 2→1 */

        ASSERT_NULL(t.ep_cap_obj);
        ASSERT_EQ(t.ep_cap_rights, 0u);
        /* A1.10 / S4: close drops ONLY the staging ref — exactly one release
         * (2→1, no double-release).  The source SLOT is not consumed; its
         * CNode refs are deliberately LEFT SET for the woken sender to drop
         * outside ep->lock (a CNode destructor may not run under it). */
        ASSERT_EQ((uintptr_t)t.ep_cap_src_cn, (uintptr_t)0x11223344u);
        ASSERT_EQ((int)atomic_load(&cap->base.refcount), 1);

        kobject_release(&e->base);
        kobject_release(&cap->base);
    }

    /* ── close wakes multiple queued tasks ───────────────────────────── */
    {
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
        struct KEndpoint *e   = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        struct KEndpoint *cap = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(e);
        ASSERT_NOT_NULL(cap);

        kobject_retain(&cap->base);   /* cap refcount: 1 → 2 */

        struct task t = { 0 };
        t.ep_cap_obj    = &cap->base;
        t.ep_cap_rights = 3u;
        struct KCNode *src_cn = kcnode_alloc(8);   /* S4: real source CNode */
        ASSERT_NOT_NULL(src_cn);
        kobject_retain(&src_cn->base);            /* the refs peek would hold */
        kobject_active_retain(&src_cn->base);
        t.ep_cap_src_cn  = src_cn;
        t.ep_cap_src_idx = 3u;
        eq_enqueue(e, &t, EP_STATE_SEND);

        kendpoint_cancel_waiter(&t);

        ASSERT_NULL(t.ep_cap_obj);
        ASSERT_EQ(t.ep_cap_rights, 0u);
        /* S4: cancel DOES clear it — ep_cancel_wait releases the CNode refs
         * outside the lock, unlike the close-walk above.  The slot itself is
         * never deleted: nothing was delivered, so the sender keeps its cap. */
        ASSERT_NULL(t.ep_cap_src_cn);   /* released, source slot not consumed */
        ASSERT_EQ((int)atomic_load(&src_cn->base.refcount), 1);
        kobject_release(&src_cn->base);
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
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
        struct KEndpoint *e = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
            eps[i] = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
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
