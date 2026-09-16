/*
 * it_t347_t350.c — the adversarial phase (SMP roadmap §9.3 step 5).
 *
 * ── What this is for, and what it is not ────────────────────────────────────
 *
 * Step 4 made four processors schedule, and the whole suite passes there.  That
 * is a weaker statement than it looks: the suite's threads happen to be spread
 * across cores, so what it exercises is whatever interleavings fall out of that
 * — never the ones it does not happen to produce.  "It passes on four cores"
 * is not "it is correct on four cores", and the gap between those two sentences
 * is this file.
 *
 * So these tests do not merely RUN on several processors, they AIM several
 * processors at one object and check something that can only be true if the
 * kernel serialised them properly.  Each one is pointed at a specific piece of
 * machinery:
 *
 *   T347  the reply object and the endpoint queue — several callers on
 *         different cores calling one server, each checking it got ITS OWN
 *         answer.  A reply delivered to the wrong caller is the classic
 *         multiprocessor IPC defect and is invisible to a test with one client.
 *   T348  the derivation tree — several cores minting and deleting from one
 *         capability while a fifth revokes it out from under them.
 *   T349  the retype sequence — several cores retyping into the SAME slot,
 *         where exactly one may win and the losers must lose CLEANLY.  §9.2
 *         re-derived that path's atomicity argument and found the code right
 *         for a different reason than its comment gave; this runs it.
 *   T350  the death machinery of step 4 — several cores killing the same
 *         thread at once, which is what `task->on_cpu`, the idempotent reap
 *         enqueue and the stalling Suspend were all written for.
 *
 * ── What they found ─────────────────────────────────────────────────────────
 *
 * Four defects, and none of them was in the code written for SMP:
 *
 *   T349  RETYPE2 recorded its carve window with two separate reads of
 *         `ut->used`.  On four cores that window covers another core's
 *         allocation, so the rollback handed a live block back to the
 *         allocator and two cores built objects in one block.
 *   T350  `sys_tcb_exit` dropped the resolve's reference on the line ABOVE the
 *         call that dereferenced the pointer.
 *   T350  `task->terminal` was a plain byte tested by an unlocked read, so
 *         four cores calling Exit on one thread all entered its teardown and
 *         released one reference four times.
 *   T333  — not one of these tests, but flushed out by the load they put on
 *         the dispatcher: a thread already dequeued is in no queue, so a
 *         `Suspend` could not take it out of one, and the dispatcher then
 *         marked it RUNNING and cleared its reschedule flag.  The suspend was
 *         lost.  T333 suspends a thread and reads its registers, which the
 *         kernel refuses for a RUNNING one, so it noticed.  The fix's first
 *         version was too broad — it dropped any choice that was not READY,
 *         and a thread can legitimately be queued while BLOCKED_REPLY, so
 *         dropping one wedged the system.  SUSPENDED is the whole rule.
 *
 * The first three presented identically — a reference assert on an object
 * whose `type` field read 0, a type nothing creates, which is what a header
 * already zero-filled by the free looks like.  The asserts NAME the object now.
 *
 * ── What they cannot prove (roadmap §9.4) ───────────────────────────────────
 *
 * QEMU's TCG interleaves; it does not reorder.  These find LOGIC races — two
 * cores reaching one structure in an order nobody arranged for — and they will
 * not find a wrong `memory_order` on a relaxed atomic.  Four defects found
 * this way is evidence the method works, not evidence the kernel is free of
 * the other kind.  Saying so here is cheaper than discovering it in a ledger
 * row later.
 *
 * ── On one processor ────────────────────────────────────────────────────────
 *
 * Every test here passes on one processor too, and the invariants are the same
 * ones; the workers take turns instead of contending.  That is deliberate:
 * a test that only exists on a four-core machine is a test that rots.  Each
 * one reports how many distinct cores its workers actually landed on, so the
 * log says which of the two things was measured.
 */
#include "it_priv.h"

/* Four, because that is what a four-processor run has, and because the point
 * is one worker per core rather than a crowd on each. */
#define ADV_WORKERS   4u
#define ADV_STACK     4096u

/* How long the workers hammer.  Ticks rather than iterations: an iteration
 * count is a wall-clock duration that changes with the core count, which is
 * the mistake step 4 found in the suite's waits. */
#define ADV_TICKS     8u

/* Leaves of the suite's object CNode.  T311 holds 220..243 and the rotating
 * pool ends at IT_OBJ_SLOT_SPAN, so 244.. is clear; 252/253 are the page-table
 * scratch.  Written as an explicit range so the next person adding one can see
 * what is taken. */
#define ADV_LEAF_BASE (IT_OBJ_SLOT_SPAN + 44u)          /* 244 */
#define ADV_LEAF_DST(i) (ADV_LEAF_BASE + (uint32_t)(i)) /* 244..247, one per worker */
#define ADV_LEAF_SRC  (ADV_LEAF_BASE + 4u)              /* 248, the shared subject */
#define ADV_LEAF_ONE  (ADV_LEAF_BASE + 5u)              /* 249, the ONE contested slot */
#define ADV_LEAF_UT   (ADV_LEAF_BASE + 6u)              /* 250, a sub-untyped */
/*
 * The last two leaves of the object CNode (it has 256; 252/253 are the
 * page-table scratch).  T347's endpoint and reply object live here rather than
 * in the ROTATING POOL that `it_ep_create_slot` and `it_retype_slot_alloc`
 * hand out, and that is not tidiness: T324 asserts a recorded ceiling on how
 * often that pool recycles a live leaf, and these tests hold their objects for
 * eight ticks of real time.  Spending a budget another test measures makes the
 * two tests fail each other by turns, which is how this was found.
 */
#define ADV_LEAF_EP   254u
#define ADV_LEAF_RP   255u

#define ADV_MAGIC 0x5A5Aull

enum {
    ADV_KIND_IDLE = 0,
    ADV_KIND_CALL,      /* T347 */
    ADV_KIND_MINT,      /* T348 */
    ADV_KIND_RETYPE,    /* T349 */
    ADV_KIND_KILL,      /* T350 */
};

/*
 * The harness state.
 *
 * Every counter is indexed BY WORKER and written only by that worker, which is
 * why none of it is atomic: the supervisor sums after the workers have stopped
 * and said so.  A shared counter would need a read-modify-write from four
 * cores, and then the test's own instrumentation would be the thing under
 * test — a flaky total would be indistinguishable from the defect it is
 * looking for.
 */
static volatile uint32_t g_adv_kind;
static volatile uint32_t g_adv_go;
static volatile uint32_t g_adv_stop;
static volatile uint32_t g_adv_entered[ADV_WORKERS];
static volatile uint32_t g_adv_exited[ADV_WORKERS];
static volatile uint32_t g_adv_ops[ADV_WORKERS];      /* attempts */
static volatile uint32_t g_adv_won[ADV_WORKERS];      /* attempts that succeeded */
static volatile uint32_t g_adv_bad[ADV_WORKERS];      /* answers nothing explains */
static volatile long     g_adv_err[ADV_WORKERS];      /* the first such answer */

/* What the workers work on.  Published before `go`, read after. */
static volatile long g_adv_ep;
static volatile long g_adv_src;
static volatile long g_adv_ut;
static volatile long g_adv_victim[ADV_WORKERS];

static uint8_t g_adv_stack[ADV_WORKERS][ADV_STACK] __attribute__((aligned(16)));
static long    g_adv_tcb[ADV_WORKERS];
static uint8_t g_adv_cpu[ADV_WORKERS];

/* An answer this test did not ask for.  Recorded once per worker, with the
 * code, because "something failed" and "NOT_FOUND where ALREADY_EXISTS was
 * expected" are different bugs and the second names itself. */
static void adv_unexpected(uint32_t i, long r) {
    if (g_adv_bad[i]++ == 0u) g_adv_err[i] = r;
}

/* ── the workers ─────────────────────────────────────────────────────────── */

static void adv_call_loop(uint32_t i) {
    uint32_t n = 0;
    while (!g_adv_stop) {
        struct iris_msg m;
        iris_msg_zero(&m);
        /* A value only THIS worker can produce, and a different one every
         * round: the index alone would not catch a reply that arrived one
         * round late. */
        uint64_t mine = ((uint64_t)(i + 1u) << 32) | (uint64_t)(++n);
        m.label = mine;
        g_adv_ops[i]++;
        long r = iris_msg_call(g_adv_ep, &m);
        if (r != 0) { adv_unexpected(i, r); break; }
        if (m.label != (mine ^ ADV_MAGIC)) {
            /* The answer to somebody else's question.  This is the whole
             * reason the test exists. */
            adv_unexpected(i, (long)0x7E51);
            break;
        }
        g_adv_won[i]++;
    }
}

static void adv_mint_loop(uint32_t i) {
    const long dst_leaf = (long)ADV_LEAF_DST(i);
    while (!g_adv_stop) {
        g_adv_ops[i]++;
        long r = it_invoke2(g_adv_src, INV_CSPACE_MINT,
                            (long)(((uint64_t)dst_leaf << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                            (long)RIGHT_READ);
        if (r == 0) {
            g_adv_won[i]++;
            long d = it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, dst_leaf);
            if (d != 0) { adv_unexpected(i, d); break; }
        } else if (r == (long)IRIS_ERR_NOT_FOUND ||
                   r == (long)IRIS_ERR_ALREADY_EXISTS) {
            /* NOT_FOUND: the revoke reached the source between the resolve and
             * the install, which is the race this test is for and is a CLEAN
             * refusal.  ALREADY_EXISTS: our own previous mint is still there,
             * which cannot happen with one worker per slot but is refused
             * rather than silently overwritten, so it is accepted here as a
             * refusal too rather than being called a defect. */
        } else {
            adv_unexpected(i, r);
            break;
        }
    }
}

static void adv_retype_loop(uint32_t i) {
    while (!g_adv_stop) {
        g_adv_ops[i]++;
        long r = it_invoke(g_adv_ut, INV_UNTYPED_RETYPE,
                           (long)((uint64_t)IRIS_KOBJ_ENDPOINT | (1ULL << 32)),
                           (long)(((uint64_t)ADV_LEAF_ONE << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                           0);
        if (r == 0) {
            g_adv_won[i]++;
            /* The winner gives it back, so the next round has somebody to
             * race.  A retype that succeeded and could not be undone would
             * turn this into a memory-exhaustion test. */
            long d = it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE,
                                (long)ADV_LEAF_ONE);
            if (d != 0 && d != (long)IRIS_ERR_NOT_FOUND) { adv_unexpected(i, d); break; }
        } else if (r == (long)IRIS_ERR_ALREADY_EXISTS ||
                   r == (long)IRIS_ERR_NO_MEMORY) {
            /* ALREADY_EXISTS: somebody else holds the slot — the refusal the
             * exclusive install exists to give.  NO_MEMORY: the sub-untyped is
             * a bump allocator and does not rewind until RESET, so it runs out;
             * that is the budget working, not a race. */
        } else {
            adv_unexpected(i, r);
            break;
        }
    }
}

static void adv_kill_loop(uint32_t i) {
    /*
     * Every worker kills EVERY victim, so each thread is killed four times by
     * four processors.  Killing one victim each would be four independent
     * kills and would test nothing this file is about.
     */
    while (!g_adv_stop) {
        for (uint32_t v = 0; v < ADV_WORKERS && !g_adv_stop; v++) {
            long t = g_adv_victim[v];
            if (t == 0) continue;
            g_adv_ops[i]++;
            long r = it_invoke0(t, INV_TCB_EXIT);
            if (r == 0) { g_adv_won[i]++; continue; }
            /* NOT_FOUND is the honest answer once the thread is terminal: the
             * capability still names the object, the execution is over. */
            if (r != (long)IRIS_ERR_NOT_FOUND) { adv_unexpected(i, r); return; }
        }
        (void)it_sys0(SYS_YIELD);
    }
}

static void adv_worker(uint64_t arg) {
    uint32_t i = (uint32_t)arg;
    if (i >= ADV_WORKERS) {
        /* The entry argument did not arrive.  Say so where the supervisor can
         * see it rather than corrupting somebody else's slot. */
        g_adv_bad[0] = 0xFFFFFFFFu;
        it_sys1(SYS_EXIT, 0);
        for (;;) { }
    }

    while (!g_adv_go) (void)it_sys0(SYS_YIELD);
    g_adv_entered[i] = 1u;

    switch (g_adv_kind) {
    case ADV_KIND_CALL:   adv_call_loop(i);   break;
    case ADV_KIND_MINT:   adv_mint_loop(i);   break;
    case ADV_KIND_RETYPE: adv_retype_loop(i); break;
    case ADV_KIND_KILL:   adv_kill_loop(i);   break;
    default: break;
    }

    g_adv_exited[i] = 1u;
    it_sys1(SYS_EXIT, 0);
    for (;;) { }
}

/* ── the harness ─────────────────────────────────────────────────────────── */

static void adv_reset(uint32_t kind) {
    g_adv_kind = kind;
    g_adv_go = 0; g_adv_stop = 0;
    for (uint32_t i = 0; i < ADV_WORKERS; i++) {
        g_adv_entered[i] = 0; g_adv_exited[i] = 0;
        g_adv_ops[i] = 0; g_adv_won[i] = 0; g_adv_bad[i] = 0; g_adv_err[i] = 0;
        g_adv_tcb[i] = 0; g_adv_cpu[i] = 0xFFu; g_adv_victim[i] = 0;
    }
}

/* Start the workers and record which processor each landed on.
 *
 * The placement is not CHOSEN — there is no affinity invocation, because
 * nothing migrates a thread and a kernel that moves them has to decide when.
 * It is READ, through `iris_tcb_info.home_cpu`, so that a run can say whether
 * it was contending or taking turns instead of assuming. */
static int adv_start(uint32_t count, const char **why) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t rsp = ((uint64_t)(uintptr_t)(g_adv_stack[i] + ADV_STACK)) & ~0xFULL;
        long t = it_thread_create((uint64_t)(uintptr_t)adv_worker, rsp, (uint64_t)i);
        if (t < 0) { *why = "worker create"; return 0; }
        g_adv_tcb[i] = t;
        struct iris_tcb_info info;
        if (it_invoke1(t, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) {
            *why = "worker info"; return 0;
        }
        g_adv_cpu[i] = info.home_cpu;
    }
    return 1;
}

/* How many distinct processors the workers landed on. */
static uint32_t adv_spread(uint32_t count) {
    uint32_t seen = 0u, n = 0u;
    for (uint32_t i = 0; i < count; i++) {
        if (g_adv_cpu[i] >= 32u) continue;
        if (seen & (1u << g_adv_cpu[i])) continue;
        seen |= (1u << g_adv_cpu[i]);
        n++;
    }
    return n;
}

/* Let them run for a bounded stretch of REAL time, then stop them and wait for
 * every one to say it has left its loop.  Both bounds are time, not iteration
 * counts: on four processors an iteration count is a duration that depends on
 * how many cores there are, which is the mistake step 4 found throughout the
 * suite's waits. */
static int adv_run_and_stop(uint32_t count, const char **why) {
    it_settle(ADV_TICKS);
    g_adv_stop = 1u;

    for (uint32_t t = 0; t < 200u; t++) {
        uint32_t out = 0;
        for (uint32_t i = 0; i < count; i++) out += g_adv_exited[i];
        if (out == count) return 1;
        it_settle(1);
    }
    *why = "a worker never left its loop";
    return 0;
}

static void adv_reap(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (g_adv_tcb[i] > 0) (void)it_invoke0(g_adv_tcb[i], INV_TCB_EXIT);
    }
    it_quiesce_reaper();
}

/* One line per test saying what actually happened: how many cores, how many
 * attempts, how many of them won.  A race test that reports nothing cannot be
 * told from a race test whose workers never started. */
static void adv_report(const char *name, uint32_t count) {
    uint32_t ops = 0, won = 0;
    for (uint32_t i = 0; i < count; i++) { ops += g_adv_ops[i]; won += g_adv_won[i]; }
    it_serial_write("[IRIS][TEST] ");
    it_serial_write(name);
    it_serial_write(" cores=");
    it_log_num(adv_spread(count));
    it_serial_write(" attempts=");
    it_log_num(ops);
    it_serial_write(" won=");
    it_log_num(won);
    it_serial_write("\n");
}

/*
 * The baseline, with ONE quantity left out and the reason stated.
 *
 * Everything these tests create is expected back: threads, address spaces,
 * mappings, handles, and the objects the races themselves make — endpoints,
 * reply objects, CNodes.  FRAMES are left out, and not because a frame leak
 * would not matter.
 *
 * Two reasons, and the second is the one that decides it.  `it_thread_create`
 * retypes a frame for every thread's IPC buffer and the harness never returns
 * it — the pool is carved once and reused for the whole run, so a thread's
 * frame outlives the thread.  And these tests hold their bracket open for
 * eight ticks of REAL time, which is long enough for the services running
 * beside the suite to map and unmap pages of their own; a count taken before
 * and after is then measuring their work as well as this test's.  It was
 * observed both up and DOWN across a window in which this test created no
 * frame at all.
 *
 * None of the four races here creates a frame, so nothing is lost by not
 * asking.  A change is still PRINTED, so a real one is visible rather than
 * silently tolerated — that is the difference between leaving a check out and
 * pretending it passed.
 */
static int adv_baseline(const struct it_snap *b, const struct it_snap *a,
                        uint32_t threads, const char **why) {
    if (!a->ok || !b->ok)              { *why = "snap read"; return 0; }
    if (a->task != b->task)            { *why = "task live drift"; return 0; }
    if (a->proclive != b->proclive)    { *why = "proc live drift"; return 0; }
    if (a->hlive != b->hlive)          { *why = "handle leak"; return 0; }
    if (a->vs != b->vs)                { *why = "vspace drift"; return 0; }
    if (a->map != b->map)              { *why = "mapping drift"; return 0; }
    if (a->fr != b->fr) {
        it_serial_write("[IRIS][TEST] adversary frames before=");
        it_log_num(b->fr);
        it_serial_write(" after=");
        it_log_num(a->fr);
        it_serial_write(" threads=");
        it_log_num(threads);
        it_serial_write("\n");
    }
    (void)threads;
    return 1;
}

/* Did any worker see an answer this test cannot explain? */
static int adv_clean(uint32_t count, const char **why) {
    for (uint32_t i = 0; i < count; i++) {
        if (g_adv_bad[i] == 0u) continue;
        it_serial_write("[IRIS][TEST] adversary worker ");
        it_log_num(i);
        it_serial_write(" cpu ");
        it_log_num(g_adv_cpu[i]);
        it_serial_write(" saw ");
        it_log_num((uint32_t)(-g_adv_err[i]));
        it_serial_write(" (negated) ");
        it_log_num(g_adv_bad[i]);
        it_serial_write(" times\n");
        *why = (g_adv_err[i] == 0x7E51)
             ? "a reply reached the wrong caller"
             : "a worker saw an answer the test cannot explain";
        return 0;
    }
    return 1;
}

/* Every worker must have got SOMEWHERE.  A test whose workers never ran is a
 * test that passes by doing nothing, and on a machine where thread placement
 * or the tick went wrong that is exactly what would happen. */
static int adv_made_progress(uint32_t count, const char **why) {
    for (uint32_t i = 0; i < count; i++) {
        if (g_adv_entered[i] == 0u) { *why = "a worker never entered its loop"; return 0; }
        if (g_adv_ops[i] == 0u)     { *why = "a worker never made an attempt";  return 0; }
    }
    return 1;
}

/* ── T347: one server, four callers, four processors ─────────────────────────
 *
 * Each caller sends a value only it can produce and requires the answer to be
 * ITS OWN value transformed.  The server is this thread, answering with
 * `label ^ ADV_MAGIC` through a single reply object — strictly receive, reply,
 * receive, because a second receive before the reply would re-stage the reply
 * object and lose the caller it was holding.
 *
 * What it can catch that a one-client test cannot: a reply delivered to the
 * wrong caller.  Four callers blocked on one endpoint from four processors is
 * the arrangement in which the endpoint's wait queue, the reply object's
 * binding and the caller's resume all have to agree about WHICH thread is
 * owed an answer — and a defect there hands somebody else's answer over,
 * which every caller would otherwise accept without looking.
 * Invariants: I1, I3, S1. */
void test_t347(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "concurrent call/reply";

    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_EP);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_RP);
    if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE,
                  (long)((uint64_t)IRIS_KOBJ_ENDPOINT | (1ULL << 32)),
                  (long)(((uint64_t)ADV_LEAF_EP << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                  0) != 0) { it_fail("T347", "endpoint"); return; }
    if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE,
                  (long)((uint64_t)IRIS_KOBJ_REPLY | (1ULL << 32)),
                  (long)(((uint64_t)ADV_LEAF_RP << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                  0) != 0) { it_fail("T347", "reply object"); return; }
    long ep = (long)IT_OBJ_CPTR(ADV_LEAF_EP);
    long rp = (long)IT_OBJ_CPTR(ADV_LEAF_RP);

    adv_reset(ADV_KIND_CALL);
    g_adv_ep = ep;
    if (ok && !adv_start(ADV_WORKERS, &why)) ok = 0;

    uint32_t served = 0;
    if (ok) {
        g_adv_go = 1u;
        /*
         * Serve until the workers are done, bounded in real time.  Not a
         * blocking receive: this thread is also the one that has to notice
         * they have stopped, and a server blocked in `recv` with no caller
         * left would never look.
         */
        long t0 = it_sys0(SYS_CLOCK_GET);
        for (uint32_t spin = 0; spin < 4000000u; spin++) {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.reply = rp;
            if (iris_msg_nb_recv(ep, &m) == 0) {
                uint64_t asked = m.label;
                iris_msg_zero(&m);
                m.label = asked ^ ADV_MAGIC;
                if (iris_msg_reply(rp, &m) != 0) { ok = 0; why = "reply failed"; break; }
                served++;
                continue;
            }
            long now = it_sys0(SYS_CLOCK_GET);
            if (t0 > 0 && now > 0 &&
                (uint64_t)(now - t0) >= (uint64_t)ADV_TICKS * IRIS_TICK_NS) {
                g_adv_stop = 1u;
            }
            /* Once stopped, leave as soon as every worker has said so — but
             * keep serving until then, because a worker blocked in `call` is
             * waiting for THIS thread and will never see the stop otherwise. */
            if (g_adv_stop) {
                uint32_t out = 0;
                for (uint32_t i = 0; i < ADV_WORKERS; i++) out += g_adv_exited[i];
                if (out == ADV_WORKERS) break;
            }
            (void)it_sys0(SYS_YIELD);
        }
    }

    if (ok && !adv_made_progress(ADV_WORKERS, &why)) ok = 0;
    if (ok && !adv_clean(ADV_WORKERS, &why)) ok = 0;

    /* Every answer this thread sent was accepted by the caller that asked for
     * it — `adv_clean` proves that — so the totals must agree.  A server that
     * replied more often than the callers were satisfied would mean an answer
     * went somewhere nobody was counting. */
    if (ok) {
        uint32_t won = 0;
        for (uint32_t i = 0; i < ADV_WORKERS; i++) won += g_adv_won[i];
        if (served != won) { ok = 0; why = "served and satisfied disagree"; }
    }
    if (ok) adv_report("T347", ADV_WORKERS);

    adv_reap(ADV_WORKERS);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_RP);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_EP);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !adv_baseline(&b, &a, ADV_WORKERS, &why)) ok = 0;
    if (ok) it_pass("T347"); else it_fail("T347", why);
}

/* ── T348: four cores deriving from one capability while a fifth revokes it ──
 *
 * Each worker mints the shared source into a slot of its own and deletes it
 * again, for as long as it is allowed to; this thread revokes the source
 * underneath them, repeatedly.
 *
 * The property: a mint either succeeds or is refused CLEANLY, and after the
 * last revoke nothing derived from the source survives.  The failure it is
 * looking for is a node parented to one that has been freed — a mint that
 * resolved its parent, was preempted while the revoke destroyed it, and then
 * installed itself as a child of nothing.  That leaves a capability the tree
 * cannot reach, which is charter A9's whole subject, and it is invisible until
 * something walks the tree.
 * Invariants: C2, C6, A9. */
void test_t348(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "concurrent mint under revoke";

    /* The subject is retyped into a leaf of its own, not the rotating pool:
     * this test deletes and re-derives it, and a pool slot would be handed to
     * somebody else mid-race. */
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_SRC);
    if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE,
                  (long)((uint64_t)IRIS_KOBJ_ENDPOINT | (1ULL << 32)),
                  (long)(((uint64_t)ADV_LEAF_SRC << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                  0) != 0) {
        it_fail("T348", "source"); return;
    }
    long src = (long)IT_OBJ_CPTR(ADV_LEAF_SRC);

    for (uint32_t i = 0; i < ADV_WORKERS; i++)
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_DST(i));

    adv_reset(ADV_KIND_MINT);
    g_adv_src = src;
    if (ok && !adv_start(ADV_WORKERS, &why)) ok = 0;

    uint32_t revokes = 0;
    if (ok) {
        g_adv_go = 1u;
        long t0 = it_sys0(SYS_CLOCK_GET);
        for (uint32_t spin = 0; spin < 100000u; spin++) {
            long r = it_invoke0(src, INV_CSPACE_REVOKE);
            /* The count is what was revoked this time; any count is legal,
             * including zero.  What is not legal is an error. */
            if (r < 0) { ok = 0; why = "revoke failed"; break; }
            revokes++;
            long now = it_sys0(SYS_CLOCK_GET);
            if (t0 > 0 && now > 0 &&
                (uint64_t)(now - t0) >= (uint64_t)ADV_TICKS * IRIS_TICK_NS) break;
            (void)it_sys0(SYS_YIELD);
        }
    }
    if (ok && !adv_run_and_stop(ADV_WORKERS, &why)) ok = 0;

    if (ok && !adv_made_progress(ADV_WORKERS, &why)) ok = 0;
    if (ok && !adv_clean(ADV_WORKERS, &why)) ok = 0;
    if (ok && revokes == 0u) { ok = 0; why = "nothing was revoked"; }

    /* The last word: one more revoke, and then nothing derived from the source
     * may answer.  A descendant that survives here is the defect — a node the
     * tree could not reach when it walked it. */
    if (ok && it_invoke0(src, INV_CSPACE_REVOKE) < 0) { ok = 0; why = "final revoke"; }
    if (ok) {
        for (uint32_t i = 0; i < ADV_WORKERS; i++) {
            if (it_invoke2((long)IT_OBJ_CPTR(ADV_LEAF_DST(i)), INV_CAP_IDENTIFY, 0, 0) >= 0) {
                ok = 0; why = "a derived capability survived the revoke";
            }
        }
    }
    if (ok) adv_report("T348", ADV_WORKERS);

    adv_reap(ADV_WORKERS);
    for (uint32_t i = 0; i < ADV_WORKERS; i++)
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_DST(i));
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_SRC);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !adv_baseline(&b, &a, ADV_WORKERS, &why)) ok = 0;
    if (ok) it_pass("T348"); else it_fail("T348", why);
}

/* ── T349: four cores retyping into ONE slot ─────────────────────────────────
 *
 * §9.2 re-derived the retype sequence's atomicity and found the comment wrong
 * and the code right: the occupancy scan is an optimisation, and the
 * authoritative check is the exclusive install under the tree's lock, which a
 * lost race rolls back exactly.  That was an argument.  This runs it.
 *
 * Every worker retypes an endpoint into the same slot; the winner deletes it
 * so the next round has somebody to race.  What must hold: every attempt is a
 * success or one of two named refusals, and when it is over the sub-untyped
 * returns EXACTLY to where it started after a RESET.  A retype that consumed
 * budget and installed nothing — the shape a rolled-back race takes if the
 * rollback is incomplete — shows up as a budget that does not come back.
 * Invariants: U1, U4, M1. */
void test_t349(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "concurrent retype into one slot";

    /* A sub-untyped of its own: the suite's main budget is shared with every
     * other test, and this one deliberately exhausts what it is given. */
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_UT);
    if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE,
                  (long)((uint64_t)IRIS_KOBJ_UNTYPED | (1ULL << 32)),
                  (long)(((uint64_t)ADV_LEAF_UT << 32) | (uint64_t)IT_OBJ_CNODE_SLOT),
                  (long)(64u * 1024u)) != 0) {
        it_fail("T349", "sub-untyped"); return;
    }
    long ut = (long)IT_OBJ_CPTR(ADV_LEAF_UT);
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_ONE);

    struct it_utq_one u0, u1;
    if (ok && !it_utq_1(ut, &u0)) { ok = 0; why = "budget query"; }

    adv_reset(ADV_KIND_RETYPE);
    g_adv_ut = ut;
    if (ok && !adv_start(ADV_WORKERS, &why)) ok = 0;
    if (ok) {
        g_adv_go = 1u;
        if (!adv_run_and_stop(ADV_WORKERS, &why)) ok = 0;
    }

    if (ok && !adv_made_progress(ADV_WORKERS, &why)) ok = 0;
    if (ok && !adv_clean(ADV_WORKERS, &why)) ok = 0;

    /* Somebody has to have won, or the slot was never contested and the test
     * measured nothing. */
    if (ok) {
        uint32_t won = 0;
        for (uint32_t i = 0; i < ADV_WORKERS; i++) won += g_adv_won[i];
        if (won == 0u) { ok = 0; why = "no retype ever succeeded"; }
    }

    /* And the budget comes all the way back.  This is the assertion about the
     * ROLLBACK: a race that lost after reserving, and un-reserved incompletely,
     * leaves bytes that a RESET cannot return because nothing owns them. */
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_ONE);
    adv_reap(ADV_WORKERS);
    if (ok && it_invoke0(ut, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset"; }
    if (ok && !it_utq_1(ut, &u1)) { ok = 0; why = "budget query 2"; }
    if (ok && u1.used_bytes != u0.used_bytes) {
        ok = 0; why = "a raced retype left bytes a reset could not return";
    }
    if (ok) adv_report("T349", ADV_WORKERS);

    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)ADV_LEAF_UT);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !adv_baseline(&b, &a, ADV_WORKERS, &why)) ok = 0;
    if (ok) it_pass("T349"); else it_fail("T349", why);
}

/* ── T350: four cores killing the same threads ───────────────────────────────
 *
 * This is step 4's death machinery under the load it was written for.  Four
 * workers each kill all four VICTIMS, so every victim is killed four times
 * from four processors — and one of those kills is racing the victim's own
 * core, which may be executing it at that instant.
 *
 * What it exercises, exactly: `task->on_cpu` (a thread being torn down while a
 * processor is still standing on it frees the address space under a live
 * instruction pointer); the idempotent reap enqueue (two paths hand the same
 * thread over and neither can tell whether the other already has); and the
 * mark-and-leave shape of an external kill (a killer is often inside an
 * endpoint, so it cannot wait for the dispatch that would finish the job).
 *
 * The invariant is the one a leak or a double-free both break: the live task
 * count returns to where it started, and the reap ring never refused an entry.
 * Invariants: L1, L4, S4. */
static uint8_t g_adv_victim_stack[ADV_WORKERS][ADV_STACK] __attribute__((aligned(16)));

static void adv_victim_body(void) {
    /* Something to be killed in the middle of: a syscall, then user code, so a
     * kill lands sometimes in ring 0 and sometimes in ring 3. */
    for (;;) {
        (void)it_sys0(SYS_YIELD);
        for (volatile uint32_t k = 0; k < 2000u; k++) { }
    }
}

void test_t350(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "concurrent kill";

    uint32_t w6a[5], w6b[5];
    if (!it_sched_ext6(w6a)) { it_fail("T350", "processor tier"); return; }

    adv_reset(ADV_KIND_KILL);

    for (uint32_t v = 0; ok && v < ADV_WORKERS; v++) {
        uint64_t rsp = ((uint64_t)(uintptr_t)(g_adv_victim_stack[v] + ADV_STACK)) & ~0xFULL;
        long t = it_thread_create((uint64_t)(uintptr_t)adv_victim_body, rsp, 0);
        if (t < 0) { ok = 0; why = "victim create"; break; }
        g_adv_victim[v] = t;
    }
    /* Let them actually be RUNNING somewhere before anybody kills them: a
     * thread killed before it was ever dispatched never exercises the on-CPU
     * path, which is the whole subject. */
    if (ok) it_settle(2);

    if (ok && !adv_start(ADV_WORKERS, &why)) ok = 0;
    if (ok) {
        g_adv_go = 1u;
        if (!adv_run_and_stop(ADV_WORKERS, &why)) ok = 0;
    }

    if (ok && !adv_made_progress(ADV_WORKERS, &why)) ok = 0;
    if (ok && !adv_clean(ADV_WORKERS, &why)) ok = 0;

    /*
     * Every victim was killed at least once, which is what says the workers
     * reached them at all.
     *
     * Not "exactly once": Exit on a thread that is already terminal answers
     * SUCCESS, not an error, and that is the right ABI — a killer racing
     * another killer should not be told its capability was bad.  So several
     * successes for one thread are legitimate and say nothing.
     *
     * What catches the double TEARDOWN is not this count but the baseline
     * below and the kernel's own reference asserts, which is how the defect
     * this test found actually surfaced: four cores entered one thread's
     * teardown and released one reference four times.
     */
    if (ok) {
        uint32_t won = 0;
        for (uint32_t i = 0; i < ADV_WORKERS; i++) won += g_adv_won[i];
        if (won < ADV_WORKERS) { ok = 0; why = "a victim was never killed"; }
    }

    adv_reap(ADV_WORKERS);
    for (uint32_t v = 0; v < ADV_WORKERS; v++) {
        if (g_adv_victim[v] > 0) (void)it_invoke0(g_adv_victim[v], INV_TCB_EXIT);
    }
    it_quiesce_reaper();

    /* The reap ring refused nothing.  T344 pins this at zero for the whole
     * run; here it is checked across the one stretch that can actually fill
     * it — four processors handing threads over at once. */
    if (ok && !it_sched_ext6(w6b)) { ok = 0; why = "processor tier 2"; }
    if (ok && w6b[IT_S6_DEATHS_PENDING] != 0u) { ok = 0; why = "a death never finished"; }
    if (ok) adv_report("T350", ADV_WORKERS);

    for (uint32_t v = 0; v < ADV_WORKERS; v++) {
        if (g_adv_victim[v] > 0) it_slot_delete((uint32_t)g_adv_victim[v]);
    }
    for (uint32_t i = 0; i < ADV_WORKERS; i++) {
        if (g_adv_tcb[i] > 0) it_slot_delete((uint32_t)g_adv_tcb[i]);
    }
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    /* Twice the workers: this test starts victims as well. */
    if (ok && !adv_baseline(&b, &a, ADV_WORKERS * 2u, &why)) ok = 0;
    if (ok) it_pass("T350"); else it_fail("T350", why);
}
