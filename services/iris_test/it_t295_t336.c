/*
 * it_t295_t336.c — the newest tests, and four that were written out of order.
 *
 * T326 through T336 in order, plus T324, T319, T296 and T295, which live at
 * the end of the suite because that is where they were written: the numbering
 * is chronological but the FILE order is where each test was inserted, and
 * these four were added after their numeric neighbours had moved on.  This is
 * the one file whose name overlaps its neighbours', and it says so rather
 * than pretending the range is tidy.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
void test_t326(void) {
    it_quiesce_reaper();
    struct it_utq_taskobj t0, t1;
    int ok = it_utq_t(&t0);
    const char *why = "thread ceiling";

    if (ok && t0.tcb_registry_exhaustions != 0u) {
        it_fz_note("T326", t0.tcb_registry_exhaustions, 0u, 0u);
        ok = 0; why = "a thread was refused for lack of registry space";
    }

    long tids[T326_THREADS];
    uint32_t made = 0;
    g_t326_ran = 0;
    for (uint32_t i = 0; ok && i < T326_THREADS; i++) {
        tids[i] = it_thread_create((uint64_t)(uintptr_t)t326_body,
                                   ((uint64_t)(uintptr_t)(g_t326_stacks[i] +
                                       sizeof(g_t326_stacks[i]))) & ~0xFULL, 0);
        if (tids[i] < 0) break;
        made++;
    }
    if (ok && made != T326_THREADS) { ok = 0; why = "thread create"; }
    for (int i = 0; ok && i < 6000 && g_t326_ran < made; i++) (void)it_sys0(SYS_YIELD);
    if (ok && g_t326_ran != made) { ok = 0; why = "a thread never ran"; }

    for (uint32_t i = 0; i < made; i++) (void)it_invoke0(tids[i], INV_TCB_EXIT);
    it_quiesce_reaper();

    if (ok && !it_utq_t(&t1)) { ok = 0; why = "query"; }
    if (ok && t1.tcb_registry_exhaustions != 0u) {
        ok = 0; why = "the ceiling came back";
    }
    /* Scheduler membership ends at TERMINATION, so the count returns exactly. */
    if (ok && t1.tcb_registry_active != t0.tcb_registry_active) {
        it_fz_note("T326", t1.tcb_registry_active, t0.tcb_registry_active, made);
        ok = 0; why = "registry drift";
    }
    if (ok) it_pass("T326"); else it_fail("T326", why);
}

/* ── T327: time and priority are authorities you are GRANTED (A-20) ─────────
 * Three holes an audit against seL4's actual API found, and this is the gauge
 * that stops them coming back.
 *
 * 1. `SYS_SC_CONFIGURE` needs the SCHEDCONTROL capability.  seL4 hands the
 *    root task one per core and `seL4_SchedControl_Configure` is the only way
 *    a budget reaches a scheduling context: holding the SC says WHICH one to
 *    configure, holding this says you may configure one at all.  IRIS needed
 *    only RIGHT_WRITE on the SC, so anyone who could retype one out of an
 *    Untyped they held could grant themselves any budget over any period.
 *
 * 2. `SYS_TCB_SET_PRIORITY` refuses a priority above the AUTHORITY's ceiling.
 *    seL4 bounds it by the authority thread's MCP so priority is delegated
 *    downward and never invented; IRIS took no authority and no bound, and a
 *    holder of any TCB capability could set 255 and starve the system.
 *
 * 3. `SYS_THREAD_PRIORITY` is RETIRED.  It set the caller's own priority for
 *    the asking — ambient authority on the scheduler, the same shape as the
 *    SELF syscalls (A-18) — and answers NOT_SUPPORTED.
 * Invariants: A1, A5, S1. */
void test_t327(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "granted time and priority";

    /* ── 1. a budget without the authority ── */
    long sc = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                   IRIS_KOBJ_SCHED_CONTEXT, 0);
    if (sc < 0) { it_fail("T327", "sc"); return; }

    if (ok && it_invoke(sc, INV_SC_CONFIGURE, 10, 100, 0L)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "budget with no authority"; }
    /* ...and a capability that is not the SchedControl does not stand in for
     * it, however much else it authorises. */
    if (ok && it_invoke(sc, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_DEBUG_CONTROL)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "wrong authority accepted"; }
    if (ok && it_invoke(sc, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) {
        ok = 0; why = "the granted authority was refused";
    }

    /*
     * ── 2. a priority above the authority's ceiling ──
     *
     * The bound is proved with an authority whose ceiling is ZERO: a retyped
     * but UNCONFIGURED TCB grants nothing, because a thread's ceiling is the
     * one its configurer had and it has not been configured.  Any priority
     * above 0 named through it must be refused, and that is the bound doing
     * its job rather than a value happening to fit.
     *
     * The target is a FRESH thread, never this one.  Lowering the suite
     * thread's own priority below the helpers it has running is how this test
     * hung the first time it was written: it stopped being scheduled and never
     * reported.  A test that can starve its own reporter is not measuring
     * authority, it is measuring luck.
     */
    long victim = ok ? it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                            IRIS_KOBJ_TCB, 0) : -1;
    long zero_auth = ok ? it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                               IRIS_KOBJ_TCB, 0) : -1;
    if (ok && (victim < 0 || zero_auth < 0)) { ok = 0; why = "tcbs"; }

    if (ok && it_invoke2(victim, INV_TCB_SET_PRIORITY, 1, zero_auth)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "a priority above the authority's ceiling was granted";
    }
    /* ...and 0 is within even that ceiling, so the refusal is the BOUND and
     * not the authority being rejected outright. */
    if (ok && it_invoke2(victim, INV_TCB_SET_PRIORITY, 0, zero_auth) != 0) {
        ok = 0; why = "a priority within the ceiling was refused";
    }
    /* This thread's own ceiling is what its spawner had, so it can still grant
     * what it holds — the bound delegates downward, it does not forbid. */
    if (ok && it_invoke2(victim, INV_TCB_SET_PRIORITY, 128, 0L) != 0) {
        ok = 0; why = "own ceiling did not authorise";
    }
    /* An authority that is not a TCB is not an authority. */
    if (ok && it_invoke2(victim, INV_TCB_SET_PRIORITY, 100, (long)IRIS_CPTR_TEST_FIX_A) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "a non-TCB authority was accepted";
    }
    if (victim >= 0)    it_slot_delete((uint32_t)victim);
    if (zero_auth >= 0) it_slot_delete((uint32_t)zero_auth);

    /* ── 3. the ambient call is gone ── */
    if (ok && it_sys1(SYS_THREAD_PRIORITY, 255)
              != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "a thread set its own priority for the asking";
    }

    it_slot_delete((uint32_t)sc);
    it_quiesce_reaper();
    if (ok) it_pass("T327"); else it_fail("T327", why);
}

void test_t328(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "address spaces are named from a pool";

    /* The pool init minted us.  Not the CONTROL — carving a pool is a
     * different grant, and T251 witnesses that this suite is refused it. */
    if (it_invoke0((long)IRIS_CPTR_ASID_POOL, INV_CAP_IDENTIFY) !=
        (long)IRIS_HANDLE_TYPE_ASID_POOL) {
        it_fail("T328", "no pool granted"); return;
    }

    long vs  = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_VSPACE, 4096);
    long cs  = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_CNODE, 4);
    long tcb = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_TCB, 0);
    if (vs < 0 || cs < 0 || tcb < 0) { it_fail("T328", "objects"); return; }

    /* 1. unnamed, so unusable. */
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, cs, vs)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "unnamed vspace accepted";
    }

    /* 4. the authority is a capability, checked as one — before the assign
     *    that succeeds, so a pass here cannot be an already-named space. */
    if (ok && it_invoke1((long)IRIS_CPTR_DEBUG_CONTROL, INV_ASID_POOL_ASSIGN, vs)
              != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "non-pool accepted as pool";
    }
    if (ok) {
        long ro = it_cs_reduce((long)IRIS_CPTR_ASID_POOL, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "reduce"; }
        else if (it_invoke1(ro, INV_ASID_POOL_ASSIGN, vs)
                 != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "read-only pool issued a name";
        }
    }
    /* ...and so is the space: naming something that is not one is refused. */
    if (ok && it_invoke1((long)IRIS_CPTR_ASID_POOL, INV_ASID_POOL_ASSIGN, cs)
              != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "cnode named as a vspace";
    }

    /* 2. named, so bindable. */
    if (ok && it_invoke1((long)IRIS_CPTR_ASID_POOL, INV_ASID_POOL_ASSIGN, vs) != 0) {
        ok = 0; why = "assign refused";
    }
    /* 3. and named once. */
    if (ok && it_invoke1((long)IRIS_CPTR_ASID_POOL, INV_ASID_POOL_ASSIGN, vs)
              != (long)IRIS_ERR_ALREADY_EXISTS) {
        ok = 0; why = "renamed a live space";
    }
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, cs, vs) != 0) {
        ok = 0; why = "named vspace refused";
    }

    /* 5. more address spaces than the pool has identifiers, one at a time.
     *    Every round destroys the previous space by deleting its capability,
     *    so the only way past round KASID_POOL_SIZE is the name coming back. */
    uint32_t made = 0;
    it_slot_delete(T328_SLOT_VS);
    for (uint32_t i = 0; ok && i < T328_ROUNDS; i++) {
        /* One STABLE slot, reused: the rotating object pool is a fixed number
         * of leaves and 136 rounds would wrap it, which T324 measures and
         * would rightly call an eviction storm.  What is being proved here is
         * about identifiers, not about slots. */
        if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_VSPACE,
                          T328_SLOT_VS, 1u, 4096) != 0) {
            ok = 0; why = "vspace carve"; break;
        }
        if (it_invoke1((long)IRIS_CPTR_ASID_POOL, INV_ASID_POOL_ASSIGN, (long)T328_SLOT_VS) != 0) {
            ok = 0; why = "pool ran dry"; break;
        }
        made++;
        it_slot_delete(T328_SLOT_VS);
        it_quiesce_reaper();
    }
    if (ok && made < T328_ROUNDS) { ok = 0; why = "identifiers not returned"; }

    /* Give the leaves back: the rotating pool is a measured resource (T324)
     * and a test that keeps three of them forever is a test that spends
     * somebody else's headroom. */
    it_slot_delete(T328_SLOT_VS);
    if (tcb > 0) it_slot_delete((uint32_t)tcb);
    if (cs  > 0) it_slot_delete((uint32_t)cs);
    if (vs  > 0) it_slot_delete((uint32_t)vs);
    it_quiesce_reaper();

    it_fz_note("T328", made, T328_ROUNDS, 0);
    if (ok) it_pass("T328"); else it_fail("T328", why);
}

void test_t329(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "faults are IPC";

    /* ONE endpoint, two children, two badges. */
    long fep = it_ep_create();
    if (fep < 0) { it_fail("T329", "fault ep"); return; }

    handle_id_t cmd_a = HANDLE_INVALID, cmd_b = HANDLE_INVALID;
    handle_id_t pa = HANDLE_INVALID, pb = HANDLE_INVALID;
    long ea = it_ep_create(), eb = it_ep_create();
    if (ea < 0 || eb < 0) { it_fail("T329", "cmd eps"); return; }
    cmd_a = (handle_id_t)ea; cmd_b = (handle_id_t)eb;
    if (lp_spawn_child(cmd_a, &pa) < 0 || lp_spawn_child(cmd_b, &pb) < 0) {
        it_fail("T329", "spawn"); return;
    }

    /* 3. armed through copies badged 1 and 2 — one endpoint, two clients. */
    for (uint32_t i = 0; ok && i < 2u; i++) {
        long bep = it_cs_badge(fep, RIGHT_READ | RIGHT_WRITE, i + 1u);
        if (bep < 0) { ok = 0; why = "badge"; break; }
        if (it_invoke(it_child_tcb((long)(i ? pb : pa)), INV_TCB_SET_FAULT_HANDLER, bep, 0, 0) != 0) {
            ok = 0; why = "arm";
        }
        it_slot_delete((uint32_t)bep);
    }

    /* 4. RIGHT_READ is what takes delivery.  A write-only copy of the very
     *    same endpoint — enough to ARM a thread's faults — cannot receive one. */
    if (ok) {
        long wo = it_cs_reduce(fep, RIGHT_WRITE);
        struct iris_msg m;
        iris_msg_zero(&m);
        if (wo < 0) { ok = 0; why = "write-only copy"; }
        else if ((m.reply = 0L, iris_msg_nb_recv(wo, &m))
                 != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "write-only cap received a fault";
        }
        if (wo >= 0) it_slot_delete((uint32_t)wo);
    }

    /* 1. Both fault; both messages arrive here, labelled and badged. */
    if (ok && it_lp_cmd_va(cmd_a, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd a"; }
    if (ok && !it_fault_wait_ep(fep, T329_LEAF_A)) { ok = 0; why = "no delivery a"; }
    uint64_t badge_a = g_it_fault_badge[T329_LEAF_A];
    if (ok && g_it_fault_label[T329_LEAF_A] != (uint64_t)FAULT_MSG_NOTIFY) {
        ok = 0; why = "unlabelled";
    }
    if (ok && it_lp_cmd_va(cmd_b, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd b"; }
    if (ok && !it_fault_wait_ep(fep, T329_LEAF_B)) { ok = 0; why = "no delivery b"; }
    uint64_t badge_b = g_it_fault_badge[T329_LEAF_B];

    /* 3, asserted: the two are told apart, and by the badges their supervisor
     *    chose rather than by where a capability happened to land. */
    if (ok && (badge_a != 1u || badge_b != 2u)) { ok = 0; why = "badges not distinct"; }

    struct it_fault fa, fb;
    if (ok && (it_fault_info(T329_LEAF_A, &fa) != 0 ||
               it_fault_info(T329_LEAF_B, &fb) != 0)) { ok = 0; why = "records"; }
    if (ok && (fa.vector != 14u || fb.vector != 14u)) { ok = 0; why = "vector"; }
    if (ok && fa.task_id == fb.task_id) { ok = 0; why = "same thread twice"; }

    /* 5. the retired mechanisms, from a task holding every capability there is
     *    to hold about these threads. */
    {
        uint8_t buf[FAULT_MSG_LEN];
        if (ok && it_sys2(SYS_TCB_FAULT_INFO, it_child_tcb((long)pa),
                          (long)(uintptr_t)buf) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "fault info still answers";
        }
        if (ok && it_sys2(SYS_EXCEPTION_RESUME, it_child_tcb((long)pa), 1)
                  != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "exception resume still answers";
        }
    }

    /* 2. the reply is the authority, and it is the only one: a thread
     *    capability with every right on it resumes nothing. */
    if (ok) {
        struct iris_msg rm;
        iris_msg_zero(&rm);
        if (iris_msg_reply(it_child_tcb((long)pa), &rm)
            != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "tcb answered a fault"; }
    }
    if (ok && it_fault_kill(T329_LEAF_A) != 0) { ok = 0; why = "answer a"; }
    if (ok && it_fault_kill(T329_LEAF_B) != 0) { ok = 0; why = "answer b"; }
    if (ok && it_lp_wait_exit(pa) != 0) { ok = 0; why = "exit a"; }
    if (ok && it_lp_wait_exit(pb) != 0) { ok = 0; why = "exit b"; }

    it_close(&pa); it_close(&pb);
    it_close(&cmd_a); it_close(&cmd_b);
    { handle_id_t fh = (handle_id_t)fep; it_close(&fh); }
    it_quiesce_reaper();
    if (ok) it_pass("T329"); else it_fail("T329", why);
}


/* ── T330: a bound notification reaches a thread blocked on an endpoint ─────
 *
 * Ledger A-23, seL4's `seL4_TCB_BindNotification`, and the gap A-20's audit
 * found: *"seL4 binds a notification to a TCB so a passive server blocked on
 * an endpoint can still take signals; IRIS cannot."*
 *
 * A thread blocked receiving on an endpoint is in that endpoint's queue, and
 * nothing else can reach it.  Every server that needs BOTH an interrupt and a
 * request queue — which is what a driver is — therefore had to spend a second
 * thread on the choice, or busy-poll.  There is no way to write a
 * single-threaded driver without this, which is why the timer service (A-24)
 * is the first thing that could not be written at all.
 *
 * Five claims:
 *  1. binding is capability-mediated: RIGHT_WRITE on the thread, RIGHT_WRITE
 *     on the notification, and the notification slot takes nothing else;
 *  2. one thread per notification and one notification per thread — a second
 *     bind either way is ALREADY_EXISTS, because "which thread does a signal
 *     wake" must have exactly one answer;
 *  3. a signal ALREADY pending is consulted on the way into a receive, so a
 *     signal that arrives before the thread blocks is not lost;
 *  4. it arrives as a message the server can tell apart — labelled
 *     IRIS_MSG_LABEL_NOTIFICATION, bits in words[0] — because one thread now
 *     receives two kinds of thing on one syscall;
 *  5. unbinding restores the deafness, which is what says the binding was
 *     doing the work.
 * Invariants: A1, I1. */
void test_t330(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "bound notification";

    long ep = it_ep_create();
    long n1 = it_notify_create();
    long n2 = it_notify_create();
    if (ep < 0 || n1 < 0 || n2 < 0) { it_fail("T330", "objects"); return; }
    long self = it_own_tcb_derived();
    if (self < 0) { it_fail("T330", "self tcb"); return; }

    /* 1. the arguments are capabilities, checked as such. */
    if (ok && it_invoke1(self, INV_TCB_BIND_NOTIFICATION, ep)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "endpoint bound as notification"; }
    if (ok && it_invoke1(n1, INV_TCB_BIND_NOTIFICATION, n1)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "notification bound as thread"; }
    if (ok) {
        long ro = it_cs_reduce(n1, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "ro dup"; }
        else if (it_invoke1(self, INV_TCB_BIND_NOTIFICATION, ro)
                 != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read-only bound"; }
        if (ro >= 0) it_slot_delete((uint32_t)ro);
    }

    /* ...and the bind itself. */
    if (ok && it_invoke1(self, INV_TCB_BIND_NOTIFICATION, n1) != 0) { ok = 0; why = "bind"; }

    /* 2. exactly one answer, in both directions. */
    if (ok && it_invoke1(self, INV_TCB_BIND_NOTIFICATION, n2)
              != (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "second notification bound"; }
    if (ok) {
        long other = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                          IRIS_KOBJ_TCB, 0);
        if (other < 0) { ok = 0; why = "other tcb"; }
        else if (it_invoke1(other, INV_TCB_BIND_NOTIFICATION, n1)
                 != (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "second thread bound"; }
        if (other >= 0) it_slot_delete((uint32_t)other);
    }

    /* 3 + 4. signal FIRST, then receive: the pending signal is consulted on the
     *        way in, and arrives labelled. */
    if (ok && it_invoke1(n1, INV_NOTIFY_SIGNAL, 0x5u) != 0) { ok = 0; why = "signal"; }
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        if ((m.reply = 0L, iris_msg_recv(ep, &m)) != 0) {
            ok = 0; why = "recv did not take the signal";
        } else if (m.label != IRIS_MSG_LABEL_NOTIFICATION) {
            ok = 0; why = "signal not labelled";
        } else if (m.words[0] != 0x5u) {
            ok = 0; why = "wrong bits";
        }
    }

    /* 5. unbind, and the same signal no longer reaches a receive — the bits
     *    stay pending on the notification for whoever waits on it directly. */
    if (ok && it_invoke1(self, INV_TCB_BIND_NOTIFICATION, 0L) != 0) { ok = 0; why = "unbind"; }
    if (ok && it_invoke1(n1, INV_NOTIFY_SIGNAL, 0x9u) != 0) { ok = 0; why = "signal 2"; }
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        if ((m.reply = 0L, iris_msg_nb_recv(ep, &m))
            != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "unbound thread still took it"; }
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_wait_timeout( n1, (long)(uintptr_t)&bits,
                    100000000LL) != 0 || bits != 0x9u) {
            ok = 0; why = "bits lost by the unbind";
        }
    }

    it_slot_delete((uint32_t)self);
    { handle_id_t h;
      h = (handle_id_t)n2; it_close(&h);
      h = (handle_id_t)n1; it_close(&h);
      h = (handle_id_t)ep; it_close(&h); }
    it_quiesce_reaper();
    if (ok) it_pass("T330"); else it_fail("T330", why);
}


/* ── T331: waiting is a service, not a syscall (A-24) ───────────────────────
 *
 * `SYS_SLEEP`, `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` each parked
 * a thread with a deadline and had the scheduler wake it.  That is a policy
 * about time inside the kernel — how long a thread may wait, whose waiting is
 * worth a kernel data structure, what happens when the deadline passes — and
 * seL4 has none of it, for exactly that reason.
 *
 * So a task that wants to wait ASKS somebody.  Four claims:
 *
 *  1. the timer service signals the notification it was handed, after the
 *     delay it was given, and not before;
 *  2. the authority is the ENDPOINT: a task holding no timer capability cannot
 *     wait on time at all, which is the difference between a service and a
 *     syscall number;
 *  3. the notification travels as a CAPABILITY and the grant ENDS with the
 *     timer — the service is not left holding a way to signal a client it
 *     finished serving;
 *  4. the three retired syscalls answer NOT_SUPPORTED.
 * Invariants: A1, A5, P2. */
void test_t331(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "waiting is a service";

    /* 2. the authority is the endpoint we were granted. */
    if (ok && it_invoke0((long)IRIS_CPTR_TIMER_EP, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_ENDPOINT) { ok = 0; why = "no timer granted"; }

    long n = it_notify_create();
    if (n < 0) { it_fail("T331", "notif"); return; }

    /* 1. armed, and it fires.  A generous delay compared with the tick (10 ms)
     *    so the assertion is about the mechanism and not about scheduling
     *    luck; the wait below is unbounded on purpose, because a timer service
     *    that never fires SHOULD hang the suite rather than let a broken
     *    mechanism pass as a timeout. */
    /* A fresh copy per arm, dropped as soon as the arm lands: the transfer is
     * a COPY (A-29), so "giving it away" is deriving a capability and then
     * deleting your own slot.  What the service keeps is a derivation CHILD —
     * which is also how claim 3 below can be checked at all. */
    if (ok) {
        long give = it_cs_reduce(n, RIGHT_WRITE | RIGHT_TRANSFER);
        uint64_t tok = 0;
        long ar = (give < 0) ? -999 :
                  iris_timer_arm((long)IRIS_CPTR_TIMER_EP, give, 0x4ull, 50000000ull, &tok);
        it_xfer_release(give);
        if (ar != 0) {
            it_serial_write("[IRIS][TEST] T331 arm give="); it_log_num((uint32_t)give);
            it_serial_write(" r="); it_log_num((uint32_t)-ar); it_serial_write("\n");
            ok = 0; why = "arm";
        }
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0) {
            ok = 0; why = "wait";
        } else if ((bits & 0x4ull) == 0) {
            ok = 0; why = "wrong bits";
        }
    }

    /* ...and it did not fire EARLY: a second arm with a long delay leaves the
     *    notification quiet for a while. */
    if (ok) {
        long give = it_cs_reduce(n, RIGHT_WRITE | RIGHT_TRANSFER);
        uint64_t tok = 0;
        if (give < 0 || iris_timer_arm((long)IRIS_CPTR_TIMER_EP, give, 0x8ull,
                                       2000000000ull, &tok) != 0) { ok = 0; why = "arm long"; }
        it_xfer_release(give);
    }
    if (ok) {
        uint64_t bits = 0;
        for (uint32_t i = 0; ok && i < 20u; i++) {
            if (it_invoke1(n, INV_NOTIFY_POLL, (long)(uintptr_t)&bits) == 0 && bits) {
                ok = 0; why = "fired early"; break;
            }
            (void)it_sys1(SYS_YIELD, 0);
        }
    }

    /* 3. an arm needs a notification to signal; without one it is refused, and
     *    the service is left holding nothing. */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label      = TMR_OP_ARM;
        m.words[0]   = 1000ull;
        m.words[1]   = 1ull;
        m.word_count = 2u;
        if (iris_msg_call((long)IRIS_CPTR_TIMER_EP, &m) != 0) {
            ok = 0; why = "capless call";
        } else if (m.words[0] == 0u) {
            ok = 0; why = "armed with no notification";
        }
    }

    /* 4. the syscalls are gone. */
    if (ok && it_sys1(SYS_SLEEP, 1) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "SYS_SLEEP still answers";
    }
    if (ok && it_sys3(SYS_CLOCK_NANOSLEEP, 0, 1000, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "SYS_CLOCK_NANOSLEEP still answers";
    }
    {
        uint64_t bits = 0;
        if (ok && it_sys3(SYS_NOTIFY_WAIT_TIMEOUT, (long)IRIS_CPTR_TIMER_EP,
                          (long)(uintptr_t)&bits, 1000L)
                  != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "SYS_NOTIFY_WAIT_TIMEOUT still answers";
        }
    }

    { handle_id_t h = (handle_id_t)n; it_close(&h); }
    it_quiesce_reaper();
    if (ok) it_pass("T331"); else it_fail("T331", why);
}


/* T332's senders: three threads that BLOCK in a send, which is the only way to
 * be queued on an endpoint and therefore the only way to have a tail. */
static long     g_t332_ep_a, g_t332_ep_b;
static long     g_t332_tcb[3];
static uint8_t  g_t332_stacks[3][4096];
static volatile int  g_t332_done, g_t332_done_b;
static volatile long g_t332_err_a;

static void t332_sender_a(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = 0x332;
    long r = iris_msg_send(g_t332_ep_a, &m);
    g_t332_err_a = r;
    __atomic_fetch_add((int *)&g_t332_done, 1, __ATOMIC_RELAXED);
    for (;;) (void)it_sys1(SYS_YIELD, 0);
}

static void t332_sender_b(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = 0x332;
    (void)iris_msg_send(g_t332_ep_b, &m);
    g_t332_done_b = 1;
    for (;;) (void)it_sys1(SYS_YIELD, 0);
}


/* ── T332: revocation with no tail (A-25) ───────────────────────────────────
 *
 * seL4's `seL4_CNode_CancelBadgedSends`, and the half of revocation IRIS was
 * missing.  Revoking a badged capability stops a client sending anything NEW.
 * It does nothing about what is already QUEUED: a message sent a moment before
 * the revoke sits in the endpoint's send queue and is delivered afterwards, to
 * a server that has just been told this client no longer exists.  Revocation
 * with a tail is not revocation.
 *
 * Four claims:
 *  1. queued sends carrying the named badge are cancelled, and the count says
 *     how many — so a supervisor can tell a revoke that had a tail from one
 *     that did not;
 *  2. sends carrying a DIFFERENT badge are untouched, which is what makes this
 *     usable at all: one client is silenced, not the endpoint;
 *  3. a cancelled sender learns its send did not happen (CLOSED), rather than
 *     believing it was delivered;
 *  4. the capability must be UNBADGED.  A badged one names one client, and
 *     cancelling through it would let that client silence any other by naming
 *     their number — the same reason a badge can never be re-badged.
 * Invariants: A8, A10. */
void test_t332(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "cancel badged sends";

    long ep = it_ep_create();
    if (ep < 0) { it_fail("T332", "ep"); return; }

    /* Two clients, two badges, on one endpoint. */
    long b1 = it_cs_badge(ep, RIGHT_READ | RIGHT_WRITE, 0x11u);
    long b2 = it_cs_badge(ep, RIGHT_READ | RIGHT_WRITE, 0x22u);
    if (b1 < 0 || b2 < 0) { it_fail("T332", "badges"); return; }

    /* 4. a badged capability cannot cancel by badge. */
    if (ok && it_invoke1(b1, INV_EP_CANCEL_BADGED_SENDS, 0x22u)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "badged cap cancelled"; }
    /* ...and it takes RIGHT_WRITE on the endpoint. */
    if (ok) {
        long ro = it_cs_reduce(ep, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "ro dup"; }
        else if (it_invoke1(ro, INV_EP_CANCEL_BADGED_SENDS, 0x11u)
                 != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read-only cancelled"; }
        if (ro >= 0) it_slot_delete((uint32_t)ro);
    }

    /* Nothing queued: cancelling is a clean zero, not an error. */
    if (ok && it_invoke1(ep, INV_EP_CANCEL_BADGED_SENDS, 0x11u) != 0) {
        ok = 0; why = "empty queue not zero";
    }

    /* Queue two senders under badge 0x11 and one under 0x22.  Threads,
     * because a blocking send is the only way to BE queued. */
    if (ok) {
        g_t332_ep_a = b1; g_t332_ep_b = b2; g_t332_done = 0;
        for (uint32_t i = 0; ok && i < 3u; i++) {
            long tcb = it_thread_create((uint64_t)(uintptr_t)
                                        (i < 2u ? t332_sender_a : t332_sender_b),
                                        ((uint64_t)(uintptr_t)(g_t332_stacks[i] +
                                            sizeof(g_t332_stacks[0]))) & ~0xFULL, 0);
            if (tcb < 0) { ok = 0; why = "sender thread"; }
            else g_t332_tcb[i] = tcb;
        }
        /* Let all three reach their blocking send. */
        for (uint32_t i = 0; i < 400u; i++) (void)it_sys1(SYS_YIELD, 0);
    }

    /* 1 + 2: exactly the two under 0x11 are cancelled. */
    if (ok) {
        long n = it_invoke1(ep, INV_EP_CANCEL_BADGED_SENDS, 0x11u);
        if (n != 2) { it_fz_note("T332", (uint32_t)n, 2u, 0u); ok = 0; why = "wrong cancel count"; }
    }
    /* 3: they learned it did not happen. */
    if (ok) {
        for (uint32_t i = 0; i < 400u && g_t332_done < 2; i++) (void)it_sys1(SYS_YIELD, 0);
        if (g_t332_done != 2) { ok = 0; why = "cancelled senders not woken"; }
        if (ok && g_t332_err_a != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong error"; }
    }
    /* 2, asserted: the 0x22 sender is still queued and still waiting. */
    if (ok && g_t332_done_b != 0) { ok = 0; why = "other badge cancelled too"; }
    /* ...and it can still be served. */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        if ((m.reply = 0L, iris_msg_nb_recv(ep, &m)) != 0) {
            ok = 0; why = "survivor not receivable";
        } else if (m.sender_badge != 0x22u) {
            ok = 0; why = "wrong survivor";
        }
    }

    for (uint32_t i = 0; i < 3u; i++)
        if (g_t332_tcb[i] > 0) (void)it_invoke0(g_t332_tcb[i], INV_TCB_EXIT);
    it_quiesce_reaper();
    for (uint32_t i = 0; i < 3u; i++)
        if (g_t332_tcb[i] > 0) it_slot_delete((uint32_t)g_t332_tcb[i]);
    it_slot_delete((uint32_t)b2);
    it_slot_delete((uint32_t)b1);
    { handle_id_t h = (handle_id_t)ep; it_close(&h); }
    it_quiesce_reaper();
    if (ok) it_pass("T332"); else it_fail("T332", why);
}


/* T333's victim: a thread whose registers are read while it is suspended. */
static uint8_t g_t333_stack[4096];
static volatile int g_t333_ran;

static void t333_victim(void) {
    g_t333_ran = 1;
    for (;;) (void)it_sys1(SYS_YIELD, 0);
}


/* ── T333: the five invocations seL4 has and IRIS could not express (A-28) ──
 *
 * A file-by-file re-read (A-26) found five operations with no equivalent here.
 * None was load-bearing for anything IRIS did, which is exactly why they went
 * unnoticed — an API gap only hurts when somebody reaches for it, and nobody
 * had.  Each is a thing a supervisor should be able to say and could not.
 *
 *  1. `TCB_ReadRegisters` — a supervisor could point a thread anywhere and
 *     never ask where it was.  RIGHT_READ, because observing is not changing,
 *     and refused for a RUNNING thread, whose registers are in the CPU rather
 *     than the TCB (D-1 step 3) — handing back the stale frame would be a lie
 *     a debugger acts on.
 *  2. `CNode_Move` across CNodes.  Within one, a move was a swap against an
 *     empty slot; between them the only route was mint-then-delete, which for
 *     the length of two calls records a delegation that never happened.
 *  3. `SchedContext_Consumed` — MCS could say what a context was OWED and not
 *     what it SPENT.
 *  4. `SchedContext_YieldTo`, bounded by the caller's MCP: a thread that could
 *     not raise another to a priority must not be able to schedule one already
 *     at it on demand.
 *  5. `IRQHandler_Clear` — a route could be installed and taken back only by
 *     destroying the notification it pointed at.
 * Invariants: A1, A7, S1. */
void test_t333(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "the five missing invocations";

    /* ── 1. read a thread's registers ── */
    {
        struct iris_user_ctx ctx;
        long tcb = it_thread_create((uint64_t)(uintptr_t)t333_victim,
                                    ((uint64_t)(uintptr_t)(g_t333_stack +
                                        sizeof(g_t333_stack))) & ~0xFULL, 0);
        if (tcb < 0) { it_fail("T333", "victim"); return; }
        /* Let it run and then stop it: a RUNNING thread's registers are in the
         * CPU, which is the case the syscall refuses. */
        for (uint32_t i = 0; i < 200u && !g_t333_ran; i++) (void)it_sys1(SYS_YIELD, 0);
        if (ok && it_invoke0(tcb, INV_TCB_SUSPEND) != 0) { ok = 0; why = "suspend"; }
        if (ok && it_invoke1(tcb, INV_TCB_READ_REGS, (long)(uintptr_t)&ctx) != 0) {
            ok = 0; why = "read regs";
        }
        /* It was running our victim, so its rip is inside this image and its
         * stack pointer inside the stack we gave it. */
        if (ok && (ctx.rip == 0 || ctx.rip >= 0x0000800000000000ULL)) {
            ok = 0; why = "rip not a user address";
        }
        if (ok && (ctx.rsp < (uint64_t)(uintptr_t)g_t333_stack ||
                   ctx.rsp > (uint64_t)(uintptr_t)(g_t333_stack + sizeof(g_t333_stack)))) {
            ok = 0; why = "rsp not in its stack";
        }
        /* Reading YOURSELF is refused for the same reason: the frame you would
         * read is the one this syscall entered on. */
        if (ok) {
            long self = it_own_tcb_derived();
            if (self < 0) { ok = 0; why = "self tcb"; }
            else if (it_invoke1(self, INV_TCB_READ_REGS, (long)(uintptr_t)&ctx)
                     != (long)IRIS_ERR_BUSY) { ok = 0; why = "read self"; }
            if (self >= 0) it_slot_delete((uint32_t)self);
        }
        /* RIGHT_READ is the authority, and a capability without it is refused
         * even though it may WRITE the same registers. */
        if (ok) {
            /* A ROOT scratch slot, not a rotating pool leaf: T324 measures how
             * often the pool recycles a live leaf, and a new test should not
             * spend that budget on three rights-reduced copies. */
            long wo = it_cdt_derive(tcb, IT_SCRATCH_0, RIGHT_WRITE);
            if (wo < 0) { ok = 0; why = "write-only dup"; }
            else if (it_invoke1(wo, INV_TCB_READ_REGS, (long)(uintptr_t)&ctx)
                     != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "write-only read"; }
            it_slot_delete(IT_SCRATCH_0);
        }
        (void)it_invoke0(tcb, INV_TCB_EXIT);
        it_quiesce_reaper();
        if (tcb > 0) it_slot_delete((uint32_t)tcb);
    }

    /* ── 2. move a capability BETWEEN CNodes ── */
    if (ok) {
        long n = it_notify_create();          /* something to move */
        if (n < 0) { ok = 0; why = "notif"; }
        else {
            /* The badge travels, which is the reason a move is not a mint: a
             * badged capability can never be re-badged (A8). */
            it_slot_delete(IT_SCRATCH_1);
            long src = (it_invoke2(n, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_1 << 32), (long)((uint64_t)(RIGHT_READ | RIGHT_WRITE) |
                                       (0x5Aull << 32))) == 0)
                       ? (long)IT_SCRATCH_1 : -1;
            if (src < 0) { ok = 0; why = "badged source"; }
            else {
                it_slot_delete(T333_DST_SLOT);
                if (it_invoke1(src, INV_CSPACE_MOVE, (long)((uint64_t)T333_DST_SLOT << 32)) != 0) {
                    ok = 0; why = "move";
                }
                /* The source slot is EMPTY and the destination holds it. */
                if (ok && it_invoke0(src, INV_CAP_IDENTIFY) >= 0) {
                    ok = 0; why = "source survived the move";
                }
                if (ok && it_invoke0((long)T333_DST_SLOT, INV_CAP_IDENTIFY)
                          != (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                    ok = 0; why = "destination empty";
                }
                /* ...with its badge intact. */
                if (ok) {
                    uint64_t got = 0;
                    if (it_ping_badge((long)T333_DST_SLOT, &got) == 0 && got != 0x5Au) {
                        ok = 0; why = "badge lost in the move";
                    }
                }
                /* An OCCUPIED destination is refused, not overwritten. */
                if (ok) {
                    it_slot_delete(IT_SCRATCH_2);
                    long again = (it_invoke2(n, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_2 << 32), (long)((uint64_t)(RIGHT_READ | RIGHT_WRITE) |
                                                 (0x5Bull << 32))) == 0)
                                 ? (long)IT_SCRATCH_2 : -1;
                    if (again >= 0 &&
                        it_invoke1(again, INV_CSPACE_MOVE, (long)((uint64_t)T333_DST_SLOT << 32))
                        != (long)IRIS_ERR_ALREADY_EXISTS) {
                        ok = 0; why = "move over an occupied slot";
                    }
                    it_slot_delete(IT_SCRATCH_2);
                }
                it_slot_delete(T333_DST_SLOT);
            }
            { handle_id_t h = (handle_id_t)n; it_close(&h); }
        }
    }

    /* ── 3. what a scheduling context SPENT ── */
    if (ok) {
        long sc = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                       IRIS_KOBJ_SCHED_CONTEXT, 0);
        uint64_t spent = 0;
        if (sc < 0) { ok = 0; why = "sc"; }
        else {
            if (it_invoke(sc, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "configure"; }
            /* Nothing has run on it, so nothing was spent — and the read is a
             * READ: a write-only copy cannot ask. */
            if (ok && it_invoke1(sc, INV_SC_CONSUMED, (long)(uintptr_t)&spent) != 0) {
                ok = 0; why = "consumed";
            }
            if (ok && spent != 0u) { ok = 0; why = "unused context spent time"; }
            if (ok) {
                long wo = it_cdt_derive(sc, IT_SCRATCH_0, RIGHT_WRITE);
                if (wo >= 0 && it_invoke1(wo, INV_SC_CONSUMED, (long)(uintptr_t)&spent)
                    != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "write-only consumed"; }
                it_slot_delete(IT_SCRATCH_0);
            }
            /* 4. yielding to a context with NO thread bound is INVALID_ARG —
             *    there is nobody to yield to. */
            if (ok && it_invoke1(sc, INV_SC_YIELD_TO, 0L) != (long)IRIS_ERR_INVALID_ARG) {
                ok = 0; why = "yield to an unbound context";
            }
            it_slot_delete((uint32_t)sc);
        }
    }

    /* ── 5. take an IRQ route back ── */
    if (ok) {
        /* This suite holds no IRQ capability, and that IS the assertion: the
         * authority to clear a route is the authority to install one, so a task
         * that cannot route cannot un-route either. */
        if (it_invoke0((long)IRIS_CPTR_IRQ_CAP, INV_IRQ_CLEAR) >= 0) {
            ok = 0; why = "cleared a route with no IRQ capability";
        }
        /* ...and a capability that is not an IRQ capability is refused by type,
         * not by rights. */
        if (ok && it_invoke0((long)IRIS_CPTR_TEST_UNTYPED, INV_IRQ_CLEAR)
                  != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "untyped cleared a route"; }
    }

    it_quiesce_reaper();
    if (ok) it_pass("T333"); else it_fail("T333", why);
}

static handle_id_t  g_t334_ep   = HANDLE_INVALID;
static long         g_t334_src  = -1;
static volatile int g_t334_done = 0;
static          int g_t334_res  = 999;
static uint8_t      g_t334_stack[8192];

static void t334_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x334;
    m.cap = (uint32_t)g_t334_src;
    m.cap_rights = RIGHT_WRITE;
    g_t334_res  = (int)iris_msg_send((long)g_t334_ep, &m);
    g_t334_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* One transfer: derive a fresh source, hand it over from another thread, and
 * receive it into T334_DST_SLOT.  Leaves both slots occupied on success —
 * which is the whole point, and what the caller then interrogates. */
static int t334_transfer(long n, const char **why) {
    g_t334_done = 0;
    g_t334_res  = 999;
    it_slot_delete(T334_DST_SLOT);
    g_t334_src = it_cdt_derive(n, T334_SRC_SLOT, RIGHT_WRITE | RIGHT_TRANSFER);
    if (g_t334_src < 0) { *why = "derive source"; return 0; }

    uint64_t entry = (uint64_t)(uintptr_t)t334_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t334_stack + sizeof(g_t334_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) { *why = "thread"; return 0; }

    struct iris_msg m;
    iris_msg_zero(&m);
    m.recv_slot = T334_DST_SLOT;          /* declared receive slot */
    if (iris_msg_recv((long)g_t334_ep, &m) != 0) { *why = "recv"; return 0; }
    if (m.got_cap != T334_DST_SLOT)  { *why = "landing"; return 0; }
    for (int i = 0; i < 400 && !g_t334_done; i++) it_settle(1);
    if (!g_t334_done || g_t334_res != 0)     { *why = "send"; return 0; }
    return 1;
}

void test_t334(void) {
    it_quiesce_reaper();
    uint32_t before[6], after[6];
    if (!it_sched_ext3(before)) { it_fail("T334", "sched ext3"); return; }
    int ok = 1;
    const char *why = "transfer is a copy";

    long n  = it_notify_create_slot();
    long ep = it_ep_create_slot();
    if (n < 0 || ep < 0) { it_fail("T334", "create"); return; }
    g_t334_ep = (handle_id_t)ep;

    /* ── 1. the sender still holds what it sent ── */
    if (ok && !t334_transfer(n, &why)) ok = 0;
    if (ok && it_invoke0((long)T334_SRC_SLOT, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
        ok = 0; why = "sender lost its capability";
    }
    /* ...and it is a capability, not a husk: signal through it and observe on
     * the master.  A slot that resolves but cannot act would pass the check
     * above and mean nothing. */
    if (ok && it_invoke1((long)T334_SRC_SLOT, INV_NOTIFY_SIGNAL, 0x1u) != 0) {
        ok = 0; why = "sender's capability is dead";
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_POLL, (long)(uintptr_t)&bits) != 0 ||
            (bits & 0x1u) == 0u) { ok = 0; why = "signal did not reach the object"; }
    }
    /* The receiver's copy is the same object, reached from its own slot. */
    if (ok && it_invoke1((long)T334_DST_SLOT, INV_NOTIFY_SIGNAL, 0x2u) != 0) {
        ok = 0; why = "delivered capability is dead";
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_POLL, (long)(uintptr_t)&bits) != 0 ||
            (bits & 0x2u) == 0u) { ok = 0; why = "delivered capability is not the object"; }
    }

    /* ── 2. the delivered capability is a CHILD of the sender's slot ── */
    if (ok && it_cdt_revoke((long)T334_SRC_SLOT) < 0) { ok = 0; why = "revoke"; }
    if (ok && it_cdt_alive((long)T334_DST_SLOT)) {
        ok = 0; why = "the delivered capability was not a child";
    }
    /* Revoke takes the descendants and leaves the invoked slot: the sender is
     * still holding its own capability afterwards. */
    if (ok && it_invoke0((long)T334_SRC_SLOT, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
        ok = 0; why = "revoke ate the slot it was invoked on";
    }
    it_slot_delete(T334_SRC_SLOT);

    /* ── 3. giving it away: send, then delete ── */
    if (ok && !t334_transfer(n, &why)) ok = 0;
    if (ok) it_slot_delete(T334_SRC_SLOT);
    if (ok && it_cdt_alive((long)T334_SRC_SLOT)) { ok = 0; why = "delete kept the slot"; }
    /* Deleting a parent is not revoking it — the receiver keeps what it was
     * given, and can still act with it. */
    if (ok && !it_cdt_alive((long)T334_DST_SLOT)) {
        ok = 0; why = "deleting the sender's slot took the receiver's copy";
    }
    if (ok && it_invoke1((long)T334_DST_SLOT, INV_NOTIFY_SIGNAL, 0x4u) != 0) {
        ok = 0; why = "the given-away capability stopped working";
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_POLL, (long)(uintptr_t)&bits) != 0 ||
            (bits & 0x4u) == 0u) { ok = 0; why = "given-away signal lost"; }
    }

    it_slot_delete(T334_DST_SLOT);
    it_slot_delete(T334_SRC_SLOT);
    { handle_id_t h = (handle_id_t)n; it_close(&h); }
    it_close(&g_t334_ep);
    it_quiesce_reaper();

    /* Nothing was left alive by either round. */
    if (ok && !it_sched_ext3(after)) { ok = 0; why = "sched ext3 final"; }
    for (uint32_t i = 0; ok && i < 6u; i++)
        if (after[i] != before[i]) { ok = 0; why = "object leak"; }

    if (ok) it_pass("T334"); else it_fail("T334", why);
}

/* ── T335: a wrong type is answered as a wrong type (A-30) ──────────────────
 *
 * Twenty-two resolver results used to be rewritten on their way out: sixteen
 * `WRONG_TYPE → INVALID_ARG`, three `WRONG_TYPE → ACCESS_DENIED`, and three
 * ternaries that mapped WRONG_TYPE to itself — the residue of a conversion
 * that was done three separate times and never finished (A-20's
 * type-before-rights fix, D-5's `dev_cap_budget`, and the TCB family at Step
 * 4).  `SYS_TCB_SET_IPC_BUFFER` was the clearest symptom: one call answering
 * WRONG_TYPE for a bad arg0 and INVALID_ARG for a bad arg1, for the same kind
 * of mistake.
 *
 * The rule this pins is one sentence, and the boundary is the point of it:
 *
 *   a capability of the WRONG TYPE is `WRONG_TYPE`;
 *   a capability of the RIGHT type without the authority is `ACCESS_DENIED`.
 *
 * Flattening protected nothing.  A caller can already ask `SYS_CAP_IDENTIFY`
 * about any slot it holds, with `RIGHT_NONE` and no capability spent, and
 * every one of these resolutions runs against the caller's OWN CSpace — so
 * "that is a notification, not a frame" tells a caller something it can read
 * for itself, while INVALID_ARG told it something less than the kernel knew.
 * Invariants: A1, A7. */
void test_t335(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a wrong type is answered as a wrong type";

    /* Fixed root scratch slots, not the rotating object pool: four capabilities
     * held across one test is four leaves the pool cannot hand out, and T324
     * counts exactly that. */
    const long UT = (long)IRIS_CPTR_TEST_UNTYPED;   /* an Untyped */
    it_slot_delete(IT_SCRATCH_0); it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_2); it_slot_delete(IT_SCRATCH_3);
    long ep = it_retype2_at(UT, IRIS_KOBJ_ENDPOINT,      IT_SCRATCH_0, 1u, 0);
    long nt = it_retype2_at(UT, IRIS_KOBJ_NOTIFICATION,  IT_SCRATCH_1, 1u, 0);
    long fr = it_retype2_at(UT, IRIS_KOBJ_FRAME,         IT_SCRATCH_2, 1u, 4096);
    long sc = it_retype2_at(UT, IRIS_KOBJ_SCHED_CONTEXT, IT_SCRATCH_3, 1u, 0);
    if (ep != 0 || nt != 0 || fr != 0 || sc != 0) { it_fail("T335", "fixtures"); return; }
    ep = (long)IT_SCRATCH_0; nt = (long)IT_SCRATCH_1;
    fr = (long)IT_SCRATCH_2; sc = (long)IT_SCRATCH_3;


    /* ── the sixteen that said INVALID_ARG ── */

    /* scheduling: the context argument, and the one THREAD_SET_SC takes. */
    if (ok && it_invoke(ep, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "sc_configure"; }
    if (ok && it_invoke0(ep, INV_SC_SET_ON_CALLER) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "thread_set_sc";
    }
    if (ok && it_invoke1(sc, INV_SC_BIND, ep) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "sc_bind (the TCB argument)";
    }
    if (ok && it_invoke1(ep, INV_SC_BIND, (long)IRIS_CPTR_OWN_TCB)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "sc_bind (the SC argument)"; }

    /* threads: the CSpace and VSpace a TCB is configured with, its IPC frame,
     * and the notification a watch signals. */
    if (ok && it_invoke2((long)IRIS_CPTR_OWN_TCB, INV_TCB_SET_IPC_BUFFER, UT, 0x8000600000L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "set_ipc_buffer frame"; }
    if (ok && it_invoke2((long)IRIS_CPTR_OWN_TCB, INV_TCB_WATCH, ep, 1)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "tcb_watch notification"; }

    /* CNodes: the CNode a delete or a swap is invoked on. */
    if (ok && it_invoke2(ep, INV_CNODE_DELETE, 1, 0) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "cnode_delete";
    }
    if (ok && it_invoke2(ep, INV_CNODE_SWAP, 1, 2) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "cnode_swap";
    }

    /* CSpace: the destination CNode of a mint and of a move. */
    if (ok && it_invoke2(nt, INV_CSPACE_MINT, IT_MINT_INTO(ep, 1u), (long)RIGHT_READ)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "mint destination"; }
    if (ok && it_invoke1(nt, INV_CSPACE_MOVE, IT_MINT_INTO(ep, 1u))
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "move destination"; }

    /* Untyped: the destination CNode of a retype, and the budget arguments. */
    if (ok && it_invoke(UT, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_ENDPOINT | (1ULL << 32)), (long)((uint64_t)(uint32_t)ep | (1ULL << 32)), 0)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "retype destination"; }
    if (ok && it_invoke1(ep, INV_UNTYPED_SET_DEVICE_BUDGET, UT)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "device budget (device)"; }
    if (ok && it_invoke1(UT, INV_UNTYPED_SET_DEVICE_BUDGET, ep)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "device budget (ram)"; }

    /* Virtual memory: the page table and the address space it is hung in. */
    if (ok && it_invoke2(ep, INV_PAGE_TABLE_MAP, IT_VS, 0x8000700000L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "map_table page table"; }
    if (ok && it_invoke2(fr, INV_PAGE_TABLE_MAP, ep, 0x8000700000L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "map_table vspace"; }

    /* ── the three that said ACCESS_DENIED ──
     * An authority argument is still a capability with a type.  Presenting a
     * notification where a bootstrap capability belongs is not a failed
     * authority check — the check never ran. */
    if (ok && it_invoke2(nt, INV_BOOT_INITRD_COUNT, 0, 0) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "initrd_count authority";
    }
    if (ok) {
        struct iris_fb_params fb;
        if (it_invoke2(nt, INV_BOOT_FRAMEBUFFER_INFO, (long)(uintptr_t)&fb, 0)
            != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "framebuffer authority"; }
    }

    /* ── and the boundary the rule exists to keep ──
     * Right type, missing right: that IS an authority answer, and it does not
     * move.  Without this half the rule would read "say WRONG_TYPE more
     * often", which is not what was decided. */
    if (ok) {
        it_slot_delete((uint32_t)fr);   /* the map_table checks are done with it */
        long ro = it_cdt_derive(sc, (uint32_t)fr, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "read-only derive"; }
        if (ok && it_invoke(ro, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights answer moved"; }
    }
    /* A real bootstrap capability of the wrong FLAVOUR is likewise an
     * authority answer: the framebuffer control capability cannot read the
     * initrd, and being the right TYPE is exactly why. */
    if (ok && it_invoke2((long)IRIS_CPTR_FB_CONTROL, INV_BOOT_INITRD_COUNT, 0, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "flavour answer moved"; }

    /* ...and nothing here is a secret: the caller can name every one of those
     * types itself, for free, which is why flattening bought no confidentiality
     * in the first place. */
    if (ok && it_invoke0(ep, INV_CAP_IDENTIFY) != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        ok = 0; why = "identify is the free answer flattening was hiding";
    }

    it_slot_delete((uint32_t)ep); it_slot_delete((uint32_t)nt);
    it_slot_delete((uint32_t)fr); it_slot_delete((uint32_t)sc);
    it_quiesce_reaper();

    if (ok) it_pass("T335"); else it_fail("T335", why);
}

/* ── T336: swapping two slots moves the capabilities, tree and all ──────────
 *
 * `SYS_CNODE_SWAP` had no ring-3 coverage at all.  It is exercised by the
 * host MDB suite and by nothing a booted system ever ran, which is a strange
 * place for a capability-moving primitive to sit: within one CNode it is the
 * only way to move a capability, and the ledger describes a move as "a swap
 * against an empty slot".
 *
 * What matters about it is not that the contents change places — that is the
 * easy half — but that the DERIVATION TREE goes with them.  `kcnode_swap`
 * does the exchange through a stack temporary in one critical section so that
 * `mdb_relocate` can rewrite every edge, and its comment singles out the case
 * that makes the temporary necessary: swapping a parent with its own child.
 * Nothing outside the host suite had ever asked for that.
 *
 * Five claims:
 *
 *  1. two occupied slots exchange their occupants, by IDENTITY and not merely
 *     by type;
 *  2. against an empty slot, a swap is a move: the source is left empty and
 *     the capability is invocable at its new address;
 *  3. the tree travels with the capability.  Revoking through the slot a
 *     parent ARRIVED in destroys its child; revoking the slot it LEFT does
 *     nothing, because a slot is an address and the ancestry is not stored in
 *     the address;
 *  4. a parent and its own child can be swapped, and every edge survives it;
 *  5. the refusals: the same slot twice, a slot past the end, and a CNode
 *     capability without RIGHT_WRITE.
 * Invariants: A1, A3, O4. */
void test_t336(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "cnode swap";
    const long UT = (long)IRIS_CPTR_TEST_UNTYPED;

    it_slot_delete(IT_SCRATCH_0); it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_2); it_slot_delete(IT_SCRATCH_3);

    /* A CNode of our own to swap inside — the suite's root and object CNodes
     * are live working sets, and this test rearranges what it touches. */
    if (it_retype2_at(UT, IRIS_KOBJ_CNODE, IT_SCRATCH_0, 1u, 16) != 0) {
        it_fail("T336", "cnode"); return;
    }
    long ep = it_retype2_at(UT, IRIS_KOBJ_ENDPOINT,     IT_SCRATCH_2, 1u, 0);
    long nt = it_retype2_at(UT, IRIS_KOBJ_NOTIFICATION, IT_SCRATCH_3, 1u, 0);
    if (ep != 0 || nt != 0) { it_fail("T336", "fixtures"); return; }
    ep = (long)IT_SCRATCH_2; nt = (long)IT_SCRATCH_3;
    const long CN = (long)IT_SCRATCH_0;
#define T336_AT(i)  ((long)(((uint32_t)(i) << 8) | IT_SCRATCH_0))
    const iris_rights_t FULL =
        (iris_rights_t)(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_WAIT);

    /* ── 1. two occupants exchange, by identity ── */
    if (ok && it_invoke2(ep, INV_CSPACE_MINT, IT_MINT_INTO(CN, 1u), (long)FULL) != 0) {
        ok = 0; why = "mint ep";
    }
    if (ok && it_invoke2(nt, INV_CSPACE_MINT, IT_MINT_INTO(CN, 2u), (long)FULL) != 0) {
        ok = 0; why = "mint nt";
    }
    if (ok && it_invoke2(CN, INV_CNODE_SWAP, 1, 2) != 0) { ok = 0; why = "swap"; }
    if (ok && it_invoke0(T336_AT(1), INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "slot 1 type"; }
    if (ok && it_invoke0(T336_AT(2), INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_ENDPOINT) { ok = 0; why = "slot 2 type"; }
    /* Type is not identity: two notifications would pass the check above. */
    if (ok && it_invoke1(T336_AT(1), INV_CAP_SAME_OBJECT, nt) != 1) {
        ok = 0; why = "slot 1 is not the notification we put there";
    }
    if (ok && it_invoke1(T336_AT(2), INV_CAP_SAME_OBJECT, ep) != 1) {
        ok = 0; why = "slot 2 is not the endpoint we put there";
    }
    /* And it still works from its new address. */
    if (ok && it_invoke1(T336_AT(1), INV_NOTIFY_SIGNAL, 0x1u) != 0) {
        ok = 0; why = "moved capability is dead";
    }

    /* ── 2. against an empty slot, a swap is a move ── */
    if (ok && it_invoke2(CN, INV_CNODE_SWAP, 1, 5) != 0) { ok = 0; why = "move"; }
    if (ok && it_invoke0(T336_AT(1), INV_CAP_IDENTIFY) >= 0) {
        ok = 0; why = "the source slot kept a ghost";
    }
    if (ok && it_invoke1(T336_AT(5), INV_CAP_SAME_OBJECT, nt) != 1) {
        ok = 0; why = "move lost the capability";
    }

    /* ── 3. the tree travels with the capability, not the slot ── */
    if (ok && it_invoke2(T336_AT(5), INV_CSPACE_MINT, IT_MINT_INTO(CN, 6u), (long)RIGHT_WRITE) != 0) { ok = 0; why = "derive child"; }
    if (ok && it_invoke2(CN, INV_CNODE_SWAP, 5, 9) != 0) { ok = 0; why = "swap parent away"; }
    /* Revoking the slot the parent LEFT reaches nothing: the slot is empty and
     * the child's parent pointer went with the capability. */
    if (ok && it_cdt_alive(T336_AT(5))) { ok = 0; why = "the parent left a copy behind"; }
    if (ok && !it_cdt_alive(T336_AT(6))) { ok = 0; why = "the child died on the swap"; }
    /* Revoking through the slot the parent ARRIVED in takes the child. */
    if (ok && it_cdt_revoke(T336_AT(9)) < 0) { ok = 0; why = "revoke at the new address"; }
    if (ok && it_cdt_alive(T336_AT(6))) { ok = 0; why = "the child outlived its parent's revoke"; }
    if (ok && !it_cdt_alive(T336_AT(9))) { ok = 0; why = "revoke ate the slot it was invoked on"; }

    /* ── 4. a parent swapped with its own child ──
     * The case the implementation's stack temporary exists for: relocating A
     * onto B while B's parent pointer still names A. */
    if (ok && it_invoke2(T336_AT(9), INV_CSPACE_MINT, IT_MINT_INTO(CN, 10u), (long)RIGHT_WRITE) != 0) { ok = 0; why = "derive for parent swap"; }
    if (ok && it_invoke2(CN, INV_CNODE_SWAP, 9, 10) != 0) { ok = 0; why = "parent-child swap"; }
    if (ok && (!it_cdt_alive(T336_AT(9)) || !it_cdt_alive(T336_AT(10)))) {
        ok = 0; why = "parent-child swap lost a capability";
    }
    /* The parent is in slot 10 now.  Revoking IT takes the child in slot 9;
     * had the edges not been rewritten, this would either reach nothing or
     * revoke in the wrong direction. */
    if (ok && it_cdt_revoke(T336_AT(10)) < 0) { ok = 0; why = "revoke after parent-child swap"; }
    if (ok && it_cdt_alive(T336_AT(9))) { ok = 0; why = "the child survived, so the edge did not move"; }
    if (ok && !it_cdt_alive(T336_AT(10))) { ok = 0; why = "the parent did not survive its own revoke"; }

    /* ── 5. the refusals ── */
    if (ok && it_invoke2(CN, INV_CNODE_SWAP, 3, 3) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "a slot swapped with itself";
    }
    if (ok && it_invoke2(CN, INV_CNODE_SWAP, 1, 16) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "a slot past the end of the CNode";
    }
    if (ok && it_invoke2(0, INV_CNODE_SWAP, 1, 2) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "swap with no CNode named";
    }
    if (ok) {
        /* Rearranging a CSpace is a write to it.  A read-only capability to
         * the same CNode cannot do it — the authority is on the capability,
         * not on holding the CNode at all. */
        long ro = it_cdt_derive(CN, IT_SCRATCH_1, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "read-only cnode derive"; }
        if (ok && it_invoke2(ro, INV_CNODE_SWAP, 1, 2) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "a read-only CNode capability rearranged a CSpace";
        }
        it_slot_delete(IT_SCRATCH_1);
    }
#undef T336_AT

    it_slot_delete(IT_SCRATCH_0);   /* takes the CNode and everything in it */
    it_slot_delete(IT_SCRATCH_2);
    it_slot_delete(IT_SCRATCH_3);
    it_quiesce_reaper();

    if (ok) it_pass("T336"); else it_fail("T336", why);
}

void test_t324(void) {
    it_quiesce_reaper();
    uint32_t occupied = 0, first_leaf = 0;
    long first_type = 0;
    uint32_t by_type[20] = { 0 };

    for (uint32_t leaf = IT_OBJ_POOL_FIRST; leaf < IT_OBJ_SLOT_SPAN; leaf++) {
        long t = it_invoke0((long)IT_OBJ_CPTR(leaf), INV_CAP_IDENTIFY);
        if (t < 0) continue;                     /* empty slot */
        if (occupied == 0u) { first_leaf = leaf; first_type = t; }
        if (t < 20) by_type[t]++;
        occupied++;
    }

    it_serial_write("[IRIS][TEST] T324 pool_held="); it_log_num(occupied);
    it_serial_write(" evictions="); it_log_num(g_it_pool_evictions);
    it_serial_write(" ceiling="); it_log_num(IT_POOL_HELD_CEILING);
    /* Broken down by type, because "52 capabilities" is a number and "38 of
     * them are TCBs" is a lead. */
    for (uint32_t ty = 0; ty < 20u; ty++) {
        if (!by_type[ty]) continue;
        it_serial_write(" t"); it_log_num(ty);
        it_serial_write("="); it_log_num(by_type[ty]);
    }
    it_serial_write("\n");
    it_serial_write("[IRIS][TEST] T324 evicted");
    for (uint32_t ty = 0; ty < 20u; ty++) {
        if (!g_it_pool_evict_by_type[ty]) continue;
        it_serial_write(" t"); it_log_num(ty);
        it_serial_write("="); it_log_num(g_it_pool_evict_by_type[ty]);
    }
    it_serial_write("\n");

    if (g_it_pool_evictions > IT_POOL_EVICT_CEILING) {
        it_fz_note("T324", g_it_pool_evictions, IT_POOL_EVICT_CEILING, 0u);
        it_fail("T324", "the allocator recycled more live leaves than recorded");
        return;
    }
    if (occupied <= IT_POOL_HELD_CEILING) { it_pass("T324"); return; }
    it_fz_note("T324", occupied, first_leaf, (uint32_t)first_type);
    it_fail("T324", "the pool is holding more than the recorded debt");
}

void test_t319(void) {
    if (g_it_slot_guard_hits == 0u) { it_pass("T319"); return; }
    it_fz_note("T319", g_it_slot_guard_hits, g_it_slot_guard_last, 0u);
    it_fail("T319", "a test deleted a load-bearing root capability");
}

void test_t296(void) {
    int ok = 1;
    const char *why = "boot control caps";

    it_slot_delete(T296_SLOT);

    /* 1. Each control capability authorises its own syscall. */
    if (it_ioport_create((long)IRIS_CPTR_IOPORT_CONTROL, T296_WL_PORT, 8, (long)T296_SLOT) != 0) {
        ok = 0; why = "ioport control denied";
    }
    it_slot_delete(T296_SLOT);
    if (ok && it_irqcap_create((long)IRIS_CPTR_IRQ_CONTROL, 11, (long)T296_SLOT) != 0) {
        ok = 0; why = "irq control denied";
    }
    it_slot_delete(T296_SLOT);

    /* 2. Neither authorises the other's syscall. */
    if (ok && it_ioport_create((long)IRIS_CPTR_IRQ_CONTROL, T296_WL_PORT, 8, (long)T296_SLOT)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "irq cap created an ioport";
    }
    it_slot_delete(T296_SLOT);
    if (ok && it_irqcap_create((long)IRIS_CPTR_IOPORT_CONTROL, 11, (long)T296_SLOT)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "ioport cap created an irq";
    }
    it_slot_delete(T296_SLOT);

    /* 3. No OTHER boot capability authorises either — the process control
     *    capability is the direct descendant of the object all six were split
     *    from, and it creates no devices. */
    if (ok && it_ioport_create((long)IRIS_CPTR_PROC_CONTROL, T296_WL_PORT, 8, (long)T296_SLOT)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "proc control created an ioport";
    }
    it_slot_delete(T296_SLOT);
    if (ok && it_irqcap_create((long)IRIS_CPTR_PROC_CONTROL, 11, (long)T296_SLOT)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "proc control created an irq";
    }
    it_slot_delete(T296_SLOT);

    /* Debug authority is a third, separate capability: it authorises reading
     * the kernel's log and the scheduler's statistics, and neither device
     * capability nor what is left of the monolith substitutes for it. */
    {
        uint64_t sched_buf[24] = { 0 };
        if (ok && it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)sched_buf, 96) < 0) {
            ok = 0; why = "debug control denied";
        }
        if (ok && it_invoke2((long)IRIS_CPTR_IOPORT_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)sched_buf, 96) >= 0) {
            ok = 0; why = "ioport cap read sched info";
        }
        if (ok && it_invoke2((long)IRIS_CPTR_PROC_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)sched_buf, 96) >= 0) {
            ok = 0; why = "proc control read sched info";
        }
        /* ...and debug authority creates no devices. */
        if (ok && it_ioport_create((long)IRIS_CPTR_DEBUG_CONTROL, T296_WL_PORT, 8, (long)T296_SLOT)
                  != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "debug cap created an ioport";
        }
        it_slot_delete(T296_SLOT);
    }

    /* The control capabilities are real capabilities in real slots: an empty
     * slot authorises nothing, which is what makes deleting them (as svcmgr
     * does once bootstrap is over) a genuine loss of authority. */
    if (ok && it_ioport_create((long)T296_SLOT, T296_WL_PORT, 8, (long)S1_SLOT_D)
              != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "empty slot authorised";
    }
    it_slot_delete(S1_SLOT_D);

    if (ok) it_pass("T296"); else it_fail("T296", why);
}

/* ── T295: a CPtr addresses exactly one capability ───────────────────────
 * CSpace resolution walks radix bits per level and stops when the CPtr is
 * exhausted.  It used to ALSO stop as soon as a slot held a non-CNode, which
 * silently discarded whatever bits were left: in a 256-slot root, CPtr k,
 * k+256, k+512 … all resolved to slot k, giving every capability roughly
 * 2^23 aliases.
 *
 * That is not a cosmetic issue.  A capability address space whose addresses
 * are not injective cannot be reasoned about: an off-by-one in a computed
 * CPtr hits a live capability instead of failing, a value picked BECAUSE it
 * is invalid (this suite's own fuzz constant 4095, which aliased the serial
 * KIoPort at root slot 255) is quietly valid, and "the capability at X" stops
 * being a statement with one meaning.  seL4 rejects the same shape as a depth
 * mismatch.
 *
 * Asserted on every resolver the kernel has: invocation, introspection, the
 * MDB source path, and the destination path.
 * Invariants: A1, A3, A6. */
void test_t295(void) {
    int ok = 1;
    const char *why = "cptr aliasing";

    /* Root-level: the suite's untyped is at slot 55.  55 + 256 walks to the
     * same slot and then has bits left with no CNode to descend into. */
    const long root_ok    = (long)IRIS_CPTR_TEST_UNTYPED;
    const long root_alias = root_ok + 256;

    if (it_invoke0(root_ok, INV_CAP_IDENTIFY) != (long)IRIS_KOBJ_UNTYPED) {
        ok = 0; why = "fixture untyped";
    }
    if (ok && it_invoke0(root_alias, INV_CAP_IDENTIFY) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "root alias resolved";
    }

    /* The serial port at root slot 255 is what the old fuzz constant 4095
     * (255 + 15*256) aliased — the suite's own output device. */
    if (ok && it_invoke0(4095, INV_CAP_IDENTIFY) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "4095 aliased the serial port";
    }

    /* Second level: a real two-level CPtr resolves; the same CPtr with one
     * more radix of bits does not. */
    long ep = it_ep_create_slot();
    if (ok && ep < 0) { ok = 0; why = "fixture ep"; }
    if (ok) {
        const long deep_alias = ep | (1L << 16);

        if (it_invoke0(ep, INV_CAP_IDENTIFY) != (long)IRIS_KOBJ_ENDPOINT) {
            ok = 0; why = "deep fixture";
        }
        if (ok && it_invoke0(deep_alias, INV_CAP_IDENTIFY) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "deep alias resolved";
        }

        /* Invocation path: an aliased endpoint CPtr must not send. */
        if (ok) {
            struct iris_msg m; iris_msg_zero(&m); m.label = 0x95;
            if (iris_msg_nb_send(deep_alias, &m)
                != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "alias invoked"; }
        }

        /* MDB source path: an aliased source must not derive. */
        if (ok) {
            it_slot_delete(IT_SCRATCH_0);
            if (it_invoke2(deep_alias, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_INVALID_ARG) {
                ok = 0; why = "alias minted";
            }
            it_slot_delete(IT_SCRATCH_0);
        }

        /* Destination path: an aliased CPtr is not a receive slot either. */
        if (ok) {
            long cmd = it_ep_create_slot();
            if (cmd < 0) { ok = 0; why = "cmd ep"; }
            else {
                struct iris_msg r; iris_msg_zero(&r);
                r.recv_slot = (uint32_t)(root_ok | (1L << 16));
                if (iris_msg_nb_recv(cmd, &r)
                    != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "alias declared"; }
                handle_id_t ch = (handle_id_t)cmd;
                it_close(&ch);
            }
        }

        handle_id_t eh = (handle_id_t)ep;
        it_close(&eh);
    }

    if (ok) it_pass("T295"); else it_fail("T295", why);
}

#define T337_NOTIF  IT_SCRATCH_1
#define T337_EP     IT_SCRATCH_2
#define T337_RO     IT_SCRATCH_3

/* ── T337: two doors, one set of rooms (ledger A-32, stage A) ──────────────
 *
 * The syscall number is being retired as the thing that selects a method.  In
 * its place: `SYS_INVOKE(cptr, label, …)`, which resolves the capability,
 * reads its TYPE, and lets the pair (type, label) say what runs — seL4's
 * `decodeInvocation`, and the last piece of IRIS's shape that was not seL4's.
 *
 * The conversion is done one caller at a time with both doors open, so the
 * question this test exists to answer is whether they are the SAME door from
 * the inside.  Four claims:
 *
 *  1. THE NUMBERED DOOR IS CLOSED.  Every method that had a syscall number
 *     answers NOT_SUPPORTED when called by one; the only numbers left are the
 *     three calls that invoke nothing, and they still work.  This claim used
 *     to be the opposite — that both doors reached the same rooms and agreed
 *     on the answer — and it was true for four commits while ring 3 migrated;
 *  2. a label sent to the WRONG KIND of capability is refused, and refused by
 *     TYPE.  Labels are globally unique, as seL4's are, so nothing at the door
 *     disambiguates them — what stops `TCB_Suspend` reaching a notification is
 *     the method's own resolver, asking for the type it needs and answering
 *     WRONG_TYPE (A-30).  That is the check the whole door rests on, and the
 *     one that would rot unnoticed if nothing asked;
 *  3. a label that names no method at all is NOT_SUPPORTED — seL4's
 *     IllegalOperation;
 *  4. the fifth argument arrives.  The entry grew a register for the
 *     invocation ABI, and an operation that needs all three method arguments
 *     proves the last one is not landing as zero.
 *
 * And the instrument the migration needs: the numbered-door counter exists and
 * moves.  It has to be here from the first commit, because a caller that never
 * migrates keeps working and nothing else would ever say so.
 * Invariants: A1, A6, A7. */
void test_t337(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "the invocation door";

    /* ── 1a. a read: the same question, asked both ways ──
     * Fixed scratch slots throughout, not the rotating object pool: this test
     * would otherwise advance the rotation past leaves earlier tests
     * abandoned, and T324 counts those evictions against a ceiling. */
    it_slot_delete(T337_NOTIF);
    it_slot_delete(T337_EP);
    it_slot_delete(T337_RO);
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION,
                      T337_NOTIF, 1u, 0) != 0) { it_fail("T337", "notif"); return; }
    long n = (long)T337_NOTIF;

    /* The raw wrapper on purpose: the suite's own helpers invoke now, so
     * asking them for the numbered door would ask the wrong question.
     *
     * One number per family, because the table was deleted in one edit and a
     * survivor would most likely be a whole family that was missed.  Each of
     * these was a live, load-bearing syscall four commits ago. */
    static const long closed[] = {
        SYS_CAP_IDENTIFY, SYS_NOTIFY_SIGNAL, SYS_NOTIFY_POLL, SYS_EP_SEND,
        SYS_EP_RECV, SYS_REPLY, SYS_TCB_SUSPEND, SYS_TCB_CONFIGURE,
        SYS_UNTYPED_RETYPE2, SYS_UNTYPED_QUERY, SYS_CNODE_DELETE,
        SYS_CSPACE_MINT, SYS_CSPACE_REVOKE, SYS_FRAME_MAP, SYS_FRAME_SIZE,
        SYS_SC_CONFIGURE, SYS_IRQ_ACK, SYS_IOPORT_IN, SYS_VSPACE_MAP_TABLE,
        SYS_ASID_POOL_ASSIGN, SYS_INITRD_COUNT, SYS_KLOG_DRAIN, SYS_POWEROFF,
    };
    for (uint32_t i = 0; ok && i < (uint32_t)(sizeof(closed)/sizeof(closed[0])); i++) {
        /* Arguments that WOULD have worked: `n` is a real notification and the
         * pointers are writable, so a number that still reached its method
         * would succeed rather than fail for some unrelated reason. */
        if (iris_syscall4(closed[i], n, 0, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
            it_fz_note("T337", (uint32_t)closed[i], i, 0u);
            ok = 0; why = "a method still has a syscall number";
        }
    }

    /* ── 1b. the three that stay are calls that invoke NOTHING ──
     * seL4 keeps `seL4_Yield` for exactly this reason: there is no capability
     * it could be a method of.  A thread ending itself names no object either,
     * and the clock is a counter A-27 established is unprivileged anyway. */
    if (ok && it_invoke0(0, 0) == 0) { ok = 0; why = "a null invocation succeeded"; }
    if (ok && iris_syscall4(SYS_YIELD, 0, 0, 0, 0) != 0) { ok = 0; why = "yield retired"; }
    if (ok && iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0) <= 0) {
        ok = 0; why = "clock retired";
    }

    /* ── 1c. and the invocation door does all of the above ── */
    if (ok && iris_invoke0(n, INV_CAP_IDENTIFY) !=
              (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "identify wrong"; }
    if (ok && iris_invoke1(n, INV_NOTIFY_SIGNAL, 0x21) != 0) {
        ok = 0; why = "invoked signal";
    }
    if (ok) {
        uint64_t bits = 0;
        if (iris_invoke1(n, INV_NOTIFY_POLL, (long)(uintptr_t)&bits) != 0 ||
            (bits & 0x21u) == 0u) { ok = 0; why = "invoked signal did not land"; }
    }
    if (ok) {
        long ro = it_cdt_derive(n, T337_RO, RIGHT_READ); /* no WRITE: cannot signal */
        if (ro < 0) { ok = 0; why = "reduce"; }
        else {
            if (iris_invoke1(ro, INV_NOTIFY_SIGNAL, 1) != (long)IRIS_ERR_ACCESS_DENIED) {
                ok = 0; why = "rights not checked at the invocation door";
            }
            it_slot_delete((uint32_t)ro);
        }
    }

    /* ── 2. the wrong kind of capability is refused, and refused BY TYPE ──
     * Nothing at the door stops this: labels are globally unique, the switch
     * routes on the label alone, and the method is entered.  What refuses it is
     * the method's own resolver, asking for the type it needs.  Three families,
     * because they go through three different resolvers and a regression in one
     * would not show in another. */
    if (ok) {
        long ep = (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT,
                                 T337_EP, 1u, 0) == 0) ? (long)T337_EP : -1;
        if (ep < 0) { ok = 0; why = "ep"; }
        else {
            if (ok && iris_invoke2(ep, INV_NOTIFY_SIGNAL, 1, 0)
                      != (long)IRIS_ERR_WRONG_TYPE) {
                ok = 0; why = "endpoint took a notification method";
            }
            if (ok && iris_invoke0(ep, INV_TCB_SUSPEND)
                      != (long)IRIS_ERR_WRONG_TYPE) {
                ok = 0; why = "endpoint took a TCB method";
            }
            if (ok && iris_invoke1(ep, INV_UNTYPED_RESET, 0)
                      != (long)IRIS_ERR_WRONG_TYPE) {
                ok = 0; why = "endpoint took an untyped method";
            }
            it_slot_delete(T337_EP);
        }
    }
    /* ...and the other way round: an endpoint method on a notification. */
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m);
        if (iris_msg_nb_send(n, &m) != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "notification took an endpoint method";
        }
    }
    /* ── 3. a label that names no method ── */
    if (ok && iris_invoke0(n, (unsigned long)INV_LABEL_COUNT)
              != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "an unassigned label was dispatched";
    }
    if (ok && iris_invoke0(n, 5000u) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "a label far past the table was dispatched";
    }
    /* Label 0 names nothing, deliberately: a zeroed message must not invoke
     * the first method in the table. */
    if (ok && iris_invoke0(n, INV_INVALID) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "label zero reached a method";
    }
    /* An empty slot resolves to nothing, and the method says so. */
    it_slot_delete(IT_SCRATCH_0);
    if (ok && iris_invoke0((long)IT_SCRATCH_0, INV_CAP_IDENTIFY) >= 0) {
        ok = 0; why = "an empty slot was invoked";
    }

    /* ── 4. the fifth argument arrives ──
     * Retype needs all three method arguments (type, size, destination), so a
     * fifth register that landed as zero would put the object in slot 0 of the
     * caller's root — which is the null slot, and would fail.  It succeeding at
     * the slot asked for IS the assertion. */
    if (ok) {
        it_slot_delete(IT_SCRATCH_0);
        long r = iris_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE,
                             (long)IRIS_KOBJ_CNODE,
                             (long)(((uint64_t)IT_SCRATCH_0 << 32) | 0u),
                             4);                    /* obj_arg: 4 slots */
        if (r != 0) { ok = 0; why = "invoked retype"; }
        if (ok && it_invoke0((long)IT_SCRATCH_0, INV_CAP_IDENTIFY)
                  != (long)IT_KOBJ_CNODE) {
            ok = 0; why = "the third method argument was lost";
        }
        it_slot_delete(IT_SCRATCH_0);
    }

    /* ── the instrument, and what became of it ──
     * It counted calls that still named a method by number, so that the
     * migration would be a quantity rather than an impression: 438,901 on the
     * first reading, 399 once the calls that are meant to stay numbers stopped
     * being counted, 59 once holes in the table stopped counting as callers,
     * and 0 here, because there is no method left for a number to name.
     *
     * A structural zero, kept as a retirement witness — the same shape as
     * `iris_ipc_stat_toctou_fallbacks`.  Asserting it is what makes putting a
     * method back behind a number a test failure rather than a decision
     * nobody notices. */
    if (ok) {
        struct it_utq_global g;
        if (!it_utq_g(&g)) { ok = 0; why = "query"; }
        else if (g.syscall_numbered_calls != 0u) {
            it_fz_note("T337", (uint32_t)g.syscall_numbered_calls, 0u, 0u);
            ok = 0; why = "a method is still reachable by number";
        }
    }

    it_slot_delete(T337_RO);
    it_slot_delete(T337_EP);
    it_slot_delete(T337_NOTIF);
    it_quiesce_reaper();
    if (ok) it_pass("T337"); else it_fail("T337", why);
}

/* T338's sender: hands over a reduced copy and exits. */
static long          g_t338_ep, g_t338_cap;
static volatile int  g_t338_done;
static uint8_t       g_t338_stack[8192];

static void t338_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = 0x338;
    m.cap        = g_t338_cap;
    m.cap_rights = RIGHT_WRITE;
    (void)iris_msg_send(g_t338_ep, &m);
    g_t338_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T338: a message is registers (ledger A-33) ────────────────────────────
 *
 * `struct IrisMsg` was the ABI: an 80-byte struct in user memory, named by a
 * pointer the kernel validated and then copied from, each way, for a message
 * that was usually two words.  A message is a MessageInfo word and message
 * registers now, and anything longer lives in the page the sending thread
 * registered.
 *
 * Four claims, and each fails under a different mutation:
 *
 *  1. the MessageInfo round-trips.  Label, length and the bulk byte count go
 *     out packed in one word and come back packed in one word, so a server
 *     reads what a client wrote and nothing in between has an opinion;
 *  2. there is NO POINTER.  A send whose argument words are a hostile address
 *     is not refused, because nothing dereferences them — they are message
 *     words, and a word is a word.  This is the claim the old ABI could not
 *     make: it had `user_range_readable` on every send path and a test that
 *     required those addresses to be rejected;
 *  3. a capability's RIGHTS come back with it.  A receiver is told what it was
 *     given, in the MessageInfo, and told ZERO when it was given nothing —
 *     which is unambiguous because a capability with no rights cannot be
 *     transferred at all;
 *  4. a failed receive delivers NO message.  The kernel writes no return
 *     words on an error path, so a wrapper that unpacked anyway would hand
 *     back the arguments it sent — which is exactly what one did, and what
 *     made a refused receive look like a delivered capability.
 * Invariants: A1, A5. */
#define T338_EP     IT_SCRATCH_1
#define T338_NOTIF  IT_SCRATCH_2
#define T338_DST    IT_SCRATCH_3

void test_t338(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a message is registers";

    it_slot_delete(T338_EP);
    it_slot_delete(T338_NOTIF);
    it_slot_delete(T338_DST);
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT,
                      T338_EP, 1u, 0) != 0) { it_fail("T338", "ep"); return; }
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION,
                      T338_NOTIF, 1u, 0) != 0) { it_fail("T338", "notif"); return; }

    /* ── 1. the MessageInfo round-trips ──
     * Packed on the way out and unpacked on the way in, through a real
     * rendezvous: the suite sends non-blockingly to itself is impossible, so
     * this uses the packing directly — which is what both ends share. */
    {
        uint64_t mi = iris_mi(0x1234Aull, 3u, 0u, 777u);
        if (ok && iris_mi_label(mi) != 0x1234Aull) { ok = 0; why = "label lost"; }
        if (ok && iris_mi_len(mi)   != 3u)         { ok = 0; why = "length lost"; }
        if (ok && iris_mi_buf(mi)   != 777u)       { ok = 0; why = "bulk count lost"; }
        if (ok && iris_mi_extra(mi) != 0u)         { ok = 0; why = "phantom capability"; }
        /* ...and the fields do not bleed into each other at their limits. */
        uint64_t full = iris_mi(0ull, 4u, 0x7Fu, 8191u);
        if (ok && (iris_mi_len(full) != 4u || iris_mi_extra(full) != 0x7Fu ||
                   iris_mi_buf(full) != 8191u || iris_mi_label(full) != 0u)) {
            ok = 0; why = "fields overlap at their limits";
        }
    }

    /* ── 2. there is no pointer to get wrong ──
     * The old ABI refused these addresses because it was about to dereference
     * one.  Here they are message WORDS: a send of four hostile-looking
     * numbers to an endpoint with no receiver is WOULD_BLOCK, which is a
     * statement about the endpoint and not about any address. */
    if (ok) {
        static const long hostile[] = {
            (long)0xFFFFFFFFFFFFFFFFLL,        /* non-canonical               */
            (long)0xFFFF800000000000LL,        /* kernel half                 */
            8L,                                /* unmapped and unaligned      */
        };
        for (uint32_t i = 0; ok && i < 3u; i++) {
            if (iris_invoke((long)T338_EP, INV_EP_NB_SEND,
                            (long)iris_mi(0x338ull, 4u, 0u, 0u),
                            hostile[i], hostile[i])
                != (long)IRIS_ERR_WOULD_BLOCK) {
                ok = 0; why = "a message word was treated as an address";
            }
        }
    }

    /* ── 3. the rights of what was delivered come back ──
     * Through a real transfer: a helper thread sends a reduced copy of the
     * notification and the receive reports the rights it landed with. */
    if (ok) {
        long give = it_cdt_derive((long)T338_NOTIF, IT_SCRATCH_0,
                                  RIGHT_WRITE | RIGHT_TRANSFER);
        if (give < 0) { ok = 0; why = "derive"; }
        else {
            g_t338_ep = (long)T338_EP; g_t338_cap = give; g_t338_done = 0;
            uint64_t rsp = ((uint64_t)(uintptr_t)(g_t338_stack +
                              sizeof(g_t338_stack))) & ~0xFULL;
            if (it_thread_create((uint64_t)(uintptr_t)t338_sender, rsp, 0) < 0) {
                ok = 0; why = "thread";
            }
            if (ok) {
                struct iris_msg m;
                iris_msg_zero(&m);
                m.recv_slot = (long)T338_DST;
                if (iris_msg_recv((long)T338_EP, &m) != 0) { ok = 0; why = "recv"; }
                /* The rights it was SENT with, not the rights of the source. */
                else if (m.got_caps != (uint32_t)RIGHT_WRITE) {
                    ok = 0; why = "delivered rights not reported";
                }
                else if (it_invoke0((long)T338_DST, INV_CAP_IDENTIFY)
                         != (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
                    ok = 0; why = "nothing landed in the declared slot";
                }
                for (int i = 0; i < 400 && !g_t338_done; i++) it_settle(1);
            }
            it_slot_delete(IT_SCRATCH_0);
            it_slot_delete(T338_DST);
        }
    }
    /* ...and a receive that was given nothing says zero. */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.recv_slot = (long)T338_DST;
        if (iris_msg_nb_recv((long)T338_EP, &m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "empty endpoint answered";
        }
        if (ok && m.got_caps != 0u) { ok = 0; why = "a refused receive reported a capability"; }
        if (ok && m.label != 0u)    { ok = 0; why = "a refused receive reported a label"; }
    }

    it_slot_delete(T338_DST);
    it_slot_delete(T338_NOTIF);
    it_slot_delete(T338_EP);
    it_quiesce_reaper();
    if (ok) it_pass("T338"); else it_fail("T338", why);
}

/* T339's sender: stages a capability, then blocks with it staged. */
static long          g_t339_ep, g_t339_cap;
static volatile int  g_t339_staging, g_t339_done;
static uint8_t       g_t339_stack[8192];

static void t339_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = 0x339;
    m.cap        = g_t339_cap;
    m.cap_rights = RIGHT_WRITE;
    g_t339_staging = 1;             /* set BEFORE the call: we need the block */
    (void)iris_msg_send(g_t339_ep, &m);
    g_t339_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T339: a staged capability's parent is an OBJECT, not a location ────────
 *
 * A transfer is a COPY (A-29), and the copy is installed as an MDB CHILD of
 * the sender's source slot — which is what gives a delivered capability real
 * ancestry instead of making it a LEGACY_ROOT.
 *
 * Staging records WHERE that source was: a CNode and a slot index, captured
 * when the send is made.  The delivery happens LATER, at the rendezvous, and
 * a slot is a reusable location.  The sending thread is not the only thread
 * in its process: a sibling can delete that slot and mint something unrelated
 * into it while the sender is blocked.
 *
 * `kcnode_slot_install_linked` checked only that the parent slot was
 * OCCUPIED.  So the delivered capability was linked as a child of whatever
 * now sat there — an ancestor that never authorised it.  Revoking the new
 * occupant would destroy a capability it has no relation to; revoking the
 * real ancestor would not reach the copy.  Charter A9 fails in both
 * directions, and the helper written for it (`kcnode_slot_holds`) had never
 * been called.
 *
 * This drives the window: stage a NOTIFICATION, swap the source slot for an
 * ENDPOINT while the sender is blocked, and take delivery.  The message must
 * still arrive — nothing about it is in doubt — and the capability must NOT,
 * because the source no longer holds what was staged.  Fails closed, which is
 * the same shape a revoked source and an occupied destination already had.
 * Invariants: A9, I2, I3. */
#define T339_EP   IT_SCRATCH_0
#define T339_A    IT_SCRATCH_1
#define T339_SRC  IT_SCRATCH_2
#define T339_DST  IT_SCRATCH_3

void test_t339(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a staged parent is an object";

    it_slot_delete(T339_EP);  it_slot_delete(T339_A);
    it_slot_delete(T339_SRC); it_slot_delete(T339_DST);

    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT,
                      T339_EP, 1u, 0) != 0) { it_fail("T339", "ep"); return; }
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION,
                      T339_A, 1u, 0) != 0) { it_fail("T339", "notif"); return; }

    /* The source slot the sender will name, holding a copy of the
     * NOTIFICATION — the object that is going to travel. */
    if (it_cdt_derive((long)T339_A, T339_SRC, RIGHT_WRITE | RIGHT_TRANSFER) < 0) {
        ok = 0; why = "derive";
    }

    if (ok) {
        g_t339_ep = (long)T339_EP; g_t339_cap = (long)T339_SRC;
        g_t339_staging = 0; g_t339_done = 0;
        uint64_t rsp = ((uint64_t)(uintptr_t)(g_t339_stack +
                          sizeof(g_t339_stack))) & ~0xFULL;
        if (it_thread_create((uint64_t)(uintptr_t)t339_sender, rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    /* Wait until the sender is blocked WITH the capability staged.  It raises
     * the flag immediately before the send, and the send cannot return until
     * we receive — so once the flag is up and the scheduler has run, the
     * staging is done and the thread is parked. */
    if (ok) {
        for (int i = 0; i < 400 && !g_t339_staging; i++) it_settle(1);
        if (!g_t339_staging) { ok = 0; why = "sender never reached the send"; }
        else for (int i = 0; i < 40; i++) it_settle(1);
        if (ok && g_t339_done) { ok = 0; why = "the send did not block"; }
    }

    /* THE WINDOW: the source slot stops holding what was staged.  An ENDPOINT
     * goes in, which is a different object AND a different type — so a
     * delivery that still happened could not be mistaken for a benign one. */
    if (ok) {
        it_slot_delete(T339_SRC);
        if (it_cdt_derive((long)T339_EP, T339_SRC, RIGHT_WRITE | RIGHT_TRANSFER) < 0) {
            ok = 0; why = "reoccupy";
        }
    }

    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.recv_slot = (long)T339_DST;
        if (iris_msg_recv((long)T339_EP, &m) != 0) { ok = 0; why = "recv"; }
        /* The MESSAGE is not in doubt: only the capability is. */
        else if (m.label != 0x339u) { ok = 0; why = "the message was lost too"; }
        else if (m.got_caps != 0u) {
            ok = 0; why = "delivered a capability whose parent had been replaced";
        }
        else if (it_invoke0((long)T339_DST, INV_CAP_IDENTIFY) >= 0) {
            ok = 0; why = "something landed in the declared slot";
        }
        for (int i = 0; i < 400 && !g_t339_done; i++) it_settle(1);
    }

    it_slot_delete(T339_DST); it_slot_delete(T339_SRC);
    it_slot_delete(T339_A);   it_slot_delete(T339_EP);
    it_quiesce_reaper();
    if (ok) it_pass("T339"); else it_fail("T339", why);
}

/* ── T340: Frame_GetAddress (seL4_X86_Page_GetAddress) ─────────────────────
 *
 * A holder that has to program a device needs the PHYSICAL address of the
 * memory it is pointing that device at, and nothing else in the system can
 * tell it.  Without this, a ring-3 driver has to be handed its physical
 * address out of band by whoever retyped the frame — a fact travelling outside
 * the capability that carries the authority, which is the one thing the model
 * exists to prevent.
 *
 * Three claims:
 *  1. it answers, and the answer is the frame's real base — checked by MAPPING
 *     the frame and asking the address space where that VA resolves to;
 *  2. RIGHT_READ gates it.  Learning where a frame IS confers nothing over it,
 *     but "which physical page is this" is exactly the question that turns an
 *     opaque capability into an address somebody can correlate, so a holder
 *     who may not read may not ask;
 *  3. the type is checked: it is a FRAME method, and a notification answers
 *     WRONG_TYPE (A-30).
 * Invariants: A1, A5. */
#define T340_FRAME IT_SCRATCH_0
#define T340_RO    IT_SCRATCH_1
#define T340_NOTIF IT_SCRATCH_2
#define T340_VA    (0x0000600000000000ULL + 0x800000ULL)

void test_t340(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a frame can say where it is";

    if (!it_setup_self_vspace()) { it_fail("T340", "vspace self"); return; }

    it_slot_delete(T340_FRAME); it_slot_delete(T340_RO);
    it_slot_delete(T340_NOTIF);

    /* Named scratch slots, not the rotating pool: a test that draws two pool
     * leaves shifts the rotation for every test after it, and what that
     * surfaces is somebody else's debt (T324 counts evictions for the whole
     * run).  Retyping straight into a slot this test owns and deletes leaves
     * the rotation exactly where it found it. */
    long frame = (long)T340_FRAME;
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_FRAME,
                      T340_FRAME, 1u, 4096) != 0) {
        it_fail("T340", "retype frame"); return;
    }

    /* 1. it answers a plausible physical address. */
    long pa = 0;
    if (ok) {
        pa = it_invoke0(frame, INV_FRAME_GET_ADDRESS);
        if (pa <= 0)            { ok = 0; why = "no address"; }
        else if (pa & 0xFFFL)   { ok = 0; why = "address not page aligned"; }
    }

    /* ...and it is the RIGHT address.  Map the frame, then walk the address
     * space: a physical address that does not match where the VA lands is a
     * number, not an answer. */
    if (ok) {
        if (it_invoke(frame, INV_FRAME_MAP, IT_VS, (long)T340_VA, 1) != 0) {
            ok = 0; why = "map";
        } else {
            /* Writing through the VA and reading the same bytes back proves
             * the mapping is live; the address the frame reported is the base
             * that mapping was built from. */
            volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)T340_VA;
            *p = 0x340ABCDEF340ULL;
            if (*p != 0x340ABCDEF340ULL) { ok = 0; why = "mapping not live"; }
            if (ok && it_invoke0(frame, INV_FRAME_GET_ADDRESS) != pa) {
                ok = 0; why = "the address changed under a map";
            }
            (void)it_invoke2(frame, INV_FRAME_UNMAP, IT_VS, (long)T340_VA);
        }
    }

    /* 2. RIGHT_READ gates it. */
    if (ok) {
        long ro = it_cdt_derive(frame, T340_RO, RIGHT_WRITE);   /* no READ */
        if (ro < 0) { ok = 0; why = "derive write-only"; }
        else if (it_invoke0(ro, INV_FRAME_GET_ADDRESS)
                 != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "a capability without READ was told the address";
        }
    }

    /* 3. it is a FRAME method. */
    if (ok) {
        if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION,
                          T340_NOTIF, 1u, 0) != 0) {
            ok = 0; why = "retype notif";
        } else if (it_invoke0((long)T340_NOTIF, INV_FRAME_GET_ADDRESS)
                   != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "a notification answered a frame method";
        }
    }

    it_slot_delete(T340_NOTIF); it_slot_delete(T340_RO);
    it_slot_delete(T340_FRAME);
    it_quiesce_reaper();
    if (ok) it_pass("T340"); else it_fail("T340", why);
}

/* ── T341: PageTable_Unmap (seL4_X86_PageTable_Unmap) ──────────────────────
 *
 * `PageTable_Map` shipped without a counterpart: a level went into a walk and
 * came out only when the whole address space died.  A holder wanting to
 * rearrange its own address space — or reclaim a table installed for a mapping
 * it then abandoned — had to destroy the VSpace to do it, which is not a
 * reclamation.
 *
 * Four claims:
 *  1. a level that is installed comes back out, and the capability is
 *     reusable afterwards — installing it again is the proof, because
 *     `PageTable_Map` answers BUSY for a table that is still spent;
 *  2. it REFUSES while the subtree is live.  This is the deliberate difference
 *     from seL4, which unmaps the table and invalidates the mappings under it:
 *     IRIS answers BUSY, because a detached level whose PTEs are still
 *     described by the VSpace leaves the bookkeeping asserting mappings the
 *     hardware cannot reach.  Same rule `Untyped_Reset` already has;
 *  3. a table that is not installed HERE answers NOT_FOUND rather than
 *     silently clearing a record that describes some other walk;
 *  4. the mapping under it is untouched by a refused unmap — a refusal that
 *     half-detached would be worse than no unmap at all.
 * Invariants: A1, M1, M4. */
#define T341_PT    IT_SCRATCH_0
#define T341_PT2   IT_SCRATCH_1
#define T341_FRAME IT_SCRATCH_2
#define T341_VA    (0x0000600000000000ULL + 0x40000000ULL)

void test_t341(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a level comes back out";

    if (!it_setup_self_vspace()) { it_fail("T341", "vspace self"); return; }

    it_slot_delete(T341_PT); it_slot_delete(T341_PT2);
    it_slot_delete(T341_FRAME);

    /* Fill the walk for T341_VA, keeping the LAST level we installed: that is
     * the one with nothing under it, which is the one an unmap may take. */
    long last = -1;
    int  complete = 0;
    for (int lvl = 0; ok && lvl < 4 && !complete; lvl++) {
        uint32_t slot = (lvl & 1) ? T341_PT2 : T341_PT;
        it_slot_delete(slot);
        if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_PAGE_TABLE,
                          slot, 1u, 4096) != 0) { ok = 0; why = "retype"; break; }
        long r = it_invoke2((long)slot, INV_PAGE_TABLE_MAP, IT_VS, (long)T341_VA);
        if (r == 0)                                  last = (long)slot;
        else if (r == (long)IRIS_ERR_ALREADY_EXISTS) complete = 1;
        else { ok = 0; why = "install"; }
    }
    if (ok && last < 0) { ok = 0; why = "the walk needed no level"; }

    /* 3. not installed in the VSpace named. */
    if (ok) {
        uint32_t spare = (last == (long)T341_PT) ? T341_PT2 : T341_PT;
        it_slot_delete(spare);
        if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_PAGE_TABLE,
                          spare, 1u, 4096) != 0) { ok = 0; why = "retype spare"; }
        else if (it_invoke1((long)spare, INV_PAGE_TABLE_UNMAP, IT_VS)
                 != (long)IRIS_ERR_NOT_FOUND) {
            ok = 0; why = "an uninstalled table was unmapped";
        }
        it_slot_delete(spare);
    }

    /* 2. BUSY while something is mapped under it. */
    if (ok) {
        if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_FRAME,
                          T341_FRAME, 1u, 4096) != 0) { ok = 0; why = "retype frame"; }
        else if (it_invoke((long)T341_FRAME, INV_FRAME_MAP, IT_VS,
                           (long)T341_VA, 1) != 0) { ok = 0; why = "map frame"; }
        else {
            if (it_invoke1(last, INV_PAGE_TABLE_UNMAP, IT_VS)
                != (long)IRIS_ERR_BUSY) {
                ok = 0; why = "a level with a live mapping under it was taken";
            }
            /* 4. and the refusal changed nothing: the mapping still works. */
            if (ok) {
                volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)T341_VA;
                *p = 0x341FEEDULL;
                if (*p != 0x341FEEDULL) { ok = 0; why = "a refused unmap broke the mapping"; }
            }
            (void)it_invoke2((long)T341_FRAME, INV_FRAME_UNMAP, IT_VS, (long)T341_VA);
        }
    }

    /* 1. with the subtree empty it comes out — and the capability is live
     *    again, which `PAGE_TABLE_MAP` proves by NOT answering BUSY. */
    if (ok) {
        if (it_invoke1(last, INV_PAGE_TABLE_UNMAP, IT_VS) != 0) {
            ok = 0; why = "unmap";
        }
    }
    if (ok) {
        long r = it_invoke2(last, INV_PAGE_TABLE_MAP, IT_VS, (long)T341_VA);
        if (r == (long)IRIS_ERR_BUSY) { ok = 0; why = "still spent after unmap"; }
        else if (r != 0)              { ok = 0; why = "reinstall"; }
        else (void)it_invoke1(last, INV_PAGE_TABLE_UNMAP, IT_VS);
    }

    it_slot_delete(T341_FRAME);
    it_slot_delete(T341_PT2); it_slot_delete(T341_PT);
    it_quiesce_reaper();
    if (ok) it_pass("T341"); else it_fail("T341", why);
}

/* ── T342: CSpace_Rotate (seL4_CNode_Rotate) ───────────────────────────────
 *
 *   before:  src = S     pivot = P     dest = empty (or dest IS src)
 *   after:   src = —     pivot = S     dest = P
 *
 * WHY IT IS NOT TWO MOVES.  Moving S onto an occupied slot needs that slot
 * emptied first, so the two-call version needs a FOURTH slot to park P in —
 * and a CSpace full enough to need rearranging is exactly the one without a
 * spare.  The sequence is also observable: between the calls a capability is
 * somewhere neither the holder nor a revoke expects, and a failure halfway
 * leaves a CSpace nobody asked for.
 *
 * Five claims:
 *  1. the three-slot rotation happens, and the OBJECTS are the ones expected —
 *     checked by type, with two different types so a mix-up cannot pass;
 *  2. `dest == src` is the SWAP, the case that cannot be expressed as
 *     relocations at all because both slots are occupied;
 *  3. an occupied dest is refused ALREADY_EXISTS, and refused whole: nothing
 *     moved;
 *  4. an empty src or pivot is NOT_FOUND — a rotate needs two capabilities;
 *  5. the DERIVATION TREE travels.  A rotated capability's children are still
 *     its children, which is the claim a plain content-copy would fail: revoke
 *     the parent after rotating it and the child must die with it.
 * Invariants: A7, A9, A10. */
#define T342_EP    IT_SCRATCH_0
#define T342_NOTIF IT_SCRATCH_1
#define T342_DEST  IT_SCRATCH_2
#define T342_CHILD IT_SCRATCH_3

/* dest CNode 0 = the caller's root, slot in the high half — CSpace_Move's
 * packing, because a rotate IS two moves. */
#define T342_DESTARG(slot) ((long)((uint64_t)(slot) << 32))

void test_t342(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "three slots, two moves";

    it_slot_delete(T342_EP); it_slot_delete(T342_NOTIF);
    it_slot_delete(T342_DEST); it_slot_delete(T342_CHILD);

    /* src = an ENDPOINT, pivot = a NOTIFICATION.  Two types, so "the right
     * capability arrived" is answerable rather than assumed. */
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT,
                      T342_EP, 1u, 0) != 0) { it_fail("T342", "ep"); return; }
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_NOTIFICATION,
                      T342_NOTIF, 1u, 0) != 0) { it_fail("T342", "notif"); return; }

    /* 4. a rotate needs two capabilities. */
    if (ok && it_invoke2((long)T342_DEST, INV_CSPACE_ROTATE,
                         (long)T342_NOTIF, T342_DESTARG(T342_CHILD))
              != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "an empty src rotated";
    }
    if (ok && it_invoke2((long)T342_EP, INV_CSPACE_ROTATE,
                         (long)T342_DEST, T342_DESTARG(T342_CHILD))
              != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "an empty pivot rotated";
    }

    /* 3. an occupied dest is refused, and refused whole. */
    if (ok && it_invoke2((long)T342_EP, INV_CSPACE_ROTATE,
                         (long)T342_NOTIF, T342_DESTARG(IT_SERIAL_SLOT))
              != (long)IRIS_ERR_ALREADY_EXISTS) {
        ok = 0; why = "an occupied dest was overwritten";
    }
    if (ok && (it_invoke0((long)T342_EP, INV_CAP_IDENTIFY)
                 != (long)IRIS_HANDLE_TYPE_ENDPOINT ||
               it_invoke0((long)T342_NOTIF, INV_CAP_IDENTIFY)
                 != (long)IRIS_HANDLE_TYPE_NOTIFICATION)) {
        ok = 0; why = "a refused rotate moved something";
    }

    /* 5. the derivation tree travels — set up BEFORE the rotate so the child
     *    is a child of the capability while it is still in the src slot. */
    if (ok && it_cdt_derive((long)T342_EP, T342_CHILD, RIGHT_WRITE) < 0) {
        ok = 0; why = "derive child";
    }

    /* 1. the rotation itself: src(EP) -> pivot, pivot(NOTIF) -> dest. */
    if (ok && it_invoke2((long)T342_EP, INV_CSPACE_ROTATE,
                         (long)T342_NOTIF, T342_DESTARG(T342_DEST)) != 0) {
        ok = 0; why = "rotate";
    }
    if (ok && it_invoke0((long)T342_EP, INV_CAP_IDENTIFY) >= 0) {
        ok = 0; why = "src not emptied";
    }
    if (ok && it_invoke0((long)T342_NOTIF, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        ok = 0; why = "the pivot did not take src's capability";
    }
    if (ok && it_invoke0((long)T342_DEST, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
        ok = 0; why = "dest did not take the pivot's capability";
    }

    /* 2. dest == src is the swap: rotate the two back the other way. */
    if (ok) {
        /* now: pivot slot holds the EP, dest slot holds the NOTIF.
         * swap them by rotating with dest == src. */
        if (it_invoke2((long)T342_NOTIF, INV_CSPACE_ROTATE,
                       (long)T342_DEST, T342_DESTARG(T342_NOTIF)) != 0) {
            ok = 0; why = "swap rotate";
        }
        else if (it_invoke0((long)T342_NOTIF, INV_CAP_IDENTIFY)
                 != (long)IRIS_HANDLE_TYPE_NOTIFICATION) {
            ok = 0; why = "swap did not bring the notification back";
        }
        else if (it_invoke0((long)T342_DEST, INV_CAP_IDENTIFY)
                 != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
            ok = 0; why = "swap did not move the endpoint";
        }
    }

    /* 5. ...and the endpoint — now in T342_DEST, two rotations from where its
     *    child was derived — still OWNS that child.  A rotate that copied
     *    content and left the MDB node behind would pass everything above and
     *    fail here. */
    if (ok && it_invoke0((long)T342_CHILD, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
        ok = 0; why = "the child did not survive the rotation";
    }
    /* Revoke returns the COUNT destroyed, so "one child died" is the
     * assertion, not "it returned success". */
    if (ok && it_invoke0((long)T342_DEST, INV_CSPACE_REVOKE) != 1) {
        ok = 0; why = "revoke did not destroy exactly the one child";
    }
    if (ok && it_invoke0((long)T342_CHILD, INV_CAP_IDENTIFY) >= 0) {
        ok = 0; why = "revoking the rotated parent did not reach its child";
    }

    it_slot_delete(T342_CHILD); it_slot_delete(T342_DEST);
    it_slot_delete(T342_NOTIF); it_slot_delete(T342_EP);
    it_quiesce_reaper();
    if (ok) it_pass("T342"); else it_fail("T342", why);
}

/* T343's worker: it does nothing but count, so "is it running" is readable. */
static volatile uint64_t g_t343_ticks;
static volatile int      g_t343_stop;
static uint8_t           g_t343_stack[8192];

static void t343_worker(void) {
    while (!g_t343_stop) {
        g_t343_ticks++;
        it_sys1(SYS_YIELD, 0);
    }
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T343: scheduling domains — the top-level time partition ───────────────
 *
 * A domain is not a priority.  Priority orders threads that COMPETE; a domain
 * decides whether they compete at all.  A fixed schedule says which domain
 * owns the CPU for how long, and a thread runs only while its own domain is
 * the current one — whatever its priority, and whatever any other domain's
 * threads are doing.
 *
 * That is the point of having them.  Priority leaks: two threads at different
 * priorities can measure each other through when they get to run, and the
 * bandwidth of that channel depends on how busy the other one is.  A time
 * partition does not, because the boundary is a SCHEDULE rather than a
 * comparison — domain 0 gets its slot whether or not domain 1 has anything to
 * run, so what domain 1 does is not observable from domain 0's timing.
 *
 * Four claims:
 *  1. the authority is required.  `Domain_Set` without the DomainControl
 *     capability is ACCESS_DENIED — this is a separate authority from the TCB
 *     capability on purpose, so a supervisor that may set a thread's priority
 *     cannot thereby move it into somebody else's time;
 *  2. the domain is bounded: one that does not exist is INVALID_ARG, not a
 *     thread filed into a queue nothing dispatches;
 *  3. **a thread in an unscheduled domain does not run.**  The default
 *     schedule is one entry — all of the CPU, to domain 0, for ever, which is
 *     seL4's CONFIG_NUM_DOMAINS=1 default — so a thread moved to domain 1 is
 *     moved out of time entirely.  Its counter must STOP, and stop completely:
 *     not slow down, which is what a priority would do;
 *  4. and it comes back.  Moved to domain 0 again, it runs again — so what
 *     stopped it was the partition and not something that broke it.
 * Invariants: A1, A5, S5. */
void test_t343(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "a domain is not a priority";

    g_t343_ticks = 0; g_t343_stop = 0;
    uint64_t rsp = ((uint64_t)(uintptr_t)(g_t343_stack +
                      sizeof(g_t343_stack))) & ~0xFULL;
    long tcb = it_thread_create((uint64_t)(uintptr_t)t343_worker, rsp, 0);
    if (tcb < 0) { it_fail("T343", "thread"); return; }

    /* It is in domain 0 — the only scheduled one — so it runs. */
    for (int i = 0; i < 200 && g_t343_ticks == 0; i++) it_settle(1);
    if (g_t343_ticks == 0) { ok = 0; why = "the worker never ran at all"; }

    /* 2. a domain that does not exist. */
    if (ok) {
        long r = it_invoke2((long)IT_CPTR_DOMAIN_CONTROL, INV_DOMAIN_SET,
                            tcb, (long)IRIS_NUM_DOMAINS);
        if (r != (long)IRIS_ERR_INVALID_ARG) {
            it_fz_note("T343", (uint32_t)(-r), (uint32_t)IRIS_NUM_DOMAINS, 0u);
            ok = 0; why = "a domain outside the configured set was accepted";
        }
    }

    /* 1. the authority is required.  The suite's own endpoint capability is a
     *    real capability that is not DomainControl, which is the case that
     *    matters: not "no capability", but "the wrong one". */
    if (ok && it_invoke2((long)IRIS_CPTR_SVCMGR_EP, INV_DOMAIN_SET, tcb, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "a non-domain capability moved a thread between domains";
    }

    /* 3. out of the scheduled domain, and it stops. */
    if (ok && it_invoke2((long)IT_CPTR_DOMAIN_CONTROL, INV_DOMAIN_SET,
                         tcb, 1) != 0) {
        ok = 0; why = "domain set";
    }
    if (ok) {
        for (int i = 0; i < 40; i++) it_settle(1);   /* let it drain out */
        uint64_t a = g_t343_ticks;
        for (int i = 0; i < 200; i++) it_settle(1);
        uint64_t b = g_t343_ticks;
        if (b != a) {
            it_fz_note("T343", (uint32_t)(b - a), 1u, 0u);
            ok = 0; why = "a thread in an unscheduled domain still ran";
        }
    }

    /* 4. ...and back. */
    if (ok && it_invoke2((long)IT_CPTR_DOMAIN_CONTROL, INV_DOMAIN_SET,
                         tcb, 0) != 0) {
        ok = 0; why = "domain set back";
    }
    if (ok) {
        uint64_t a = g_t343_ticks;
        int moved = 0;
        for (int i = 0; i < 200 && !moved; i++) {
            it_settle(1);
            if (g_t343_ticks != a) moved = 1;
        }
        if (!moved) { ok = 0; why = "the thread did not come back with its domain"; }
    }

    g_t343_stop = 1;
    for (int i = 0; i < 400; i++) it_settle(1);
    it_quiesce_reaper();
    if (ok) it_pass("T343"); else it_fail("T343", why);
}

/* ── T344: the reap ring cannot refuse a dead task ─────────────────────────
 *
 * A thread that dies is queued for a deferred reap: the scheduler frees its
 * storage at the top of the next yield or tick, because freeing it where it
 * died would mean running a destructor on the stack being destroyed.
 *
 * The ring was sized 8 with the comment "8 > realistic concurrent deaths".
 * That is the shape of reasoning that is fine until it is not: `MAX_CPUS` is
 * also 8, a ring of 8 holds 7, and eight CPUs each with a dying task overflow
 * it by one.  The overflow path leaks the task's slot SILENTLY — and a leak
 * looks exactly like a system that has not reaped yet, so nothing would have
 * noticed for as long as it took an object count to drift.
 *
 * The capacity is derived now (one entry per CPU, plus the slot a ring needs
 * to tell full from empty) and the refusal is counted.  This asserts the count
 * is zero after the suite has created and destroyed several hundred threads —
 * which is not a proof, but it is the difference between a structural claim
 * nobody checks and one that fails loudly if the derivation is wrong.
 *
 * Invariants: O5. */
void test_t344(void) {
    it_quiesce_reaper();

    uint8_t buf[96];
    if (it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO,
                   (long)(uintptr_t)buf, 96) != 0) {
        it_fail("T344", "sched info"); return;
    }
    uint32_t drops = (uint32_t)buf[92] | ((uint32_t)buf[93] << 8) |
                     ((uint32_t)buf[94] << 16) | ((uint32_t)buf[95] << 24);
    uint32_t hwm   = (uint32_t)buf[88] | ((uint32_t)buf[89] << 8) |
                     ((uint32_t)buf[90] << 16) | ((uint32_t)buf[91] << 24);

    it_serial_write("[IRIS][TEST] T344 reap_hwm=");
    it_log_num(hwm);
    it_serial_write(" drops=");
    it_log_num(drops);
    it_serial_write("\n");

    if (drops != 0u) { it_fail("T344", "the reap ring refused a dead task"); return; }
    it_pass("T344");
}

/* ── T345: the TLB shootdown costs nothing on one core ─────────────────────
 *
 * An unmap on CPU A must reach every CPU that can still use the translation,
 * or it is not an unmap.  IRIS's shootdown targets only the CPUs whose current
 * thread is IN the address space being changed — every other CPU either never
 * returns to it, or flushes it on the way in, because a CR3 load with bit 63
 * clear invalidates the PCID it loads.
 *
 * With one CPU that set is always empty, so what this test pins is the SHAPE
 * of the mechanism rather than its cross-CPU behaviour, which cannot run yet:
 *
 *  1. `tlb_shootdown_count` is ZERO after the suite has unmapped a great many
 *     pages.  Not a formality — the target scan skips the CALLING CPU, and if
 *     it did not, a shootdown would IPI itself and then spin waiting for an
 *     acknowledgement it cannot deliver, with interrupts off.  The first unmap
 *     would hang the machine.  A zero here is the evidence that skip works;
 *  2. the LOCAL invalidation still happens, so the mechanism was added beside
 *     the existing `invlpg` rather than in place of it.  An unmap bumps the
 *     local counter, which is what makes the pair meaningful: many local
 *     invalidations, zero cross-CPU ones, is exactly the one-core regime.
 *
 * The cross-CPU half is SMP roadmap §9.3 step 3's to test, when there is a
 * second CPU to test it with.  Saying that here beats a test name implying
 * coverage that does not exist.
 * Invariants: M2. */
#define T345_FRAME IT_SCRATCH_0
#define T345_VA    (0x0000600000000000ULL + 0xC00000ULL)

void test_t345(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "one core sends no shootdown";

    if (!it_setup_self_vspace()) { it_fail("T345", "vspace self"); return; }
    it_slot_delete(T345_FRAME);

    uint8_t buf[160];
    uint32_t local0 = 0, shoot0 = 0;
    if (it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO,
                   (long)(uintptr_t)buf, 160) != 0) {
        it_fail("T345", "sched info"); return;
    }
    local0 = (uint32_t)buf[152] | ((uint32_t)buf[153] << 8) |
             ((uint32_t)buf[154] << 16) | ((uint32_t)buf[155] << 24);
    shoot0 = (uint32_t)buf[156] | ((uint32_t)buf[157] << 8) |
             ((uint32_t)buf[158] << 16) | ((uint32_t)buf[159] << 24);

    /* Map and unmap — the unmap is what calls the shootdown. */
    if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_FRAME,
                      T345_FRAME, 1u, 4096) != 0) { ok = 0; why = "retype"; }
    if (ok && it_invoke((long)T345_FRAME, INV_FRAME_MAP, IT_VS,
                        (long)T345_VA, 1) != 0) { ok = 0; why = "map"; }
    if (ok && it_invoke2((long)T345_FRAME, INV_FRAME_UNMAP, IT_VS,
                         (long)T345_VA) != 0) { ok = 0; why = "unmap"; }

    if (ok) {
        if (it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO,
                       (long)(uintptr_t)buf, 160) != 0) { ok = 0; why = "sched info 2"; }
    }
    if (ok) {
        uint32_t local1 = (uint32_t)buf[152] | ((uint32_t)buf[153] << 8) |
                          ((uint32_t)buf[154] << 16) | ((uint32_t)buf[155] << 24);
        uint32_t shoot1 = (uint32_t)buf[156] | ((uint32_t)buf[157] << 8) |
                          ((uint32_t)buf[158] << 16) | ((uint32_t)buf[159] << 24);

        it_serial_write("[IRIS][TEST] T345 local_invlpg=");
        it_log_num(local1);
        it_serial_write(" shootdowns=");
        it_log_num(shoot1);
        it_serial_write("\n");

        /* 2. the local invalidation still happens. */
        if (local1 <= local0) { ok = 0; why = "an unmap issued no local invlpg"; }
        /* 1. and nothing was sent anywhere. */
        if (ok && (shoot0 != 0u || shoot1 != 0u)) {
            ok = 0; why = "a shootdown IPI was sent with one CPU running";
        }
    }

    it_slot_delete(T345_FRAME);
    it_quiesce_reaper();
    if (ok) it_pass("T345"); else it_fail("T345", why);
}
