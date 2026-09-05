/*
 * test_schedctx_refill.c — sporadic replenishment (Stage 8-mcs).
 *
 * The model this replaces refilled `remaining_budget` in exactly one place:
 * the exhaustion branch.  A thread that blocked before exhausting carried its
 * remainder forward for ever, so a server handling a request in 2 of its 5
 * ticks and then waiting on its endpoint kept 3, then 1, then stalled for a
 * whole period.  Its real bandwidth fell the more often it blocked — the
 * opposite of what a budget is for.  R-2 is that defect, asserted directly.
 *
 * The invariant every case below re-checks is the one that turns a budget into
 * a guarantee:
 *
 *     remaining_budget + consumed_run + sum(pending refills) == budget_ticks
 *
 * If it ever fails high, a thread got time nobody granted; if it fails low,
 * time was lost and the thread is being starved.  Both are silent in a running
 * system, which is why they are asserted here rather than left to observation.
 */
#include "framework.h"
#include <iris/nc/kschedctx.h>
#include <iris/nc/kreply.h>
#include <iris/task.h>
#include <iris/kpage.h>
#include <string.h>

static struct KSchedContext *rf_sc(uint64_t budget, uint64_t period) {
    void *mem = kpage_alloc((uint32_t)kschedctx_bytes(0));
    if (!mem) return NULL;
    memset(mem, 0, (size_t)kschedctx_bytes(0));
    struct KSchedContext *sc = kschedctx_alloc_at(mem, 0);
    if (!sc) return NULL;
    if (kschedctx_configure(sc, budget, period) != IRIS_OK) return NULL;
    return sc;
}

/* The conservation law, checked after every operation. */
static uint64_t rf_total(const struct KSchedContext *sc) {
    uint64_t sum = sc->remaining_budget + sc->consumed_run;
    for (uint32_t i = 0; i < sc->refill_count; i++)
        sum += sc->refills[(sc->refill_head + i) % sc->refill_max].amount;
    return sum;
}

void test_schedctx_refill(void) {
    TEST_SUITE("sporadic replenishment (Stage 8-mcs)");

    /* ── R-1: a fresh SC has the whole budget and owes nothing ─────────── */
    {
        struct KSchedContext *sc = rf_sc(5, 20);
        ASSERT_NOT_NULL(sc);
        ASSERT_EQ(sc->remaining_budget, 5u);
        ASSERT_EQ(sc->refill_count, 0u);
        ASSERT_EQ(rf_total(sc), 5u);
    }

    /* ── R-2: THE DEFECT.  A partial run is given back one period after it
     * started — not carried forward until the thread happens to exhaust ──
     * A server that uses 2 of 5 ticks at t=10 must have 5 again by t=30, and
     * must NOT have them at t=29. */
    {
        struct KSchedContext *sc = rf_sc(5, 20);
        ASSERT_NOT_NULL(sc);

        ASSERT_EQ(kschedctx_charge_tick(sc, 10), 0);   /* 5 -> 4 */
        ASSERT_EQ(kschedctx_charge_tick(sc, 11), 0);   /* 4 -> 3 */
        ASSERT_EQ(sc->remaining_budget, 3u);
        ASSERT_EQ(rf_total(sc), 5u);                   /* nothing lost */

        kschedctx_flush_run(sc);                       /* thread blocks */
        ASSERT_EQ(sc->refill_count, 1u);
        ASSERT_EQ(rf_total(sc), 5u);

        /* Not yet: the replenishment is due one period after consumption
         * BEGAN, which is t=10+20. */
        ASSERT_EQ(kschedctx_apply_refills(sc, 29), 0);
        ASSERT_EQ(sc->remaining_budget, 3u);

        /* Now. */
        ASSERT_EQ(kschedctx_apply_refills(sc, 30), 1);
        ASSERT_EQ(sc->remaining_budget, 5u);
        ASSERT_EQ(sc->refill_count, 0u);
        ASSERT_EQ(rf_total(sc), 5u);
    }

    /* ── R-3: exhaustion is reported, and returns exactly what was spent ── */
    {
        struct KSchedContext *sc = rf_sc(3, 10);
        ASSERT_NOT_NULL(sc);
        ASSERT_EQ(kschedctx_charge_tick(sc, 100), 0);
        ASSERT_EQ(kschedctx_charge_tick(sc, 101), 0);
        ASSERT_EQ(kschedctx_charge_tick(sc, 102), 1);  /* exhausted */
        ASSERT_EQ(sc->remaining_budget, 0u);

        kschedctx_flush_run(sc);
        ASSERT_EQ(rf_total(sc), 3u);
        ASSERT_EQ(kschedctx_apply_refills(sc, 109), 0);
        ASSERT_EQ(kschedctx_apply_refills(sc, 110), 1); /* 100 + period */
        ASSERT_EQ(sc->remaining_budget, 3u);
    }

    /* ── R-4: THE GUARANTEE.  No more than `budget` in any window of
     * `period` ───────────────────────────────────────────────────────────
     * Spend the whole budget, then keep asking for time across the period.
     * Until the replenishment falls due there is none, no matter how often
     * the thread is scheduled. */
    {
        struct KSchedContext *sc = rf_sc(4, 20);
        ASSERT_NOT_NULL(sc);
        for (int i = 0; i < 4; i++) (void)kschedctx_charge_tick(sc, (uint64_t)(50 + i));
        ASSERT_EQ(sc->remaining_budget, 0u);
        kschedctx_flush_run(sc);

        uint64_t granted = 0;
        for (uint64_t now = 54; now < 70; now++) {
            (void)kschedctx_apply_refills(sc, now);
            granted += sc->remaining_budget;
            /* charging with an empty budget must consume nothing */
            (void)kschedctx_charge_tick(sc, now);
        }
        ASSERT_EQ(granted, 0u);                 /* nothing inside the period */
        ASSERT_EQ(rf_total(sc), 4u);            /* and nothing lost */

        ASSERT_EQ(kschedctx_apply_refills(sc, 70), 1);
        ASSERT_EQ(sc->remaining_budget, 4u);
    }

    /* ── R-5: charging an empty budget consumes nothing and owes nothing ── */
    {
        struct KSchedContext *sc = rf_sc(1, 10);
        ASSERT_NOT_NULL(sc);
        ASSERT_EQ(kschedctx_charge_tick(sc, 5), 1);
        ASSERT_EQ(kschedctx_charge_tick(sc, 6), 1);   /* still exhausted */
        ASSERT_EQ(sc->consumed_run, 1u);              /* not 2 */
        kschedctx_flush_run(sc);
        ASSERT_EQ(rf_total(sc), 1u);
    }

    /* ── R-6: a full queue merges CONSERVATIVELY — later, never earlier ───
     * A thread that blocks and resumes more often than the queue is deep must
     * lose nothing, and must not be handed time early.  Merging takes the
     * LATER due time, so the guarantee holds in the safe direction. */
    {
        struct KSchedContext *sc = rf_sc(32, 100);
        ASSERT_NOT_NULL(sc);
        /* 10 separate one-tick runs into a queue of 8 */
        for (int i = 0; i < 10; i++) {
            (void)kschedctx_charge_tick(sc, (uint64_t)(200 + i * 2));
            kschedctx_flush_run(sc);
        }
        ASSERT_EQ(sc->refill_count, sc->refill_max);
        ASSERT_EQ(rf_total(sc), 32u);           /* nothing lost to the merge */

        /* The last run began at t=218, so nothing merged into that entry may
         * come due before 318. */
        (void)kschedctx_apply_refills(sc, 317);
        ASSERT_EQ(sc->remaining_budget < 32u, 1);
        (void)kschedctx_apply_refills(sc, 318);
        ASSERT_EQ(sc->remaining_budget, 32u);
        ASSERT_EQ(sc->refill_count, 0u);
    }

    /* ── R-7: reconfiguring discards the old schedule ─────────────────────
     * Pending entries are denominated in the OLD period; keeping them would
     * hand back time on a cadence nobody asked for. */
    {
        struct KSchedContext *sc = rf_sc(5, 20);
        ASSERT_NOT_NULL(sc);
        (void)kschedctx_charge_tick(sc, 10);
        kschedctx_flush_run(sc);
        ASSERT_EQ(sc->refill_count, 1u);

        ASSERT_EQ(kschedctx_configure(sc, 7, 40), IRIS_OK);
        ASSERT_EQ(sc->refill_count, 0u);
        ASSERT_EQ(sc->remaining_budget, 7u);
        ASSERT_EQ(rf_total(sc), 7u);
    }

    /* ── D-1: donation moves the SC to a passive server and back ─────────
     * seL4's model: a server with no scheduling context of its own runs on the
     * requester's time.  The accounting has to be exactly symmetric — a
     * donation that leaks leaves the server holding a stranger's budget for
     * ever and the client unable to run at all; one returned twice puts one SC
     * on two threads.  Both are silent in a running system. */
    {
        struct KSchedContext *sc = rf_sc(5, 20);
        struct KReply reply;
        struct task client, server;
        ASSERT_NOT_NULL(sc);
        memset(&reply, 0, sizeof(reply));
        memset(&client, 0, sizeof(client));
        memset(&server, 0, sizeof(server));

        client.sched_ctx = sc;
        server.sched_ctx = NULL;              /* passive */

        /* rendezvous: the loan */
        server.sched_ctx = sc;
        client.sched_ctx = NULL;
        kreply_note_donation(&reply, sc, &server);
        ASSERT_EQ(reply.donated_sc, sc);
        ASSERT_EQ(reply.donated_to, &server);

        /* reply: it goes home */
        kreply_return_donation(&reply, &client);
        ASSERT_EQ(client.sched_ctx, sc);
        ASSERT_NULL(server.sched_ctx);
        ASSERT_NULL(reply.donated_sc);

        /* idempotent — a second return must not move anything */
        kreply_return_donation(&reply, &client);
        ASSERT_EQ(client.sched_ctx, sc);
        ASSERT_NULL(server.sched_ctx);
    }

    /* ── D-2: a dead client takes the loan off the server anyway ──────────
     * back_to == NULL is "the client is gone".  The SC must still come off the
     * server: otherwise it keeps running on time that belongs to nobody, which
     * is exactly the budget leak donation is supposed to make impossible. */
    {
        struct KSchedContext *sc = rf_sc(5, 20);
        struct KReply reply;
        struct task server;
        ASSERT_NOT_NULL(sc);
        memset(&reply, 0, sizeof(reply));
        memset(&server, 0, sizeof(server));

        server.sched_ctx = sc;
        kreply_note_donation(&reply, sc, &server);
        kreply_return_donation(&reply, NULL);
        ASSERT_NULL(server.sched_ctx);
        ASSERT_NULL(reply.donated_sc);
    }

    /* ── D-3: returning the loan closes the server's accounting run ───────
     * The time the server actually used must earn its replenishment against
     * the period it was used in, not the next holder's.  If the flush were
     * missing, consumed_run would be carried into whatever the SC is lent to
     * next and the conservation law would read wrong for both. */
    {
        struct KSchedContext *sc = rf_sc(5, 20);
        struct KReply reply;
        struct task client, server;
        ASSERT_NOT_NULL(sc);
        memset(&reply, 0, sizeof(reply));
        memset(&client, 0, sizeof(client));
        memset(&server, 0, sizeof(server));

        server.sched_ctx = sc;
        kreply_note_donation(&reply, sc, &server);

        (void)kschedctx_charge_tick(sc, 40);   /* server spends borrowed time */
        (void)kschedctx_charge_tick(sc, 41);
        ASSERT_EQ(sc->consumed_run, 2u);

        kreply_return_donation(&reply, &client);
        ASSERT_EQ(sc->consumed_run, 0u);       /* flushed, not carried */
        ASSERT_EQ(sc->refill_count, 1u);
        ASSERT_EQ(rf_total(sc), 5u);

        /* and it comes back one period after the SERVER spent it */
        ASSERT_EQ(kschedctx_apply_refills(sc, 59), 0);
        ASSERT_EQ(kschedctx_apply_refills(sc, 60), 1);
        ASSERT_EQ(sc->remaining_budget, 5u);
    }

    /* ── R-8: applying refills never exceeds the configured budget ───────
     * Belt and braces: if some other path ever double-credits, the ceiling
     * turns a silent over-grant into a merely wrong-but-bounded one. */
    {
        struct KSchedContext *sc = rf_sc(4, 10);
        ASSERT_NOT_NULL(sc);
        (void)kschedctx_charge_tick(sc, 1);
        kschedctx_flush_run(sc);
        sc->remaining_budget = 4;               /* simulate a double credit */
        (void)kschedctx_apply_refills(sc, 11);
        ASSERT_EQ(sc->remaining_budget, 4u);
    }
}
