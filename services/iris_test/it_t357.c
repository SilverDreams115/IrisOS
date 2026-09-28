/* SPDX-License-Identifier: Apache-2.0 */
/*
 * it_t357.c — the run queue under concurrent requeue.
 *
 * ── Why this test exists ───────────────────────────────────────────────────
 *
 * Three defects in a row lived in the same place and none of the suite's SMP
 * tests could see any of them.  What they had in common was not a subsystem;
 * it was a SHAPE: a thread being made dispatchable on a processor other than
 * the one doing it.
 *
 *   - `sched_set_domain` read `rq_queued`, unlinked, wrote the domain and
 *     relinked, taking the queue's lock three separate times.  Another core
 *     switching away from the same thread sets TASK_READY before it enqueues,
 *     so for a few instructions a runnable thread is not queued -- and a mover
 *     reading the flag in that window filed it under the domain it had before
 *     the write.
 *   - `SYS_TCB_SET_PRIORITY` wrote the field and stopped there.  Same key,
 *     same corruption, found only because the domain half was found first.
 *   - Neither sent the reschedule IPI.  A core halted in its idle loop never
 *     looked at the queue it had just been given something for, and a thread
 *     stayed READY, linked, with its priority bit set, for fifteen seconds.
 *
 * The suite had T347 (four callers, one server), T348 (four cores deriving
 * while a fifth revokes), T349 (four cores retyping into one slot) and T350
 * (four cores killing the same threads).  Every one of them stresses an
 * OBJECT under contention.  None of them moves a thread between queues while
 * another core is dispatching it, which is why all three survived.
 *
 * ── What it does ───────────────────────────────────────────────────────────
 *
 * Four threads that do nothing but count, so "is it running" is one read.
 * They are created in sequence and homed round-robin, so on a machine with
 * more than one processor they are not all on this one -- which is the
 * condition every defect above needed.
 *
 * Then the storm: each worker's PRIORITY is moved down and back, and its
 * DOMAIN is moved out of the schedule and back, over and over.  Both are the
 * key the run queue is indexed on; both were wrong; both are supposed to be a
 * requeue rather than a field write.
 *
 * ── What it asserts, and why that is the right assertion ───────────────────
 *
 * That every worker is STILL COUNTING afterwards.  Not that the numbers are
 * equal, not that they are fair -- a thread that is briefly out of the
 * schedule legitimately falls behind, and measuring that would be measuring
 * the machine.  A thread lost by a bad requeue stops for ever, and that is a
 * property no timing can fake.
 *
 * And that the duplicate-enqueue guard never fired.  It counts the times a
 * thread was enqueued while already queued; under a correct requeue it is
 * structurally zero, and a non-zero value is the corruption arriving by a
 * different road than a stopped counter.
 *
 * On one processor this passes trivially, and that is fine: it is the same
 * suite on every configuration, and the four-CPU lane is where it bites.
 * Invariants: S5, S13.
 */
#include "it_priv.h"

#define T357_WORKERS  4u
#define T357_ROUNDS   24u
/* Long enough that a worker which is merely behind gets its turn, short
 * enough that four of them do not dominate the run.  Bounded by TIME because
 * a count of settles is a fact about the processor. */
#define T357_BACK_MS  4000u

static volatile uint64_t g_t357_ticks[T357_WORKERS];
static volatile int      g_t357_stop;
static uint8_t           g_t357_stack[T357_WORKERS][4096];

static void t357_body(uint32_t i) {
    while (!g_t357_stop) {
        g_t357_ticks[i]++;
        it_sys1(SYS_YIELD, 0);
    }
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void t357_w0(void) { t357_body(0); }
static void t357_w1(void) { t357_body(1); }
static void t357_w2(void) { t357_body(2); }
static void t357_w3(void) { t357_body(3); }

/* Every worker's counter has moved since `base`, within the deadline. */
static int t357_all_advanced(const uint64_t *base, uint32_t *out_stuck) {
    long t0 = it_sys0(SYS_CLOCK_GET);
    for (;;) {
        uint32_t stuck = T357_WORKERS;
        for (uint32_t i = 0; i < T357_WORKERS; i++) {
            if (g_t357_ticks[i] == base[i]) { stuck = i; break; }
        }
        if (stuck == T357_WORKERS) return 1;
        *out_stuck = stuck;
        it_settle(1);
        long now = it_sys0(SYS_CLOCK_GET);
        if (t0 <= 0 || now <= 0) {
            /* No clock: fall back to a bounded count rather than spinning for
             * ever.  What is being waited for is a scheduling decision, and a
             * missing clock is not evidence against one. */
            for (int k = 0; k < 400; k++) {
                it_settle(1);
                uint32_t s2 = T357_WORKERS;
                for (uint32_t i = 0; i < T357_WORKERS; i++)
                    if (g_t357_ticks[i] == base[i]) { s2 = i; break; }
                if (s2 == T357_WORKERS) return 1;
                *out_stuck = s2;
            }
            return 0;
        }
        if ((uint64_t)(now - t0) > (uint64_t)T357_BACK_MS * 1000000ull) return 0;
    }
}

void test_t357(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a requeue is not a field write";
    long tcb[T357_WORKERS];
    uint32_t prio[T357_WORKERS];
    uint32_t stuck = 0;
    uint64_t base[T357_WORKERS];

    static void (*const entry[T357_WORKERS])(void) =
        { t357_w0, t357_w1, t357_w2, t357_w3 };

    g_t357_stop = 0;
    for (uint32_t i = 0; i < T357_WORKERS; i++) {
        g_t357_ticks[i] = 0;
        tcb[i] = -1;
        prio[i] = 0;
    }

    uint32_t started = 0;
    for (uint32_t i = 0; i < T357_WORKERS; i++) {
        uint64_t rsp = ((uint64_t)(uintptr_t)(g_t357_stack[i] +
                          sizeof(g_t357_stack[i]))) & ~0xFULL;
        tcb[i] = it_thread_create((uint64_t)(uintptr_t)entry[i], rsp, 0);
        if (tcb[i] < 0) { ok = 0; why = "thread"; break; }
        started++;
        struct iris_tcb_info info;
        if (it_invoke1(tcb[i], INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) {
            ok = 0; why = "tcb info"; break;
        }
        prio[i] = info.priority;
    }

    /* They all run before anything is done to them: a worker that never
     * started would make every assertion below meaningless. */
    if (ok && started == T357_WORKERS) {
        for (uint32_t i = 0; i < T357_WORKERS; i++) base[i] = 0;
        if (!t357_all_advanced(base, &stuck)) {
            ok = 0; why = "a worker never ran at all";
        }
    } else if (ok) {
        ok = 0; why = "thread";
    }

    uint32_t dup0 = 0;
    if (ok) {
        uint32_t w2[4] = { 0, 0, 0, 0 };
        (void)it_sched_ext2(w2);
        dup0 = w2[1];
    }

    /*
     * The storm.  Both keys, both directions, every worker, many times.
     *
     * The domain moves are what stopped a thread for ever; the priority moves
     * are the same operation on the other half of the key.  Interleaving them
     * is deliberate: the corruption that was found threaded one bucket's list
     * through another's, and it takes two different keys to build.
     */
    for (uint32_t r = 0; ok && r < T357_ROUNDS; r++) {
        for (uint32_t i = 0; i < T357_WORKERS; i++) {
            if (prio[i] > 0u) {
                (void)it_invoke2(tcb[i], INV_TCB_SET_PRIORITY,
                                 (long)(prio[i] - 1u), 0);
                (void)it_invoke2(tcb[i], INV_TCB_SET_PRIORITY,
                                 (long)prio[i], 0);
            }
            (void)it_invoke2((long)IT_CPTR_DOMAIN_CONTROL, INV_DOMAIN_SET,
                             tcb[i], 1);
            (void)it_invoke2((long)IT_CPTR_DOMAIN_CONTROL, INV_DOMAIN_SET,
                             tcb[i], 0);
        }
        it_settle(1);
    }

    /* And every one of them is still going. */
    if (ok) {
        for (uint32_t i = 0; i < T357_WORKERS; i++) base[i] = g_t357_ticks[i];
        if (!t357_all_advanced(base, &stuck)) {
            ok = 0; why = "a worker stopped and did not come back";
            it_fz_note("T357", stuck, (uint32_t)g_t357_ticks[stuck], 0u);
        }
    }

    /* A thread enqueued while already queued is the corruption arriving by a
     * different road: the guard rejects it, so the count is the evidence. */
    if (ok) {
        uint32_t w2[4] = { 0, 0, 0, 0 };
        (void)it_sched_ext2(w2);
        if (w2[1] != dup0) {
            ok = 0; why = "a thread was enqueued while already queued";
            it_fz_note("T357", dup0, w2[1], 0u);
        }
    }

    g_t357_stop = 1;
    for (uint32_t i = 0; i < started; i++) {
        /* Back into the scheduled domain first, or a worker parked out of
         * time never reaches its own exit and the reaper waits for ever. */
        (void)it_invoke2((long)IT_CPTR_DOMAIN_CONTROL, INV_DOMAIN_SET,
                         tcb[i], 0);
    }
    for (int k = 0; k < 200; k++) it_settle(1);
    for (uint32_t i = 0; i < started; i++)
        if (tcb[i] >= 0) it_slot_delete((uint32_t)tcb[i]);
    it_quiesce_reaper();

    if (ok) it_pass("T357"); else it_fail("T357", why);
}
