/*
 * it_t121_t148.c — tests T121 through T148.
 *
 * The suite's numbering is chronological, not thematic: T121 was written
 * stages before T148, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T121 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"

#include "../common/iris_msg.h"
void test_t121(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext(e0)) { it_fail("T121", "sched ext"); return; }
    int ok = 1;
    const char *why = "ipc blocking";

    /* kind 0 = EP_RECV, 1 = EP_SEND, 2 = EP_CALL — one worker each, closed. */
    for (uint32_t kind = 0; ok && kind < 3u; kind++) {
        long ep = it_ep_create();
        if (ep < 0) { ok = 0; why = "ep create"; break; }
        g_sh_ep = (handle_id_t)ep;
        g_t121_res[kind] = 999;
        g_sh_done[0] = 0;

        /* Drive one worker directly (index 0) into the chosen blocking state. */
        uint64_t entry = (uint64_t)(uintptr_t)g_t121_entries[kind];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[0] + sizeof(g_sh_stk[0]))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread create"; }

        /* let the worker reach its blocking syscall */
        if (ok) for (int y = 0; y < 40; y++) it_sys0(SYS_YIELD);
        /* close the endpoint's only handle → close op wakes the waiter CLOSED */
        it_close(&g_sh_ep);
        /* wait for the worker to observe the wake-up and record its result */
        if (ok) IT_AWAIT(g_sh_done[0], 4000);
        if (ok && !g_sh_done[0]) { ok = 0; why = "waiter not woken"; }
        if (ok && g_t121_res[kind] != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake error"; }
        it_quiesce_reaper();
    }

    /* Kill a child blocked as an EP_RECV waiter; endpoint keeps no dead waiter. */
    if (ok) {
        long ep = it_ep_create();
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t p_h  = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &p_h) < 0) { ok = 0; why = "spawn"; }
        else {
            if (it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "cmd recv"; }
            it_settle(3);
            if (ok && it_kill((long)p_h) != 0) { ok = 0; why = "kill"; }
            if (ok) {
                struct iris_msg p;
                iris_msg_zero(&p);
                p.label = 0x121;
                if (iris_msg_nb_send((long)ep_h, &p) != (long)IRIS_ERR_WOULD_BLOCK) {
                    ok = 0; why = "dead waiter";
                }
            }
        }
        it_close(&p_h);
        it_close(&ep_h);
        it_quiesce_reaper();
    }

    if (ok && (!it_task_live(&tl_after) || !it_sched_ext(e1))) { ok = 0; why = "sched ext 2"; }
    if (ok && tl_after != tl_before)                    { ok = 0; why = "task-live drift"; }
    if (ok && e1[IT_SI_PROCLIVE] != e0[IT_SI_PROCLIVE]) { ok = 0; why = "proc-live drift"; }
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY])       { ok = 0; why = "ghost kreply"; }
    if (ok && e1[IT_SI_REAPHWM] >= 8u)                  { ok = 0; why = "reap backlog"; }

    if (ok) it_pass("T121"); else it_fail("T121", why);
}
void test_t122(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t s2b[4], s2a[4];
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext2(s2b) || !it_sched_ext(e0)) {
        it_fail("T122", "sched ext"); return;
    }
    (void)e0;
    int ok = 1;
    const char *why = "fairness";

    g_sh_mode  = SH_MODE_SPIN;
    g_sh_iters = T122_ITERS;
    if (!sh_start(SH_NWORK)) { ok = 0; why = "thread create"; }
    if (ok && !sh_wait_all(SH_NWORK)) { ok = 0; why = "worker stuck"; }

    /* No starvation: every cooperative worker ran to completion. */
    for (uint32_t w = 0; ok && w < SH_NWORK; w++)
        if (g_sh_prog[w] != T122_ITERS) { ok = 0; why = "starved worker"; }

    it_quiesce_reaper();
    if (ok && (!it_task_live(&tl_after) || !it_sched_ext2(s2a) || !it_sched_ext(e1))) {
        ok = 0; why = "sched ext 2";
    }
    if (ok && (s2a[IT_S2_YIELD] - s2b[IT_S2_YIELD]) < SH_NWORK * T122_ITERS) {
        ok = 0; why = "yield accounting";
    }
    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }

    if (ok) it_pass("T122"); else it_fail("T122", why);
}

/* ── T123: Scheduling Context lifetime and cleanup ──────────────────────────
 * Exercises the full KSchedContext lifecycle and every documented failure path,
 * then proves the live-SC object count returns exactly to baseline — no leak,
 * no double free, no stale ref surviving a dead task.
 *   Happy path: create → configure(valid) → bind(main) → rebind(main) →
 *     unbind(main) → unbind again (idempotent); a worker thread binds an SC and
 *     self-exits, so the deferred reaper is the one that drops the SC's task ref
 *     (reap_dead_task_off_cpu → task_release_sched_ctx — the same helper the
 *     external-kill path uses).
 *   Failure paths: budget 0, period 0, budget==period, budget>period (all
 *     INVALID_ARG); wrong object type (endpoint handle → INVALID_ARG); missing
 *     RIGHT_WRITE (read-only dup → ACCESS_DENIED); THREAD_SET_SC on a bogus
 *     handle (rejected, no stale ref).
 * Invariants: S8, S9, S10, S11. */
void test_t123(void) {
    uint32_t s2b[4], s2a[4];
    it_quiesce_reaper();
    if (!it_sched_ext2(s2b)) { it_fail("T123", "sched ext"); return; }
    uint32_t sc_base = s2b[IT_S2_SCLIVE];
    int ok = 1;
    const char *why = "sc lifetime";

    /* Phase S2: SYS_SC_CREATE retired; SCs come from Untyped RETYPE2. */
    long a = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_SCHED_CONTEXT, 0);
    long b = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_SCHED_CONTEXT, 0);
    handle_id_t sc  = (a >= 0) ? (handle_id_t)a : HANDLE_INVALID;
    handle_id_t sc2 = (b >= 0) ? (handle_id_t)b : HANDLE_INVALID;
    if (sc == HANDLE_INVALID || sc2 == HANDLE_INVALID) { ok = 0; why = "sc create"; }

    /* live count reflects two fresh SC objects. */
    if (ok && !it_sched_ext2(s2a)) { ok = 0; why = "ext mid"; }
    if (ok && s2a[IT_S2_SCLIVE] != sc_base + 2u) { ok = 0; why = "sc not counted"; }

    /* SC_CONFIGURE validation (S10). */
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0)   { ok = 0; why = "configure valid"; }
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 0, 100, (long)IRIS_CPTR_SCHED_CONTROL) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget 0"; }
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 10, 0, (long)IRIS_CPTR_SCHED_CONTROL)  != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "period 0"; }
    /* Phase S2 (fix I1): budget == period is a full CPU reservation, VALID
     * (MCS style).  Only budget > period is rejected. */
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 100, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "budget==period rejected"; }
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "reconfigure back"; }
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 200, 100, (long)IRIS_CPTR_SCHED_CONTROL) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget>period"; }

    /* Wrong object type: an endpoint handle is not a SchedContext. */
    if (ok) {
        long ep = it_ep_create();
        handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        if (ep_h == HANDLE_INVALID) { ok = 0; why = "ep create"; }
        if (ok && it_invoke((long)ep_h, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "wrong type";
        }
        if (ok && it_invoke0((long)ep_h, INV_SC_SET_ON_CALLER) != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "set_sc wrong type";
        }
        it_close(&ep_h);
    }

    /* Missing RIGHT_WRITE: a read-only dup cannot configure (S11 rights). */
    if (ok) {
        long ro = it_cs_reduce((long)sc, RIGHT_READ);
        handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
        if (ro_h == HANDLE_INVALID) { ok = 0; why = "ro dup"; }
        if (ok && it_invoke((long)ro_h, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "rights not enforced";
        }
        it_close(&ro_h);
    }

    /* Bind / rebind / unbind on the main thread (S11 — no stale ref). */
    if (ok && it_invoke0((long)sc, INV_SC_SET_ON_CALLER)  != 0) { ok = 0; why = "bind"; }
    if (ok && it_invoke0((long)sc2, INV_SC_SET_ON_CALLER) != 0) { ok = 0; why = "rebind"; }
    if (ok && it_invoke0(0, INV_SC_SET_ON_CALLER)         != 0) { ok = 0; why = "unbind"; }
    if (ok && it_invoke0(0, INV_SC_SET_ON_CALLER)         != 0) { ok = 0; why = "unbind idempotent"; }

    /* A worker thread binds an SC and self-exits; the reaper releases the ref. */
    if (ok) {
        g_sh_mode = SH_MODE_SC;
        g_sh_sc   = sc;
        g_sh_done[0] = 0; g_sh_prog[0] = 0;
        uint64_t entry = (uint64_t)(uintptr_t)g_sh_entries[0];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[0] + sizeof(g_sh_stk[0]))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "sc worker create"; }
        if (ok) IT_AWAIT(g_sh_done[0], 4000);
        if (ok && !g_sh_done[0]) { ok = 0; why = "sc worker stuck"; }
        it_quiesce_reaper();
    }

    /* Close both SC handles; with the worker reaped and main unbound, every ref
     * is gone and both objects must be destroyed → live back to baseline. */
    it_close(&sc);
    it_close(&sc2);
    it_quiesce_reaper();

    if (ok && !it_sched_ext2(s2a)) { ok = 0; why = "ext final"; }
    if (ok && s2a[IT_S2_SCLIVE] != sc_base) { ok = 0; why = "sc leak/double-free"; }

    if (ok) it_pass("T123"); else it_fail("T123", why);
}

/* ── T124: scheduler SMP-readiness audit ────────────────────────────────────
 * Not an SMP implementation — a codified audit that the scheduler's current
 * single-core assumptions still hold and are locked against silent drift.  It
 * checks, at runtime, the invariants that a future SMP port MUST revisit, using
 * only observable diagnostics:
 *   - the deferred-reap queue never approached its bound (the "one death per
 *     yield interval" single-CPU assumption held — SMP would need per-CPU dead
 *     lists);
 *   - the duplicate-enqueue guard is the single point that enforces "a task is
 *     in the run queue at most once"; under SMP the same guard must be held
 *     under the target CPU's run-queue lock (documented, asserted-reachable);
 *   - run-queue depth stayed within TASK_MAX (a single global run queue today);
 *   - the live task and live SC counts are internally consistent.
 * The authoritative list of single-core assumptions and required-before-SMP
 * work lives in docs/architecture/scheduler-hardening.md; this test is its
 * runtime tripwire.  Invariants: S4, S6, S16. */
void test_t124(void) {
    uint32_t e[14];
    uint32_t s2[4];
    uint32_t tl = 0;
    it_quiesce_reaper();
    if (!it_task_live(&tl) || !it_sched_ext(e) || !it_sched_ext2(s2)) {
        it_fail("T124", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "smp-readiness";

    /* Single-CPU deferred-reap assumption: depth never neared REAP_QUEUE_SIZE
     * (8).  Approaching it would mean multiple concurrent deaths per yield
     * interval — impossible on one CPU, mandatory to redesign for SMP. */
    if (ok && e[IT_SI_REAPHWM] >= 8u) { ok = 0; why = "reap-queue bound (SMP risk)"; }

    /* Run-queue depth fits the single global queue (≤ TASK_MAX). */
    if (ok && s2[IT_S2_RQHWM] > TASK_MAX_HINT) { ok = 0; why = "rq depth > TASK_MAX"; }

    /* The S4 guard must remain the one enforcement point for "at most once in
     * the run queue".  Its counter must be readable (reachable) — under SMP it
     * has to move to the per-CPU run-queue lock; here we assert the mechanism
     * exists and is wired to the diagnostics so a regression is visible. */
    if (ok && (s2[IT_S2_DUPENQ] == 0xFFFFFFFFu)) { ok = 0; why = "dup-enqueue counter unwired"; }

    /* Live counts are internally consistent: at least the idle task + this test
     * process's threads are alive, and SC live is a sane small number. */
    if (ok && tl < 1u) { ok = 0; why = "task-live underflow"; }
    if (ok && s2[IT_S2_SCLIVE] > TASK_MAX_HINT) { ok = 0; why = "sc-live implausible"; }

    if (ok) it_pass("T124"); else it_fail("T124", why);
}
static long g_it_auth_ut = -1;
long it_auth_ut(void) {
    if (g_it_auth_ut < 0) {
        it_slot_delete((uint32_t)IT_AUTH_UT_CPTR);
        long r = it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_UNTYPED | (1ULL << 32)), (long)((uint64_t)IT_OBJ_CNODE_SLOT | (244ULL << 32)), 2097152);
        if (r == 0) g_it_auth_ut = (long)IT_AUTH_UT_CPTR;
    }
    return g_it_auth_ut;
}

/* Reset the authority-test untyped after a test drops all its children.
 * Returns 1 if the region is clean (child_count == 0 → RESET ok), 0 otherwise. */
int it_ut_reset(void) {
    return it_invoke0(IT_UT, INV_UNTYPED_RESET) == 0;
}

/* ── T125: basic untyped retype authority ───────────────────────────────────
 * Retype every ring-3-supported object kind from a valid KUntyped, prove each
 * exists (type-check + a type-appropriate use), then destroy them and confirm
 * the region resets (child_count back to 0) and every live per-type count
 * returns to baseline.  Failure paths: wrong/unsupported type, invalid object
 * size, non-power-of-two CNode slot count, and retype through a cap lacking
 * RIGHT_WRITE (ACCESS_DENIED, no object born).
 * Invariants: U1, U2, U3, U6, U17, U18, U20. */
void test_t125(void) {
    uint32_t s3b[6], s3a[6];
    /* Materialize the lazy authority sub-untyped BEFORE the baseline so its
     * +1 untyped does not pollute this test's live-count deltas. */
    if (it_auth_ut() < 0) { it_fail("T125", "auth untyped carve"); return; }
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b)) { it_fail("T125", "sched ext3"); return; }
    int ok = 1;
    const char *why = "retype authority";

    /* The forwarded untyped must be present and non-trivially sized. */
    uint64_t avail = 0;
    if (it_invoke2(IT_UT, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&avail) != 0) {
        it_fail("T125", "untyped absent (boot-chain forward failed)"); return;
    }
    if (avail < 65536u) { it_fail("T125", "untyped too small"); return; }

    handle_id_t ep = HANDLE_INVALID, nt = HANDLE_INVALID, cn = HANDLE_INVALID;
    handle_id_t sc = HANDLE_INVALID, fr = HANDLE_INVALID, sub = HANDLE_INVALID;

    long r;
    r = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (r < 0) { ok = 0; why = "retype endpoint"; } else ep = (handle_id_t)r;
    r = it_retype_slot_alloc(IT_UT, IT_KOBJ_NOTIFICATION, 0);
    if (ok && r < 0) { ok = 0; why = "retype notification"; } else if (ok) nt = (handle_id_t)r;
    r = it_retype_slot_alloc(IT_UT, IT_KOBJ_CNODE, 4);
    if (ok && r < 0) { ok = 0; why = "retype cnode"; } else if (ok) cn = (handle_id_t)r;
    r = it_retype_slot_alloc(IT_UT, IT_KOBJ_SCHED_CONTEXT, 0);
    if (ok && r < 0) { ok = 0; why = "retype sc"; } else if (ok) sc = (handle_id_t)r;
    r = it_frame_create_slot(IT_UT, 4096);
    if (ok && r < 0) { ok = 0; why = "retype frame"; } else if (ok) fr = (handle_id_t)r;
    r = it_retype_slot_alloc(IT_UT, IT_KOBJ_UNTYPED, 4096);
    if (ok && r < 0) { ok = 0; why = "retype sub-untyped"; } else if (ok) sub = (handle_id_t)r;

    /* Each object exists and has the expected type, every one of them asked of
     * the slot it was born into. */
    if (ok && it_invoke0((long)ep, INV_CAP_IDENTIFY)  != (long)IT_KOBJ_ENDPOINT)     { ok = 0; why = "ep type"; }
    if (ok && it_invoke0((long)nt, INV_CAP_IDENTIFY)  != (long)IT_KOBJ_NOTIFICATION) { ok = 0; why = "nt type"; }
    if (ok && it_invoke0((long)cn, INV_CAP_IDENTIFY)  != (long)IT_KOBJ_CNODE)        { ok = 0; why = "cn type"; }
    if (ok && it_invoke0((long)sc, INV_CAP_IDENTIFY)  != (long)IT_KOBJ_SCHED_CONTEXT){ ok = 0; why = "sc type"; }
    if (ok && it_invoke0((long)fr, INV_CAP_IDENTIFY)  != (long)IT_KOBJ_FRAME)        { ok = 0; why = "fr type"; }
    if (ok && it_invoke0((long)sub, INV_CAP_IDENTIFY) != (long)IT_KOBJ_UNTYPED)      { ok = 0; why = "sub type"; }

    /* Type-appropriate use: the retyped endpoint is a real rendezvous point
     * (empty → WOULD_BLOCK); the sub-untyped answers INFO; the SC configures. */
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m);
        if (iris_msg_nb_recv((long)ep, &m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "ep not usable";
        }
    }
    if (ok && it_invoke2((long)sub, INV_UNTYPED_INFO, 0, 0) != 0) { ok = 0; why = "sub not usable"; }
    if (ok && it_invoke((long)sc, INV_SC_CONFIGURE, 10, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "sc not usable"; }

    /* Live per-type counts rose by exactly the objects we made. */
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 mid"; }
    if (ok && s3a[IT_S3_EP]      != s3b[IT_S3_EP]      + 1u) { ok = 0; why = "ep live"; }
    if (ok && s3a[IT_S3_NOTIF]   != s3b[IT_S3_NOTIF]   + 1u) { ok = 0; why = "nt live"; }
    if (ok && s3a[IT_S3_CNODE]   != s3b[IT_S3_CNODE]   + 1u) { ok = 0; why = "cn live"; }
    if (ok && s3a[IT_S3_FRAME]   != s3b[IT_S3_FRAME]   + 1u) { ok = 0; why = "fr live"; }
    /* +1 sub-untyped (the parent stays counted the whole time). */
    if (ok && s3a[IT_S3_UNTYPED] != s3b[IT_S3_UNTYPED] + 1u) { ok = 0; why = "sub live"; }

    /* ── Failure paths (no object may be born) ── */
    if (ok) {
        uint32_t f0[6], f1[6];
        if (!it_sched_ext3(f0)) { ok = 0; why = "ext3 fail-base"; }
        /* wrong/unsupported type (KOBJ_PROCESS = 0). */
        if (ok && it_retype_slot_alloc(IT_UT, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "wrong type"; }
        /* invalid frame size (not page-aligned). */
        if (ok && it_retype_slot_alloc(IT_UT, IT_KOBJ_FRAME, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad frame size"; }
        /* invalid sub-untyped size (< 4096). */
        if (ok && it_retype_slot_alloc(IT_UT, IT_KOBJ_UNTYPED, 100) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad ut size"; }
        /* non-power-of-two CNode slot count (RETYPE2 — the canonical path). */
        if (ok && it_retype2_at(IT_UT, IT_KOBJ_CNODE, 240u, 1u, 3) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad cnode slots"; }
        /* Stage 4: the LEGACY handle-publishing retype (87) is retired for
         * EVERY type, not just the migrated family it refused since Phase S1.
         * There is one way to create an object from an Untyped, and it puts
         * the result in a CSpace slot. */
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_ENDPOINT, 0)     != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy ep not retired"; }
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_FRAME, 4096)     != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy frame not retired"; }
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_UNTYPED, 4096)   != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy ut not retired"; }
        if (ok && it_sys3(SYS_UNTYPED_RETYPE, IT_UT, IT_KOBJ_SCHED_CONTEXT, 0)!= (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy sc not retired"; }
        /* missing RIGHT_WRITE: retype through a read-only derived cap. */
        if (ok) {
            /* Phase S4 (Step 3): the read-only copy is a native-CDT child. */
            long ut_root = it_cdt_root((handle_id_t)IT_UT, IT_SCRATCH_0);
            long ro = (ut_root >= 0)
                    ? it_cdt_derive(ut_root, IT_SCRATCH_1, RIGHT_READ) : -1;
            if (ro < 0) { ok = 0; why = "ro derive"; }
            if (ok && it_retype2_at(ro, IT_KOBJ_ENDPOINT, 240u, 1u, 0) != (long)IRIS_ERR_ACCESS_DENIED) {
                ok = 0; why = "rights not enforced";
            }
            it_slot_delete(IT_SCRATCH_1);
            it_slot_delete(IT_SCRATCH_0);
        }
        /* No object leaked through any failure path. */
        if (ok && !it_sched_ext3(f1)) { ok = 0; why = "ext3 fail-after"; }
        /* 6 words since Stage 7-mem: the live-VMO count joined the per-type
         * gauges, so a failure path that leaked one is caught here too. */
        for (uint32_t i = 0; ok && i < 6u; i++)
            if (f1[i] != f0[i]) { ok = 0; why = "failure leaked object"; }
    }

    /* Destroy everything and prove the region is clean (child_count → 0). */
    it_close(&ep); it_close(&nt); it_close(&cn);
    it_close(&sc); it_close(&fr); it_close(&sub);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (child leak)"; }
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    for (uint32_t i = 0; ok && i < 5u; i++)
        if (s3a[i] != s3b[i]) { ok = 0; why = "object leak"; }

    if (ok) it_pass("T125"); else it_fail("T125", why);
}

/* ── T126: retype failure atomicity ─────────────────────────────────────────
 * Drive retype into each failure mode and prove it is atomic: no object is
 * born, no handle appears, no live count moves, the region stays resettable,
 * and a valid retype right after each failure still works.
 * Invariants: U5, U17, U18, U19. */
void test_t126(void) {
    uint32_t s3b[6], s3a[6];
    uint32_t hlb[14], hla[14];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b) || !it_sched_ext(hlb)) { it_fail("T126", "sched ext"); return; }
    int ok = 1;
    const char *why = "retype atomicity";

    /* NO_MEMORY: a sub-untyped larger than the whole region. */
    uint64_t avail = 0;
    (void)it_invoke2(IT_UT, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&avail);
    uint64_t huge = (avail + 0x100000u) & ~0xFFFULL;   /* page-aligned, > avail */
    struct { uint32_t type; uint64_t arg; long expect; const char *tag; } cases[] = {
        { IT_KOBJ_UNTYPED,     huge, (long)IRIS_ERR_NO_MEMORY,     "nomem" },
        { 99u,                 0,    (long)IRIS_ERR_NOT_SUPPORTED, "badtype" },
        { IT_KOBJ_FRAME,       1u,   (long)IRIS_ERR_INVALID_ARG,   "badsize" },
        /* Was "the LEGACY path refuses KOBJ_CNODE".  With 87 retired there is
         * only RETYPE2, which accepts CNode but not a non-power-of-two slot
         * count — so the same input is still a clean rejection, under the
         * rule that actually applies to it. */
        { IT_KOBJ_CNODE,       7u,   (long)IRIS_ERR_INVALID_ARG,   "cnode bad slot count" },
    };
    for (uint32_t c = 0; ok && c < 4u; c++) {
        long rr = it_retype_slot_alloc(IT_UT, cases[c].type, (long)cases[c].arg);
        if (rr != cases[c].expect) { ok = 0; why = cases[c].tag; break; }
        /* nothing leaked after this failure */
        if (!it_sched_ext3(s3a) || !it_sched_ext(hla)) { ok = 0; why = "ext mid"; break; }
        for (uint32_t i = 0; ok && i < 5u; i++)
            if (s3a[i] != s3b[i]) { ok = 0; why = "leaked object"; }
        if (ok && hla[IT_SI_LIVE] != hlb[IT_SI_LIVE]) { ok = 0; why = "leaked handle"; }
        /* a valid retype right after the failure still works */
        if (ok) {
            long good = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
            if (good < 0) { ok = 0; why = "valid-after-fail"; }
            else { handle_id_t g = (handle_id_t)good; it_close(&g); }
        }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    for (uint32_t i = 0; ok && i < 5u; i++)
        if (s3a[i] != s3b[i]) { ok = 0; why = "final leak"; }

    if (ok) it_pass("T126"); else it_fail("T126", why);
}

/* ── T127: revoke derivation-tree cascade ───────────────────────────────────
 * Build a handle-table derivation tree from a retyped endpoint (root → child →
 * grandchild, plus a second branch) and revoke the root: every descendant dies
 * (BAD_HANDLE), the root survives, an unrelated object outside the subtree is
 * untouched, and a repeated revoke is idempotent.  Failure paths: revoke on a
 * stale handle (BAD_HANDLE), revoke of a leaf with no children (idempotent 0).
 * Also asserts revoke SCOPE: a copy minted into a CNode is an independent ref,
 * not a derivation child, and survives the revoke — then is cleaned up.
 * Invariants: U8, U9, U10, U16. */
void test_t127(void) {
    uint32_t s3b[6];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b)) { it_fail("T127", "sched ext3"); return; }
    int ok = 1;
    const char *why = "revoke cascade";

    long rr = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (rr < 0) { it_fail("T127", "retype root"); return; }
    handle_id_t root = (handle_id_t)rr;

    /* Unrelated object outside the derivation subtree (must survive revoke). */
    long orr = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
    handle_id_t outsider = (orr >= 0) ? (handle_id_t)orr : HANDLE_INVALID;
    if (orr < 0) { ok = 0; why = "retype outsider"; }

    /* Phase S4 (Step 3): the derivation tree is the NATIVE CDT over slots.
     * root → c1 → gc1 ; root → c2. */
    long rootc = ok ? it_cdt_root(root, IT_SCRATCH_0) : -1;
    if (ok && rootc < 0) { ok = 0; why = "root slot"; }
    long c1  = (rootc >= 0) ? it_cdt_derive(rootc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
    long gc1 = (c1 >= 0)    ? it_cdt_derive(c1,    IT_SCRATCH_2, RIGHT_SAME_RIGHTS) : -1;
    long c2  = (rootc >= 0) ? it_cdt_derive(rootc, IT_SCRATCH_3, RIGHT_SAME_RIGHTS) : -1;
    if (c1 < 0 || gc1 < 0 || c2 < 0) { ok = 0; why = "derive"; }

    /* The "a SYS_CNODE_MINT copy is an independent ref, not a derivation
     * child, and survives the revoke" leg is retired with that syscall: it
     * asserted the MDB LEGACY_ROOT behaviour the ledger tracks to zero.  Every
     * copy is a derivation child now, which is what the rest of this test
     * checks. */

    /* All derived caps are live before the revoke. */
    if (ok && !it_cdt_alive(c1))  { ok = 0; why = "c1 dead early"; }
    if (ok && !it_cdt_alive(gc1)) { ok = 0; why = "gc1 dead early"; }
    if (ok && !it_cdt_alive(c2))  { ok = 0; why = "c2 dead early"; }

    /* Revoke root's subtree. */
    if (ok && it_cdt_revoke(rootc) < 0) { ok = 0; why = "revoke"; }

    /* Every descendant is gone; the invoked slot survives. */
    if (ok && it_cdt_alive(c1))  { ok = 0; why = "c1 alive"; }
    if (ok && it_cdt_alive(gc1)) { ok = 0; why = "gc1 alive"; }
    if (ok && it_cdt_alive(c2))  { ok = 0; why = "c2 alive"; }
    if (ok && !it_cdt_alive(rootc)) { ok = 0; why = "root died"; }
    if (ok && it_invoke0((long)root, INV_CAP_IDENTIFY) < 0) { ok = 0; why = "root cap died"; }
    /* Outsider outside the subtree is untouched. */
    if (ok && it_invoke0((long)outsider, INV_CAP_IDENTIFY) < 0) { ok = 0; why = "outsider died"; }

    /* Idempotent: a second revoke finds an empty subtree and succeeds. */
    if (ok && it_cdt_revoke(rootc) < 0) { ok = 0; why = "revoke not idempotent"; }
    /* Revoke on an EMPTY slot fails cleanly. */
    if (ok) {
        it_slot_delete(IT_SCRATCH_1);
        if (it_cdt_revoke((long)IT_SCRATCH_1) >= 0) { ok = 0; why = "empty revoke ok"; }
    }

    /* Teardown: release the scratch slots, then close root and outsider. */
    it_slot_delete(IT_SCRATCH_0);
    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_2);
    it_slot_delete(IT_SCRATCH_3);
    it_close(&root);
    it_close(&outsider);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }
    uint32_t s3a[6];
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    for (uint32_t i = 0; ok && i < 5u; i++)
        if (s3a[i] != s3b[i]) { ok = 0; why = "object leak"; }

    if (ok) it_pass("T127"); else it_fail("T127", why);
}

/* ── T128: frame authority lifetime + revoke (VSpace-map gap documented) ─────
 * A retyped KFrame's cap authority is exercised: derive a child handle, revoke
 * it (child dies, the frame object survives via the root), then release the
 * root and prove the frame is destroyed exactly once (frame_live baseline) with
 * the region resettable (child_count → 0).
 *
 * Documented gap (see the hardening doc): ring 3 cannot MAP a retyped frame
 * here — SYS_FRAME_MAP needs a VSpace cap by CPtr that iris_test is not granted
 * (VMO is the ring-3 mapping path).  The PTE-install / mapped_count / unmap
 * invariants are covered by the host KFrame suites; and by design SYS_CAP_REVOKE
 * is cap-scoped and never force-unmaps a frame (a live mapping holds an
 * independent ref; kframe_obj_destroy asserts mapped_count == 0, so a stale PTE
 * can never outlive the frame object).
 * Invariants: U10, U13, U17, U18. */
void test_t128(void) {
    uint32_t s3b[6];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b)) { it_fail("T128", "sched ext3"); return; }
    int ok = 1;
    const char *why = "frame lifetime";

    long fr = it_frame_create_slot(IT_UT, 4096);
    if (fr < 0) { it_fail("T128", "retype frame"); return; }
    handle_id_t frame = (handle_id_t)fr;

    uint32_t mid[6];
    if (!it_sched_ext3(mid)) { ok = 0; why = "ext3 mid"; }
    if (ok && mid[IT_S3_FRAME] != s3b[IT_S3_FRAME] + 1u) { ok = 0; why = "frame not counted"; }

    /* Phase S4 (Step 3): derive a child frame through the native CDT, then
     * revoke it away from the root slot. */
    long frc   = ok ? it_cdt_root(frame, IT_SCRATCH_0) : -1;
    if (ok && frc < 0) { ok = 0; why = "root slot"; }
    long child = (frc >= 0) ? it_cdt_derive(frc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
    if (ok && child < 0) { ok = 0; why = "derive"; }
    if (ok && it_invoke0(child, INV_CAP_IDENTIFY) != (long)IT_KOBJ_FRAME) {
        ok = 0; why = "child type";
    }
    if (ok && it_cdt_revoke(frc) < 0) { ok = 0; why = "revoke"; }
    if (ok && it_cdt_alive(child)) { ok = 0; why = "child alive"; }
    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_0);
    /* The frame object survives while the root cap is held. */
    if (ok && it_invoke0((long)frame, INV_CAP_IDENTIFY) != (long)IT_KOBJ_FRAME) { ok = 0; why = "frame died early"; }
    if (ok && !it_sched_ext3(mid)) { ok = 0; why = "ext3 mid2"; }
    if (ok && mid[IT_S3_FRAME] != s3b[IT_S3_FRAME] + 1u) { ok = 0; why = "frame miscounted after revoke"; }

    /* Release the root: the frame is destroyed exactly once (unmapped: no
     * mapping ever taken, so mapped_count == 0 → clean destroy). */
    it_close(&frame);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame child leak)"; }
    uint32_t s3a[6];
    if (ok && !it_sched_ext3(s3a)) { ok = 0; why = "ext3 final"; }
    if (ok && s3a[IT_S3_FRAME] != s3b[IT_S3_FRAME]) { ok = 0; why = "frame leak/double-free"; }

    if (ok) it_pass("T128"); else it_fail("T128", why);
}

/* ── T129: revoke with IPC-visible objects ──────────────────────────────────
 * A worker thread blocks in EP_RECV on a RETYPED endpoint.  A derived child
 * handle is revoked (the endpoint object survives), then the last handle is
 * closed: the endpoint's close fires, the waiter wakes with IRIS_ERR_CLOSED,
 * and after the worker releases its ref the endpoint object is destroyed
 * (endpoint_live baseline), the region resets clean, no ghost KReply is minted,
 * and task/handle books return to baseline.
 * Invariants: U8, U13 (via S13/S14), U14, U15, U17. */
static volatile long g_t129_res;
static void t129_worker(void) {
    struct iris_msg m; iris_msg_zero(&m);
    g_t129_res = iris_msg_recv((long)g_sh_ep, &m);
    g_sh_done[0] = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void test_t129(void) {
    uint32_t s3b[6], s3a[6];
    uint32_t e0[14], e1[14];
    uint32_t tl0 = 0, tl1 = 0;
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b) || !it_sched_ext(e0) || !it_task_live(&tl0)) {
        it_fail("T129", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "revoke ipc object";

    long er = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (er < 0) { it_fail("T129", "retype endpoint"); return; }
    g_sh_ep = (handle_id_t)er;
    {
        uint32_t mid[6];
        if (!it_sched_ext3(mid) || mid[IT_S3_EP] != s3b[IT_S3_EP] + 1u) {
            ok = 0; why = "endpoint not counted";
        }
    }
    g_sh_done[0] = 0; g_t129_res = 999;

    /* Worker blocks in EP_RECV on the retyped endpoint. */
    uint64_t entry = (uint64_t)(uintptr_t)t129_worker;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[0] + sizeof(g_sh_stk[0]))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread create"; }
    if (ok) for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);

    /* Phase S4: derive a child through the native CDT and revoke it — the
     * OBJECT survives (the root slot still names it) and the blocked waiter
     * is unaffected: revocation removes capabilities, not execution. */
    long epc   = ok ? it_cdt_root(g_sh_ep, IT_SCRATCH_0) : -1;
    if (ok && epc < 0) { ok = 0; why = "root slot"; }
    long child = (epc >= 0) ? it_cdt_derive(epc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
    if (ok && child < 0) { ok = 0; why = "derive"; }
    if (ok && it_cdt_revoke(epc) < 0) { ok = 0; why = "revoke"; }
    if (ok && it_cdt_alive(child)) { ok = 0; why = "child alive"; }
    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_0);

    /* Close the last handle → endpoint close fires → waiter wakes CLOSED. */
    it_close(&g_sh_ep);
    if (ok) IT_AWAIT(g_sh_done[0], 4000);
    if (ok && !g_sh_done[0]) { ok = 0; why = "waiter not woken"; }
    if (ok && g_t129_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake error"; }

    it_quiesce_reaper();
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext(e1) || !it_task_live(&tl1))) { ok = 0; why = "ext2"; }
    if (ok && s3a[IT_S3_EP] != s3b[IT_S3_EP]) { ok = 0; why = "endpoint leak"; }
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY]) { ok = 0; why = "ghost kreply"; }
    if (ok && tl1 != tl0) { ok = 0; why = "task-live drift"; }
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }

    if (ok) it_pass("T129"); else it_fail("T129", why);
}

/* ── T130: rights monotonicity and cap derivation ───────────────────────────
 * Rights can only shrink along a derivation/mint chain, never grow, and there
 * is no fallback after ACCESS_DENIED.
 * Invariants: U7, U20 (+ U16). */
void test_t130(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "rights monotonicity";

    long rr = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
    if (rr < 0) { it_fail("T130", "retype root"); return; }
    handle_id_t root = (handle_id_t)rr;   /* full rights: READ|WRITE|DUP|TRANSFER */

    /* Phase S4 (Step 3): derivation is the native CDT.  Derive down to
     * READ-only (drops DUPLICATE). */
    long rootc = it_cdt_root(root, IT_SCRATCH_0);
    if (rootc < 0) { ok = 0; why = "root slot"; }
    long ro = (rootc >= 0) ? it_cdt_derive(rootc, IT_SCRATCH_1, RIGHT_READ) : -1;
    if (ro < 0) { ok = 0; why = "derive ro"; }

    /* A cap without DUPLICATE cannot be a derivation source — ACCESS_DENIED,
     * and nothing is installed (no fallback). */
    if (ok && it_invoke2(ro, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_2 << 32), (long)RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "escalation via derive";
    }
    if (ok && it_cdt_alive((long)IT_SCRATCH_2)) { ok = 0; why = "denied derive installed"; }

    /* Derivation cannot ADD rights: asking for FULL from a READ-only parent
     * yields a cap that still lacks WRITE (monotonic reduce).  We prove the
     * child cannot be a derivation source (no DUPLICATE) — i.e. WRITE/DUP were
     * NOT granted despite the request. */
    if (ok) {
        /* From a full-rights parent, derive asking only READ → child has READ
         * only, so it cannot itself be a derivation source. */
        long up = it_cdt_derive(rootc, IT_SCRATCH_3, RIGHT_READ);
        if (up < 0) { ok = 0; why = "derive read"; }
        if (ok && it_invoke2(up, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_2 << 32), (long)RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "read child escalated";
        }
        it_slot_delete(IT_SCRATCH_3);
    }

    /* Minting into a CNode reduces rights and never amplifies.  Asked of the
     * CSpace form: a request for a right the source lacks collapses to none
     * and is rejected, rather than quietly granting it. */
    if (ok) {
        long cnr = it_retype_slot_alloc(IT_UT, IT_KOBJ_CNODE, 4);
        handle_id_t cn = (cnr >= 0) ? (handle_id_t)cnr : HANDLE_INVALID;
        if (cnr < 0) { ok = 0; why = "retype cnode"; }
        if (ok && it_invoke2(rootc, INV_CSPACE_MINT, (long)(((uint64_t)2u << 32) | (uint64_t)cnr), (long)RIGHT_READ) != 0) {
            ok = 0; why = "cnode mint";
        }
        if (ok && it_invoke2(rootc, INV_CSPACE_MINT, (long)(((uint64_t)3u << 32) | (uint64_t)cnr), (long)RIGHT_MANAGE) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "mint amplified";
        }
        it_close(&cn);
    }

    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_0);
    it_close(&root);
    it_quiesce_reaper();
    (void)it_ut_reset();

    if (ok) it_pass("T130"); else it_fail("T130", why);
}
void test_t131(void) {
    uint32_t s3b[6], s3a[6];
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_sched_ext3(s3b) || !it_sched_ext(e0)) { it_fail("T131", "sched ext"); return; }
    g_fz_seed = T131_SEED;
    int ok = 1;
    const char *why = "untyped/revoke stress";
    uint32_t i = 0;

    for (i = 0; ok && i < T131_ROUNDS; i++) {
        uint32_t pick = fz_rand() % 3u;
        uint32_t type = (pick == 0u) ? IT_KOBJ_ENDPOINT
                      : (pick == 1u) ? IT_KOBJ_NOTIFICATION
                                     : IT_KOBJ_CNODE;
        uint64_t arg  = (type == IT_KOBJ_CNODE) ? 4u : 0u;

        long rr = it_retype_slot_alloc(IT_UT, type, (long)arg);
        if (rr < 0) { ok = 0; why = "retype"; break; }
        handle_id_t root = (handle_id_t)rr;

        /* Phase S4 (Step 3): derive a small tree through the native CDT,
         * sometimes revoke it, always tear it down. */
        long rootc = it_cdt_root(root, IT_SCRATCH_0);
        if (rootc < 0) { ok = 0; why = "root slot"; it_close(&root); break; }
        long c1 = it_cdt_derive(rootc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS);
        long c2 = (c1 >= 0) ? it_cdt_derive(c1, IT_SCRATCH_2, RIGHT_SAME_RIGHTS) : -1;

        /* Forced failure paths, interleaved deterministically. */
        if ((fz_rand() & 1u) &&
            it_retype_slot_alloc(IT_UT, 99, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "badtype"; }
        if (ok && (fz_rand() & 1u) &&
            it_retype_slot_alloc(IT_UT, IT_KOBJ_FRAME, 7) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "badsize"; }

        if (ok && (fz_rand() & 1u)) {
            /* Revoke path: descendants die, the invoked slot survives. */
            if (it_cdt_revoke(rootc) < 0) { ok = 0; why = "revoke"; }
            if (ok && c1 >= 0 && it_cdt_alive(c1)) { ok = 0; why = "c1 alive"; }
            if (ok && c2 >= 0 && it_cdt_alive(c2)) { ok = 0; why = "c2 alive"; }
            /* Repeated revoke is idempotent. */
            if (ok && it_cdt_revoke(rootc) < 0) { ok = 0; why = "revoke idem"; }
        } else {
            /* Explicit teardown path. */
            it_slot_delete(IT_SCRATCH_2);
            it_slot_delete(IT_SCRATCH_1);
        }

        /* Revoking an EMPTY slot fails cleanly. */
        if (ok && (fz_rand() & 1u)) {
            it_slot_delete(IT_SCRATCH_3);
            if (it_cdt_revoke((long)IT_SCRATCH_3) >= 0) { ok = 0; why = "empty revoke ok"; }
        }

        it_slot_delete(IT_SCRATCH_2);
        it_slot_delete(IT_SCRATCH_1);
        it_slot_delete(IT_SCRATCH_0);
        it_close(&root);
        /* Periodically drain and reset so the bump region never runs dry. */
        if ((i & 3u) == 3u) {
            it_quiesce_reaper();
            if (ok && !it_ut_reset()) { ok = 0; why = "mid reset busy"; }
        }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext(e1))) { ok = 0; why = "ext final"; }
    for (uint32_t k = 0; ok && k < 5u; k++)
        if (s3a[k] != s3b[k]) { ok = 0; why = "object leak"; }
    if (ok && e1[IT_SI_LIVE] != e0[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }

    if (ok) it_pass("T131");
    else {
        fz_note("T131", T131_SEED, i);
        it_fail("T131", why);
    }
}

/* ── Phase 19: VM / VSpace / frame mapping hardening (T132–T139) ──────────────
 *
 * These tests close the gap Phase 18 left open: ring 3 now drives SYS_FRAME_MAP /
 * SYS_FRAME_UNMAP directly against its OWN address space, using a self-VSpace
 * cap obtained from SYS_VSPACE_SELF (self-authority only) and minted into
 * IRIS_CPTR_TEST_VSPACE.  Frames come from the Phase 18 boot untyped
 * (IRIS_CPTR_TEST_UNTYPED).  The Phase 19 additive instrumentation
 * (it_sched_ext4) exposes live KVSpace count, live KFrameMapping count, and the
 * cumulative map/unmap/TLB-invalidate counters — the observables behind V10–V18.
 *
 * live_mapping_count is the anchor invariant: it returns to baseline after every
 * unmap and after every VSpace teardown, so a leaked mapping (stale PTE / stale
 * node) or a double free is immediately visible.  A frame whose cap is closed
 * while still mapped would trip the kframe_obj_destroy `mapped_count == 0`
 * assert (a kernel panic), so a clean close is itself proof the frame was
 * unmapped first (V17). */

/* Helper: retype a fresh writable KFrame from the test untyped. */
handle_id_t it_retype_frame(void) {
    long r = it_frame_create_slot(IT_UT, 4096);
    return (r >= 0) ? (handle_id_t)r : HANDLE_INVALID;
}
void test_t132(void) {
    int ok = 1;
    const char *why = "self-vspace authority";
    it_quiesce_reaper();

    if (!it_setup_self_vspace()) { it_fail("T132", "vspace self mint"); return; }

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T132", "retype frame"); return; }

    /* Valid: map + unmap through the self-VSpace cap. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "valid map";
    }
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T133_VA) != 0) {
        ok = 0; why = "valid unmap";
    }

    /* Wrong-type VSpace slot: the untyped cap is not a VSpace. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_UT, (long)T133_VA, (long)IT_MAP_W)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong-type not rejected"; }

    /* Missing rights: a read-only self-VSpace cap cannot install a PTE. */
    if (ok) {
        /* The read-only copy is a slot-to-slot derive of the self-VSpace slot,
         * so it is an MDB child of the cap it narrows. */
        it_slot_delete(IT_VS_RO);
        if (it_invoke2(IT_VS, INV_CSPACE_MINT, (long)((uint64_t)IT_VS_RO << 32), (long)RIGHT_READ) != 0) {
            ok = 0; why = "ro mint";
        }
        if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS_RO, (long)T133_VA, (long)IT_MAP_W)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro not denied"; }
        /* No fallback: the denied map installed nothing (a following valid map
         * at the same VA still succeeds, proving the VA was left free). */
        if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) {
            ok = 0; why = "post-deny map";
        }
        if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T133_VA) != 0) { ok = 0; why = "post-deny unmap"; }
    }

    it_close(&fr);
    it_quiesce_reaper();
    (void)it_ut_reset();
    if (ok) it_pass("T132"); else it_fail("T132", why);
}

/* ── T133: direct frame map/unmap from ring 3 ───────────────────────────────
 * Map a retyped frame writable into the caller's own VSpace, write and read
 * back a pattern (proving the PTE is live in the active address space), unmap,
 * and confirm the mapping/TLB counters and frame lifetime all return to
 * baseline.  Reading after unmap would fault, so the "unmap removed access"
 * property is asserted at the accounting level (live_mapping_count baseline +
 * a fresh remap of the same VA succeeding) rather than by dereference.
 * Invariants: V2, V10, V12, V13, V21. */
void test_t133(void) {
    uint32_t s3b[6], s3a[6];
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T133", "vspace self mint"); return; }
    if (!it_sched_ext3(s3b) || !it_sched_ext4(v0)) { it_fail("T133", "sched ext"); return; }
    int ok = 1;
    const char *why = "frame map/unmap";

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T133", "retype frame"); return; }

    if (it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "map";
    }
    /* mapping is live: exactly one more mapping and one map-success. */
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 1u) { ok = 0; why = "maplive"; }
    if (ok && v1[IT_S4_MAPOK]   != v0[IT_S4_MAPOK]   + 1u) { ok = 0; why = "mapok"; }

    /* Write/read a pattern through the live mapping. */
    if (ok) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T133_VA;
        *p = 0x19C0FFEEu;
        __asm__ volatile ("" ::: "memory");
        if (*p != 0x19C0FFEEu) { ok = 0; why = "readback"; }
    }

    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T133_VA) != 0) { ok = 0; why = "unmap"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid2"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "maplive not restored"; }
    if (ok && v1[IT_S4_UNMAPOK] != v0[IT_S4_UNMAPOK] + 1u) { ok = 0; why = "unmapok"; }
    if (ok && v1[IT_S4_TLB]     <= v0[IT_S4_TLB])          { ok = 0; why = "no tlb invalidate"; }

    /* VA is free again: a fresh map at the same VA succeeds, then clean up. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T133_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "remap"; }
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T133_VA) != 0) { ok = 0; why = "reunmap"; }

    /* Close the frame — a clean close proves mapped_count == 0 (no assert). */
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame mapped?)"; }
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext4(v1))) { ok = 0; why = "ext final"; }
    if (ok && s3a[IT_S3_FRAME]  != s3b[IT_S3_FRAME])  { ok = 0; why = "frame leak"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }

    if (ok) it_pass("T133"); else it_fail("T133", why);
}

/* ── T134: map failure atomicity ────────────────────────────────────────────
 * Drive SYS_FRAME_MAP into each failure mode and prove no PTE, mapping node,
 * frame ref, or counter moves, and that a valid map right after each failure
 * still works.
 * Invariants: V5, V6, V7, V9, V11. */
void test_t134(void) {
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T134", "vspace self mint"); return; }
    if (!it_sched_ext4(v0)) { it_fail("T134", "sched ext4"); return; }
    int ok = 1;
    const char *why = "map atomicity";

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T134", "retype frame"); return; }

    /* Wrong-type frame fixture: an endpoint is not a frame. */
    long er = it_retype_slot_alloc(IT_UT, IT_KOBJ_ENDPOINT, 0);
    handle_id_t ep = (er >= 0) ? (handle_id_t)er : HANDLE_INVALID;
    /* Read-only frame fixture (drops WRITE): cannot back a writable map.
     * Phase S4 (Step 3): a native-CDT child, addressed by CPtr. */
    long rr = it_cdt_reduced(fr, IT_SCRATCH_0, IT_SCRATCH_1, RIGHT_READ);
    handle_id_t fr_ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
    if (ep == HANDLE_INVALID || rr < 0) { ok = 0; why = "fixtures"; }

    it_slot_delete(IT_SCRATCH_2);
    struct { long frame; long vs; uint64_t va; uint64_t flags; long expect; const char *tag; } cases[] = {
        { (long)fr,    IT_VS, T134_VA | 0x100ULL, IT_MAP_W, (long)IRIS_ERR_INVALID_ARG,   "unaligned va" },
        { (long)fr,    IT_VS, 0xFFFF800000000000ULL, IT_MAP_W, (long)IRIS_ERR_INVALID_ARG, "kernel va" },
        { (long)fr,    IT_VS, T134_VA, 3ULL,     (long)IRIS_ERR_INVALID_ARG,   "w^x" },
        { (long)ep,    IT_VS, T134_VA, IT_MAP_W, (long)IRIS_ERR_WRONG_TYPE,    "wrong frame type" },
        { (long)fr_ro, IT_VS, T134_VA, IT_MAP_W, (long)IRIS_ERR_ACCESS_DENIED, "insufficient rights" },
        { (long)fr,    IT_UT, T134_VA, IT_MAP_W, (long)IRIS_ERR_WRONG_TYPE,    "wrong vspace type" },
        /* Stage 5 Step 2: the empty-slot probe used to name slot 99, kept
         * permanently empty for it.  Slot 99 holds the framebuffer control
         * capability now, so the probe names a SCRATCH slot deleted on the
         * line above — guaranteed empty by construction rather than by a
         * comment asking everyone to leave it alone. */
        { (long)fr, (long)IT_SCRATCH_2, T134_VA, IT_MAP_W, (long)IRIS_ERR_NOT_FOUND, "empty vspace slot" },
    };
    for (uint32_t c = 0; ok && c < 7u; c++) {
        long got = it_invoke(cases[c].frame, INV_FRAME_MAP, cases[c].vs, (long)cases[c].va, (long)cases[c].flags);
        if (got != cases[c].expect) { ok = 0; why = cases[c].tag; break; }
        /* nothing installed by the failure */
        if (!it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; break; }
        if (v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "leaked mapping"; break; }
    }

    /* Occupied VA: a real map, then a duplicate at the same VA → BUSY. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T134_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "base map"; }
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T134_VA, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "occupied not busy";
    }
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T134_VA) != 0) { ok = 0; why = "cleanup unmap"; }

    /* A valid map right after all failures still works. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T134_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "valid-after-fail"; }
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T134_VA) != 0) { ok = 0; why = "final unmap"; }

    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_0);
    it_close(&ep);
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 final"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "final mapping leak"; }

    if (ok) it_pass("T134"); else it_fail("T134", why);
}

/* ── T135: duplicate / overlap / remap semantics ────────────────────────────
 * Documents and verifies the current mapping contract:
 *   - map A @ X ................. ok
 *   - map A @ X again .......... BUSY (VA occupied)
 *   - map B @ X ................ BUSY (VA occupied, different frame)
 *   - map A @ Y ................ ok (same frame, second VA; mapped_count == 2)
 *   - unmap in either order .... exact; unmap-absent → NOT_FOUND
 * A clean close of A after both unmaps proves mapped_count returned to 0.
 * Invariants: V8, V10, V12, V13, V14. */
void test_t135(void) {
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T135", "vspace self mint"); return; }
    if (!it_sched_ext4(v0)) { it_fail("T135", "sched ext4"); return; }
    int ok = 1;
    const char *why = "map semantics";

    handle_id_t a = it_retype_frame();
    handle_id_t b = it_retype_frame();
    if (a == HANDLE_INVALID || b == HANDLE_INVALID) { it_close(&a); it_close(&b); it_fail("T135", "retype"); return; }

    if (it_invoke((long)a, INV_FRAME_MAP, IT_VS, (long)T135_VA_X, (long)IT_MAP_W) != 0) { ok = 0; why = "map A@X"; }
    if (ok && it_invoke((long)a, INV_FRAME_MAP, IT_VS, (long)T135_VA_X, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "dup A@X"; }
    if (ok && it_invoke((long)b, INV_FRAME_MAP, IT_VS, (long)T135_VA_X, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "B over X"; }
    if (ok && it_invoke((long)a, INV_FRAME_MAP, IT_VS, (long)T135_VA_Y, (long)IT_MAP_W) != 0) { ok = 0; why = "map A@Y"; }

    /* Two live mappings of frame A. */
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 2u) { ok = 0; why = "maplive != +2"; }

    /* Unmap Y first, then X (reverse order); absent unmap → NOT_FOUND. */
    if (ok && it_invoke2((long)a, INV_FRAME_UNMAP, IT_VS, (long)T135_VA_Y) != 0) { ok = 0; why = "unmap A@Y"; }
    if (ok && it_invoke2((long)a, INV_FRAME_UNMAP, IT_VS, (long)T135_VA_Y) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "absent unmap"; }
    if (ok && it_invoke2((long)a, INV_FRAME_UNMAP, IT_VS, (long)T135_VA_X) != 0) { ok = 0; why = "unmap A@X"; }

    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid2"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "maplive not restored"; }

    /* Clean close of both frames (mapped_count == 0 or destroy asserts). */
    it_close(&a);
    it_close(&b);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy"; }

    if (ok) it_pass("T135"); else it_fail("T135", why);
}
void test_t136(void) {
    uint32_t v0[5], v1[5];
    uint32_t e0[14], e1[14];
    uint32_t tl0 = 0, tl1 = 0;
    it_quiesce_reaper();
    if (!it_sched_ext4(v0) || !it_sched_ext(e0) || !it_task_live(&tl0)) { it_fail("T136", "sched ext"); return; }
    int ok = 1;
    const char *why = "vspace death cleanup";
    uint32_t i = 0;

    for (i = 0; ok && i < T136_ROUNDS; i++) {
        long ep = it_ep_create();
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t p_h  = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &p_h) < 0) { ok = 0; why = "spawn"; }
        else {
            /* Child is alive with its own VSpace + bootstrap mappings. */
            uint32_t vm[5];
            if (!it_sched_ext4(vm)) { ok = 0; why = "ext4 alive"; }
            if (ok && vm[IT_S4_VSLIVE] <= v0[IT_S4_VSLIVE]) { ok = 0; why = "child vspace not counted"; }
            if (ok && vm[IT_S4_MAPLIVE] <= v0[IT_S4_MAPLIVE]) { ok = 0; why = "child mappings not counted"; }
            if (ok) {
                if (i & 1u) {
                    it_settle(1);
                    if (it_kill((long)p_h) != 0) { ok = 0; why = "kill"; }
                } else {
                    struct iris_msg m; iris_msg_zero(&m); m.label = 0x136;
                    (void)iris_msg_send((long)ep_h, &m);
                    (void)it_lp_wait_exit(p_h);
                }
            }
        }
        it_close(&p_h);
        it_close(&ep_h);
        it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && (!it_sched_ext4(v1) || !it_sched_ext(e1) || !it_task_live(&tl1))) { ok = 0; why = "ext final"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace leak"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }
    if (ok && e1[IT_SI_PROCLIVE] != e0[IT_SI_PROCLIVE]) { ok = 0; why = "proc-live drift"; }
    if (ok && tl1 != tl0) { ok = 0; why = "task-live drift"; }

    if (ok) it_pass("T136");
    else {
        it_serial_write("[IRIS][TEST] T136 round=");
        it_log_num(i);
        it_serial_write("\n");
        it_fail("T136", why);
    }
}

/* ── T137: mapped frame revoke interaction (closes the Phase 18 T128 gap) ─────
 * A retyped frame is MAPPED, then a derived handle is revoked while the frame
 * is live in the address space.  This demonstrates the real contract:
 *   - SYS_CAP_REVOKE is cap-scoped: it kills the derived handle but does NOT
 *     unmap the frame (the mapping holds an independent ref);
 *   - the frame object survives and stays usable through its live mapping;
 *   - frame destroy is impossible while mapped — only after unmap does closing
 *     the last cap destroy it (a clean close proves mapped_count == 0);
 *   - no stale PTE and no stale cap remain.
 * Invariants: V17, V18 (+ U10/U13 from Phase 18). */
void test_t137(void) {
    uint32_t s3b[6];
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T137", "vspace self mint"); return; }
    if (!it_sched_ext3(s3b) || !it_sched_ext4(v0)) { it_fail("T137", "sched ext"); return; }
    int ok = 1;
    const char *why = "mapped revoke";

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T137", "retype frame"); return; }

    if (it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T137_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }

    /* Phase S4 (Step 3): derived cap, revoked while the frame is mapped. */
    long frc   = ok ? it_cdt_root(fr, IT_SCRATCH_0) : -1;
    if (ok && frc < 0) { ok = 0; why = "root slot"; }
    long child = (frc >= 0) ? it_cdt_derive(frc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
    if (ok && child < 0) { ok = 0; why = "derive"; }
    if (ok && it_cdt_revoke(frc) < 0) { ok = 0; why = "revoke"; }
    if (ok && it_cdt_alive(child)) { ok = 0; why = "child alive"; }
    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_0);

    /* Revoke did NOT unmap: the mapping is still live and usable. */
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 1u) { ok = 0; why = "revoke changed mapping"; }
    if (ok) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T137_VA;
        *p = 0x137ABCDEu;
        __asm__ volatile ("" ::: "memory");
        if (*p != 0x137ABCDEu) { ok = 0; why = "frame unusable after revoke"; }
    }
    /* Frame object still alive (root cap held it). */
    if (ok && it_invoke0((long)fr, INV_CAP_IDENTIFY) != (long)IT_KOBJ_FRAME) { ok = 0; why = "frame died"; }

    /* Now unmap, then close — clean close proves mapped_count == 0 (no assert). */
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T137_VA) != 0) { ok = 0; why = "unmap"; }
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame still mapped?)"; }
    uint32_t s3a[6];
    if (ok && (!it_sched_ext3(s3a) || !it_sched_ext4(v1))) { ok = 0; why = "ext final"; }
    if (ok && s3a[IT_S3_FRAME]  != s3b[IT_S3_FRAME])  { ok = 0; why = "frame leak/double-free"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }

    if (ok) it_pass("T137"); else it_fail("T137", why);
}

/* ── T138: VSpace rights and user/kernel isolation ──────────────────────────
 * PTE authority reflects cap rights, and userland cannot map kernel space:
 *   - a read-only frame cap maps non-writable (flags=0) but is DENIED a
 *     writable map (flags=1) — rights are not amplified by the map;
 *   - a W^X request (writable+exec) is INVALID_ARG;
 *   - a kernel-range VA is INVALID_ARG (isolation).
 * Documented gap: ring 3 has no safe way to observe raw PTE flags or to trigger
 * a write-protection #PF without a fault-handling endpoint (Phase-future), so
 * NX/write-enforcement at the hardware level is asserted at the authority layer
 * (rights checks), not by faulting.
 * Invariants: V4, V6, V19, V20. */
void test_t138(void) {
    int ok = 1;
    const char *why = "rights/isolation";
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T138", "vspace self mint"); return; }

    handle_id_t fr = it_retype_frame();
    if (fr == HANDLE_INVALID) { it_fail("T138", "retype frame"); return; }
    long rr = it_cdt_reduced(fr, IT_SCRATCH_0, IT_SCRATCH_1, RIGHT_READ);
    handle_id_t fr_ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
    if (rr < 0) { ok = 0; why = "ro derive"; }

    /* Read-only cap: non-writable map ok, writable map denied. */
    if (ok && it_invoke((long)fr_ro, INV_FRAME_MAP, IT_VS, (long)T138_VA, 0L) != 0) { ok = 0; why = "ro map"; }
    if (ok && it_invoke2((long)fr_ro, INV_FRAME_UNMAP, IT_VS, (long)T138_VA) != 0) { ok = 0; why = "ro unmap"; }
    if (ok && it_invoke((long)fr_ro, INV_FRAME_MAP, IT_VS, (long)T138_VA, (long)IT_MAP_W)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable not denied"; }

    /* W^X rejected. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T138_VA, 3L) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "w^x not rejected";
    }
    /* Kernel-range VA rejected (user cannot map kernel space). */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, 0xFFFF800000001000L, (long)IT_MAP_W)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel va not rejected"; }

    it_slot_delete(IT_SCRATCH_1);
    it_slot_delete(IT_SCRATCH_0);
    it_close(&fr);
    it_quiesce_reaper();
    (void)it_ut_reset();
    if (ok) it_pass("T138"); else it_fail("T138", why);
}
void test_t139(void) {
    uint32_t v0[5], v1[5];
    uint32_t s3b[6], s3a[6];
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T139", "vspace self mint"); return; }
    if (!it_sched_ext4(v0) || !it_sched_ext3(s3b) || !it_sched_ext(e0)) { it_fail("T139", "sched ext"); return; }
    g_fz_seed = T139_SEED;
    int ok = 1;
    const char *why = "vspace stress";
    uint32_t i = 0;

    for (i = 0; ok && i < T139_ROUNDS; i++) {
        handle_id_t fr = it_retype_frame();
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype"; break; }
        uint64_t va = T139_VA_BASE + (uint64_t)(fz_rand() % 8u) * 0x1000ULL;

        /* Forced failures, deterministically interleaved. */
        if ((fz_rand() & 1u) &&
            it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)(va | 0x40ULL), (long)IT_MAP_W) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "unaligned"; it_close(&fr); break;
        }

        if (it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; it_close(&fr); break; }
        /* Occupied VA is BUSY. */
        if ((fz_rand() & 1u) &&
            it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) {
            ok = 0; why = "occupied"; it_close(&fr); break;
        }

        /* Sometimes derive + revoke while mapped (revoke must not unmap).
         * Phase S4 (Step 3): native CDT over scratch slots. */
        if (fz_rand() & 1u) {
            long frc = it_cdt_root(fr, IT_SCRATCH_0);
            long c   = (frc >= 0) ? it_cdt_derive(frc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
            if (c >= 0 && it_cdt_revoke(frc) < 0) { ok = 0; why = "revoke"; it_close(&fr); break; }
            if (ok && c >= 0 && it_cdt_alive(c)) { ok = 0; why = "child alive"; it_close(&fr); break; }
            it_slot_delete(IT_SCRATCH_1);
            it_slot_delete(IT_SCRATCH_0);
        }

        if (it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)va) != 0) { ok = 0; why = "unmap"; it_close(&fr); break; }
        it_close(&fr);

        if ((i & 7u) == 7u) {
            it_quiesce_reaper();
            if (!it_ut_reset()) { ok = 0; why = "mid reset busy"; break; }
        }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    if (ok && (!it_sched_ext4(v1) || !it_sched_ext3(s3a) || !it_sched_ext(e1))) { ok = 0; why = "ext final"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping leak"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace leak"; }
    if (ok && s3a[IT_S3_FRAME]  != s3b[IT_S3_FRAME])  { ok = 0; why = "frame leak"; }
    if (ok && e1[IT_SI_LIVE]    != e0[IT_SI_LIVE])    { ok = 0; why = "handle leak"; }

    if (ok) it_pass("T139");
    else {
        fz_note("T139", T139_SEED, i);
        it_fail("T139", why);
    }
}
static uint8_t g_it_fault_rec[IT_FAULT_LEAVES][FAULT_MSG_LEN];
uint8_t g_it_fault_have[IT_FAULT_LEAVES];
/* ...and the message LABEL it arrived under.  A server may share one endpoint
 * between faults and ordinary requests, so what tells them apart has to be on
 * the message: FAULT_MSG_NOTIFY is the kernel's, and nothing else sets it on a
 * message the kernel composed. */
uint64_t g_it_fault_label[IT_FAULT_LEAVES];
/* ...and the BADGE it was delivered through, which is how a handler serving
 * many clients on one endpoint knows whose fault it is (A-22). */
uint64_t g_it_fault_badge[IT_FAULT_LEAVES];

long it_fault_info(uint32_t leaf, struct it_fault *f) {
    if (leaf >= IT_FAULT_LEAVES || !g_it_fault_have[leaf]) return (long)IRIS_ERR_WOULD_BLOCK;
    const uint8_t *b = g_it_fault_rec[leaf];
    f->vector  = (uint32_t)b[FAULT_OFF_VECTOR]  | ((uint32_t)b[FAULT_OFF_VECTOR + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_VECTOR + 2] << 16) | ((uint32_t)b[FAULT_OFF_VECTOR + 3] << 24);
    f->task_id = (uint32_t)b[FAULT_OFF_TASK_ID] | ((uint32_t)b[FAULT_OFF_TASK_ID + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_TASK_ID + 2] << 16) | ((uint32_t)b[FAULT_OFF_TASK_ID + 3] << 24);
    f->error   = (uint32_t)b[FAULT_OFF_ERROR]   | ((uint32_t)b[FAULT_OFF_ERROR + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_ERROR + 2] << 16) | ((uint32_t)b[FAULT_OFF_ERROR + 3] << 24);
    f->seq     = (uint32_t)b[FAULT_OFF_SEQ]     | ((uint32_t)b[FAULT_OFF_SEQ + 1] << 8) |
                 ((uint32_t)b[FAULT_OFF_SEQ + 2] << 16) | ((uint32_t)b[FAULT_OFF_SEQ + 3] << 24);
    f->rip = 0; f->cr2 = 0;
    for (uint32_t i = 0; i < 8u; i++) f->rip |= (uint64_t)b[FAULT_OFF_RIP + i] << (i * 8);
    for (uint32_t i = 0; i < 8u; i++) f->cr2 |= (uint64_t)b[FAULT_OFF_CR2 + i] << (i * 8);
    return 0;
}

/* Send a fault-trigger command with a target VA (blocking send — returns once
 * the child has picked the message up, i.e. is about to fault). */
long it_lp_cmd_va(handle_id_t ep_h, uint32_t label, uint64_t va) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = label;
    m.words[0]   = va;
    m.word_count = 1u;
    return iris_msg_send((long)ep_h, &m);
}

/* Spawn a lifecycle_probe child wired for fault supervision: command endpoint,
 * fault-handler notification (signal bit 0) registered via proc cap, and an
 * exit watch (bit 0 of w_h).  All-or-nothing; on failure everything is closed
 * and *why is set.  Returns 1 on success. */
/* `mbox` is the reply-object leaf this child's faults are answered through.
 * Two children blocked in a fault at once is a case T147 deliberately
 * produces, so each needs its own — one shared leaf would bind the second
 * fault's reply where the first one's was, and "resume the primary" would
 * silently resume the other. */
static int it_fault_spawn_mbox(uint32_t mbox,
                          handle_id_t *ep_h, handle_id_t *proc_h,
                          handle_id_t *n_h, handle_id_t *w_h, const char **why) {
    (void)mbox;   /* the leaf is named by whoever RECEIVES, not by the arming */
    *ep_h = *proc_h = *n_h = *w_h = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ep create"; return 0; }
    *ep_h = (handle_id_t)ep;
    if (lp_spawn_child(*ep_h, proc_h) < 0 || *proc_h == HANDLE_INVALID) {
        it_close(ep_h); *why = "spawn"; return 0;
    }
    /* Ledger A-22: `n_h` is the child's FAULT ENDPOINT, not a notification.
     * The suite receives on it; the record arrives as the message and the
     * reply capability arrives with it. */
    long fe = it_ep_create();
    long w  = it_notify_create();
    *n_h = (fe >= 0) ? (handle_id_t)fe : HANDLE_INVALID;
    *w_h = (w  >= 0) ? (handle_id_t)w  : HANDLE_INVALID;
    if (fe < 0 || w < 0 ||
        it_invoke(it_child_tcb((long)*proc_h), INV_TCB_SET_FAULT_HANDLER, fe, 0, 0) != 0 ||
        it_invoke2(it_child_tcb((long)*proc_h), INV_TCB_WATCH, w, 1) != 0) {
        (void)it_kill((long)*proc_h);
        it_close(n_h); it_close(w_h); it_close(proc_h); it_close(ep_h);
        *why = "wire handler/watch"; return 0;
    }
    return 1;
}

static int it_fault_spawn(handle_id_t *ep_h, handle_id_t *proc_h,
                          handle_id_t *n_h, handle_id_t *w_h, const char **why) {
    return it_fault_spawn_mbox(0u, ep_h, proc_h, n_h, w_h, why);
}

/*
 * Bounded wait for a fault on `fault_ep`, answered through reply leaf `mbox`.
 * 1 on success, 0 if none arrived.
 *
 * Non-blocking receive in a retry loop rather than a blocking one: several
 * tests here assert that NO fault arrives, and a blocking receive would hang
 * the suite instead of failing it.  The old notification wait had a 2-second
 * timeout for exactly this reason; a receive has no timeout, so the bound is
 * the retry count with a yield between tries.
 */
static int it_fault_try(long fault_ep, uint32_t mbox) {
    struct iris_msg m;
    iris_msg_zero(&m);
    if ((m.reply = (long)(IT_FAULT_CPTR(mbox)), iris_msg_nb_recv((long)fault_ep, &m)) != 0)
        return 0;
    for (uint32_t b = 0; b < FAULT_MSG_LEN; b++)
        g_it_fault_rec[mbox][b] = ((const uint8_t *)m.words)[b];
    g_it_fault_have[mbox]  = 1u;
    g_it_fault_label[mbox] = m.label;
    g_it_fault_badge[mbox] = m.sender_badge;
    return 1;
}

int it_fault_wait_ep(long fault_ep, uint32_t mbox) {
    if (mbox >= IT_FAULT_LEAVES) return 0;
    /* ONE fresh reply object for the whole wait.  Retyping one per poll would
     * churn the object pool T324 measures — and a reply object that nothing
     * bound to is reusable, so there is nothing to refresh between tries. */
    if (!it_fault_reply_fresh(mbox)) return 0;

    /* The fast path, unchanged: most faults are already waiting, and the ones
     * that are not arrive within a handful of dispatches. */
    for (uint32_t tries = 0; tries < 3000u; tries++) {
        if (it_fault_try(fault_ep, mbox)) return 1;
        (void)it_sys1(SYS_YIELD, 0);
    }

    /*
     * And a tail bounded in TIME (SMP roadmap §9.3 step 4).
     *
     * Three thousand yields was the whole bound, and on one processor it was a
     * real one — every yield was a dispatch, so three thousand of them was
     * thousands of chances for the faulting thread to run.  On four it is
     * three thousand fast syscalls on THIS core that can all complete before
     * another core has taken a single timer interrupt.
     *
     * T308 is the case that needs it: the fault it waits for is a TIMEOUT, so
     * it cannot arrive until the server has burned a budget measured in ticks.
     * A bound expressed in this thread's syscalls cannot express "three ticks
     * from now" on a machine where this thread is not the one being charged.
     *
     * Two seconds, and it only costs that when the fault genuinely never
     * comes — which is a test failing, where two seconds is not the problem.
     */
    for (uint32_t t = 0; t < 200u; t++) {
        if (it_fault_try(fault_ep, mbox)) return 1;
        it_settle(1);
    }
    return 0;
}

/* Resume the thread whose fault leaf `mbox` holds. */
long it_fault_resume(uint32_t mbox) {
    struct iris_msg m;
    iris_msg_zero(&m);
    if (mbox < IT_FAULT_LEAVES) g_it_fault_have[mbox] = 0u;
    long r = iris_msg_reply(IT_FAULT_CPTR(mbox), &m);
    /* A reply object is ONE-SHOT: once spent it can never answer anything
     * again, so the capability is dropped here rather than left in a slot
     * where a later test would count it as live authority. */
    if (r == 0) (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)IT_FAULT_LEAF(mbox));
    return r;
}

/* ...and refuse it: destroying the reply object leaves the fault unanswerable,
 * which the kernel resolves by destroying the thread.  This is what "kill the
 * faulting thread" is now — a supervisor with no reply to give. */
long it_fault_kill(uint32_t mbox) {
    if (mbox < IT_FAULT_LEAVES) g_it_fault_have[mbox] = 0u;
    return it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)IT_FAULT_LEAF(mbox));
}

static void it_fault_close4(handle_id_t *a, handle_id_t *b,
                            handle_id_t *c, handle_id_t *d) {
    it_close(a); it_close(b); it_close(c); it_close(d);
}

/* ── T140: register fault endpoint authority (A-22) ─────────────────────────
 * Registration is capability-mediated with no fallback:
 *   - RIGHT_WRITE on the THREAD and RIGHT_WRITE on the ENDPOINT are both
 *     required — reduced-rights copies get ACCESS_DENIED;
 *   - the endpoint slot takes an endpoint and nothing else (WRONG_TYPE), and
 *     the thread slot takes a thread and nothing else;
 *   - the RETIRED arguments are refused, not ignored: the signal mask and the
 *     mailbox destination the old three-mechanism form carried are
 *     INVALID_ARG, so code written for that shape fails loudly instead of
 *     silently arming a mailbox nothing will ever fill;
 *   - an empty slot fails;
 *   - a failed registration leaves NO partial handler installed: a subsequent
 *     fault takes the no-handler path (task killed, nohandler counter up);
 *   - re-registration replaces the handler (last registration wins — only the
 *     new endpoint receives);
 *   - sending an ordinary message to a fault endpoint does not FORGE a fault:
 *     it arrives with the sender's own badge and its own label, and a reply to
 *     it resumes nobody;
 *   - registering on a dead thread fails NOT_FOUND (would leak the pin).
 * Invariants: F3, F4, F5, F9, F17, F18. */
void test_t140(void) {
    uint32_t e0[14], e1[14], s3b[6], s3a[6], f0[6], f1[6];
    int ok = 1;
    const char *why = "register authority";
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext3(s3b) || !it_sched_ext5(f0)) {
        it_fail("T140", "sched ext"); return;
    }

    /* Child 1: probe every failure path, then fault with NO handler. */
    long ep = it_ep_create_slot();
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    handle_id_t proc_h = HANDLE_INVALID;
    if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
        it_close(&ep_h); it_fail("T140", "spawn"); return;
    }
    long n1 = it_notify_create_slot();
    long w  = it_notify_create_slot();
    handle_id_t n1_h = (n1 >= 0) ? (handle_id_t)n1 : HANDLE_INVALID;
    handle_id_t w_h  = (w  >= 0) ? (handle_id_t)w  : HANDLE_INVALID;
    if (n1 < 0 || w < 0) { ok = 0; why = "notify create"; }

    if (ok && it_invoke2(it_child_tcb((long)proc_h), INV_TCB_WATCH, w, 1) != 0) { ok = 0; why = "watch"; }

    /* Wrong types, both slots.  A-22: the handler is an ENDPOINT, so a
     * notification in that slot is WRONG_TYPE — it used to be the only thing
     * accepted there. */
    if (ok && it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, n1, 0, 0)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "notif wrong-type"; }
    /* Stage 7 Step 12: the first argument names the THREAD, so the wrong-type
     * probe on that half passes a notification where a TCB belongs.  The TCB
     * family answers WRONG_TYPE there too — one is the object the syscall is
     * invoked ON, the other an object it is handed, and both are checked. */
    if (ok && it_invoke(n1, INV_TCB_SET_FAULT_HANDLER, (long)ep_h, 0, 0)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "tcb wrong-type"; }
    /* Reduced rights, both slots — ACCESS_DENIED, no fallback.  Arming where
     * an execution's faults go is a WRITE to that execution, and arranging for
     * messages to be sent to an endpoint is a WRITE to that endpoint. */
    long pr_ro = it_cs_reduce(it_child_tcb((long)proc_h), RIGHT_READ);
    long n_ro  = it_cs_reduce((long)ep_h, RIGHT_READ);
    handle_id_t pr_ro_h = (pr_ro >= 0) ? (handle_id_t)pr_ro : HANDLE_INVALID;
    handle_id_t n_ro_h  = (n_ro  >= 0) ? (handle_id_t)n_ro  : HANDLE_INVALID;
    if (ok && (pr_ro < 0 || n_ro < 0)) { ok = 0; why = "ro dups"; }
    if (ok && it_invoke(pr_ro, INV_TCB_SET_FAULT_HANDLER, (long)ep_h, 0, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "tcb no-write not denied"; }
    if (ok && it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, n_ro, 0, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ep no-write not denied"; }
    /* The retired arguments are REFUSED.  Ignoring them would let the old
     * three-mechanism call keep compiling and keep "succeeding" while the
     * mailbox it names is never written. */
    if (ok && it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, (long)ep_h, 1, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "signal mask accepted"; }
    if (ok && it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, (long)ep_h, 0, (long)(((uint64_t)IT_FAULT_LEAF(0) << 32) |
                             (uint64_t)IT_OBJ_CNODE_SLOT))
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "mailbox accepted"; }
    /* Empty slot. */
    if (ok && it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, 9999L, 0, 0) >= 0) {
        ok = 0; why = "empty slot accepted";
    }

    /* Every attempt above failed → no handler may be installed: the fault must
     * take the kill path (F5 — no partial registration, no fallback). */
    if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_wait_timeout( w, (long)(uintptr_t)&bits,
                    2000000000LL) != 0 || !(bits & 1ull)) { ok = 0; why = "nohandler kill"; }
    }
    if (ok && it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE) != 0) { ok = 0; why = "kill exit code"; }
    if (ok && it_fault_info(0u, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "dead proc fault info";
    }
    /* Dead thread: registration must fail NOT_FOUND, not silently pin. */
    if (ok && it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, (long)ep_h, 0, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "dead reg not NOT_FOUND"; }

    it_close(&pr_ro_h); it_close(&n_ro_h);
    it_fault_close4(&ep_h, &proc_h, &n1_h, &w_h);

    /* Child 2: valid registration, replacement contract, forgery check. */
    handle_id_t ep2, pr2, na, wb;
    if (ok && !it_fault_spawn_mbox(1u, &ep2, &pr2, &na, &wb, &why)) { ok = 0; }
    if (ok) {
        long n2 = it_ep_create_slot();
        handle_id_t n2_h = (n2 >= 0) ? (handle_id_t)n2 : HANDLE_INVALID;
        if (n2 < 0) { ok = 0; why = "n2 create"; }
        /* Replace na with n2 — last registration wins. */
        if (ok && it_invoke(it_child_tcb((long)pr2), INV_TCB_SET_FAULT_HANDLER, n2, 0, 0) != 0) {
            ok = 0; why = "re-register";
        }
        /*
         * Nothing has been received, so there is nothing to answer.
         *
         * The old spoof check signalled the handler's NOTIFICATION by hand and
         * asserted no fault record appeared — possible because delivery and
         * information were two mechanisms, so one could be faked without the
         * other.  There is nothing to fake now: the message IS the delivery.
         * What replaces it is the property a handler actually relies on, and
         * it is checked on the real fault below — a delivered fault carries
         * the label FAULT_MSG_NOTIFY, so a server sharing an endpoint between
         * faults and requests can tell them apart.
         */
        if (ok && it_fault_info(1u, &(struct it_fault){0})
                  != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "fault before any"; }
        if (ok && it_lp_cmd_va(ep2, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd2"; }
        if (ok && !it_fault_wait_ep((long)n2_h, 1u)) { ok = 0; why = "replaced handler no delivery"; }
        if (ok && g_it_fault_label[1] != (uint64_t)FAULT_MSG_NOTIFY) {
            ok = 0; why = "fault message unlabelled";
        }
        /* The replaced-away endpoint must NOT have received it. */
        if (ok) {
            struct iris_msg stale;
            iris_msg_zero(&stale);
            if ((stale.reply = 0L, iris_msg_nb_recv((long)na, &stale))
                != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "old handler fired"; }
        }
        struct it_fault f;
        if (ok && it_fault_info(1u, &f) != 0) { ok = 0; why = "fault info 2"; }
        if (ok && f.vector != 14u) { ok = 0; why = "vector 2"; }
        if (ok && it_fault_kill(1) != 0) {
            ok = 0; why = "resume kill";
        }
        if (ok && it_lp_wait_exit(pr2) != 0) { ok = 0; why = "child2 exit"; }
        it_close(&n2_h);
    }
    it_fault_close4(&ep2, &pr2, &na, &wb);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext3(s3a) || !it_sched_ext5(f1))) {
        ok = 0; why = "ext final";
    }
    if (ok && f1[IT_S5_NOHAND]  != f0[IT_S5_NOHAND] + 1u)  { ok = 0; why = "nohandler count"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && e1[IT_SI_LIVE]    != e0[IT_SI_LIVE])         { ok = 0; why = "handle leak"; }
    if (ok && s3a[IT_S3_NOTIF]  != s3b[IT_S3_NOTIF])       { ok = 0; why = "notif leak"; }
    if (ok && s3a[IT_S3_EP]     != s3b[IT_S3_EP])          { ok = 0; why = "ep leak"; }
    if (ok) it_pass("T140"); else it_fail("T140", why);
}

/* ── T141: page fault delivery — invalid user VA ────────────────────────────
 * The foundational delivery contract: the child touches an unmapped user VA;
 * the fault arrives exactly once on the registered notification with honest
 * info (vector 14, cr2 == the VA, user-range rip, error P=0/U=1); the child is
 * suspended (still alive, not running past the faulting load) while pending;
 * the record persists until resolved; kill-resolution reaps the child and
 * clears the record.  Invariants: F1, F6, F7, F8, F11, F15, F19. */
void test_t141(void) {
    uint32_t t0 = 0, t1 = 0, e0[14], e1[14], f0[6], f1[6];
    int ok = 1;
    const char *why = "invalid VA delivery";
    it_quiesce_reaper();
    if (!it_task_live(&t0) || !it_sched_ext(e0) || !it_sched_ext5(f0)) {
        it_fail("T141", "sched ext"); return;
    }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T141", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery"; }

    struct it_fault f;
    if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.vector != 14u)               { ok = 0; why = "vector"; }
    if (ok && f.cr2 != T14X_BAD_VA)          { ok = 0; why = "cr2"; }
    if (ok && (f.rip == 0 || f.rip >= 0x0000800000000000ULL)) { ok = 0; why = "rip range"; }
    if (ok && f.task_id == 0)                { ok = 0; why = "task id"; }
    if (ok && f.error != PF_ERR_U)           { ok = 0; why = "error bits"; }  /* not-present user read */

    /* Suspended while pending: alive (EXIT_CODE blocks), no second signal, and
     * the record is stable across reads. */
    if (ok && it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "child not suspended-alive"; }
    if (ok) {
        /* Exactly once: a blocked thread cannot fault again, so a second
         * receive on its endpoint finds nothing. */
        struct iris_msg again;
        iris_msg_zero(&again);
        if ((again.reply = 0L, iris_msg_nb_recv((long)n_h, &again))
            != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "double delivery"; }
    }
    struct it_fault f2;
    if (ok && (it_fault_info(0u, &f2) != 0 || f2.task_id != f.task_id ||
               f2.cr2 != f.cr2 || f2.rip != f.rip)) { ok = 0; why = "record unstable"; }

    /* Kill-resolution: reaps the child, clears the record. */
    if (ok && it_fault_kill(0) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit code"; }
    if (ok && it_fault_info(0u, &f2) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived kill";
    }

    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);
    it_quiesce_reaper();
    if (ok && (!it_task_live(&t1) || !it_sched_ext(e1) || !it_sched_ext5(f1))) {
        ok = 0; why = "ext final";
    }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivered != 1"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "kill count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 1u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0)                    { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]  != e0[IT_SI_LIVE])  { ok = 0; why = "handle leak"; }
    /* Ledger A-22: a fault issues EXACTLY ONE reply capability — the
     * authority to resume the thread, and nothing else.  This used to assert
     * the counter did not move at all, because a fault issued no capability
     * and answering one was a syscall anybody holding the TCB could make. */
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY] + 1u) { ok = 0; why = "kreply drift"; }
    if (ok) it_pass("T141"); else it_fail("T141", why);
}

/* ── T142: write-protection fault on a read-only mapping ────────────────────
 * Closes the Phase 19 T138 gap: write-enforcement is now asserted at the
 * HARDWARE level, not only at the rights layer.  The child's own code pages
 * are mapped r-x by the loader (PF flags → map flags → PTE), so:
 *   - a READ of its own text completes (child exits normally, no fault);
 *   - a WRITE to its own text raises #PF with error P=1/W=1/U=1, cr2 = the
 *     write target, and the store must NOT retire: resuming without fixing
 *     the condition re-faults at the same rip/cr2 (no silent write, no
 *     corruption), after which the supervisor kills the child.
 * VSpace books return to baseline after reap.  Invariants: F6, F7, F16, F20,
 * F21 (write bit distinguishable).  Phase 19 V6 gap closed. */
void test_t142(void) {
    uint32_t v0[5], v1[5], f0[6], f1[6];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "ro write fault";
    it_quiesce_reaper();
    if (!it_sched_ext4(v0) || !it_sched_ext5(f0) || !it_task_live(&t0)) {
        it_fail("T142", "sched ext"); return;
    }

    /* Child A: reading own text is allowed — exits, no fault delivered. */
    handle_id_t ep_a, pr_a, n_a, w_a;
    if (!it_fault_spawn_mbox(1u, &ep_a, &pr_a, &n_a, &w_a, &why)) { it_fail("T142", why); return; }
    if (it_lp_cmd_va(ep_a, LP_CMD_FAULT_READ, 0) != 0) { ok = 0; why = "cmd read"; }
    if (ok) {
        long ec = it_lp_wait_exit(pr_a);
        if ((ec >> 8) != (LP_EXIT_MARKER >> 8)) { ok = 0; why = "text read blocked"; }
    }
    it_fault_close4(&ep_a, &pr_a, &n_a, &w_a);

    /* Child B: writing own text must fault — and must not retire. */
    handle_id_t ep_h, proc_h, n_h, w_h;
    if (ok && !it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T142", why); return; }
    if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_WRITE, 0) != 0) { ok = 0; why = "cmd write"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery"; }

    struct it_fault f;
    if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.vector != 14u) { ok = 0; why = "vector"; }
    if (ok && f.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U)) { ok = 0; why = "not a write-protect err"; }
    if (ok && (f.cr2 == 0 || f.cr2 >= 0x0000800000000000ULL)) { ok = 0; why = "cr2 range"; }

    /* Resume without fixing: the same store re-faults (no silent write). */
    if (ok && it_fault_resume(0) != 0) {
        ok = 0; why = "resume";
    }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no refault"; }
    struct it_fault g;
    if (ok && it_fault_info(0u, &g) != 0) { ok = 0; why = "refault info"; }
    if (ok && (g.rip != f.rip || g.cr2 != f.cr2 ||
               g.error != f.error)) { ok = 0; why = "refault mismatch"; }
    /* The child must NOT have exited (the store never retires). */
    if (ok && it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "write retired"; }

    if (ok && it_fault_kill(0) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext4(v1) || !it_sched_ext5(f1) || !it_task_live(&t1))) {
        ok = 0; why = "ext final";
    }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 1u)  { ok = 0; why = "resume count"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping drift"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace drift"; }
    if (ok && t1 != t0) { ok = 0; why = "task live drift"; }
    if (ok) it_pass("T142"); else it_fail("T142", why);
}

/* ── T143: NX instruction-fetch fault ───────────────────────────────────────
 * NX is real end to end: EFER.NXE is enabled at paging init and every
 * non-EXEC user mapping carries PTE.NX (kframe_map_page), including the
 * child's stack (rw- by the loader/creator).  The child copies `ret` opcodes
 * onto its stack and calls them: the fetch must fault with error
 * P=1/U=1/I=1 and cr2 == rip (the fetch address IS the faulting address) in
 * the user range.  No escalation: the supervisor observes and kills.
 * Invariants: F1, F7, F21 (execute bit distinguishable), F22. */
void test_t143(void) {
    uint32_t f0[6], f1[6];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "nx exec fault";
    it_quiesce_reaper();
    if (!it_sched_ext5(f0) || !it_task_live(&t0)) { it_fail("T143", "sched ext"); return; }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T143", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_EXEC, 0) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery"; }

    struct it_fault f;
    if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.vector != 14u) { ok = 0; why = "vector"; }
    if (ok && f.error != (PF_ERR_P | PF_ERR_U | PF_ERR_I)) { ok = 0; why = "not an ifetch err"; }
    if (ok && f.cr2 != f.rip) { ok = 0; why = "cr2 != rip"; }
    if (ok && (f.cr2 == 0 || f.cr2 >= 0x0000800000000000ULL)) { ok = 0; why = "cr2 range"; }

    if (ok && it_fault_kill(0) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext5(f1) || !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && t1 != t0) { ok = 0; why = "task live drift"; }
    if (ok) it_pass("T143"); else it_fail("T143", why);
}

/* ── T144: fault resume semantics (A-22) ────────────────────────────────────
 * Resolution is exact, authorized, and one-shot — and every one of those is
 * now a property of the REPLY CAPABILITY rather than of a syscall's argument
 * checking:
 *   - answering needs RIGHT_WRITE on the reply object; a read-only copy is
 *     ACCESS_DENIED (F10);
 *   - a capability that is not a reply answers nothing (F11);
 *   - a valid reply re-executes the faulting instruction — the same unmapped
 *     load faults again as a NEW fault, with a NEW reply capability (F16);
 *   - one-shot needs no generation number: the second reply on a spent object
 *     is a clean NOT_FOUND (F11, F12 — no stale state). */
void test_t144(void) {
    uint32_t f0[6], f1[6];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "resume semantics";
    it_quiesce_reaper();
    if (!it_sched_ext5(f0) || !it_task_live(&t0)) { it_fail("T144", "sched ext"); return; }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T144", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery"; }
    struct it_fault f;
    if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }

    /* Wrong authority: RIGHT_READ-only proc dup must be denied. */
    long pr_ro = it_cs_reduce((long)proc_h, RIGHT_READ);
    handle_id_t pr_ro_h = (pr_ro >= 0) ? (handle_id_t)pr_ro : HANDLE_INVALID;
    if (ok && pr_ro < 0) { ok = 0; why = "ro dup"; }
    /* Stage 7 Step 7: the authority is the TCB capability, so the denial test
     * is a rights-reduced TCB rather than a rights-reduced process cap —
     * RIGHT_WRITE on a thread is what decides whether it runs again. */
    {
        /* RIGHT_WRITE on the reply object is what decides whether the thread
         * runs again — a read-only copy of the very same authority answers
         * nothing.  This is the whole of "who may resume", where it used to be
         * RIGHT_WRITE on a TCB capability the kernel had minted into a
         * mailbox, which also authorised everything else a thread can be made
         * to do. */
        struct iris_msg rm;
        iris_msg_zero(&rm);
        long rp_ro = it_cs_reduce(IT_FAULT_CPTR(0), RIGHT_READ);
        if (ok && rp_ro < 0) { ok = 0; why = "ro reply dup"; }
        if (ok && iris_msg_reply(rp_ro, &rm)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-write not denied"; }
        if (rp_ro >= 0) { handle_id_t h = (handle_id_t)rp_ro; it_close(&h); }
    }
    /* Exactness: a capability that is not a reply answers nothing.  "Wrong
     * task id" has no analogue any more, and neither does "bad action" — there
     * is no number to get wrong and no action to choose, only a capability you
     * either hold or do not. */
    {
        struct iris_msg rm;
        iris_msg_zero(&rm);
        if (ok && iris_msg_reply((long)ep_h, &rm)
                  != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "non-reply not rejected"; }
        if (ok && iris_msg_reply(it_child_tcb((long)proc_h), &rm)
                  != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "tcb accepted as reply"; }
    }

    /* Valid resume: the load re-executes and faults again — a NEW fault. */
    if (ok && it_fault_resume(0) != 0) {
        ok = 0; why = "resume";
    }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no refault"; }
    struct it_fault g;
    if (ok && it_fault_info(0u, &g) != 0) { ok = 0; why = "refault info"; }
    if (ok && (g.cr2 != f.cr2 || g.task_id != f.task_id)) { ok = 0; why = "refault mismatch"; }

    /* Kill-resolution, then verify nothing stale remains. */
    if (ok && it_fault_kill(0) != 0) {
        ok = 0; why = "resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
    if (ok && it_fault_resume(0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume not NOT_FOUND"; }
    if (ok && it_fault_info(0u, &g) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "stale record";
    }

    it_close(&pr_ro_h);
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);
    it_quiesce_reaper();
    if (ok && (!it_sched_ext5(f1) || !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 1u)  { ok = 0; why = "resume count"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "kill count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0) { ok = 0; why = "task live drift"; }
    if (ok) it_pass("T144"); else it_fail("T144", why);
}

/* ── T145: handler drop during a pending fault ──────────────────────────────
 * The handler's notification HANDLE is not the resolution authority — the
 * process cap is.  Registration pins the notification object inside the
 * KProcess, so the supervisor closing its own handle mid-fault leaves the
 * pending fault fully resolvable:
 *   (a) close the handler notif while a fault is pending → EXCEPTION_RESUME
 *       (kill) through the proc cap still resolves and reaps;
 *   (b) same, but resolve via SYS_PROCESS_KILL → teardown clears the fault
 *       record, releases the pinned notification, reaps everything.
 * Documented contract: handler death never auto-kills the faulted process;
 * the faulted task stays suspended until a RIGHT_MANAGE holder resolves it.
 * No zombies, no waiter/KReply drift, notification objects at baseline.
 * Invariants: F13, F14, F15, F17, F18, F19. */
void test_t145(void) {
    uint32_t e0[14], e1[14], s3b[6], s3a[6], f0[6], f1[6];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "handler drop";
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext3(s3b) || !it_sched_ext5(f0) ||
        !it_task_live(&t0)) { it_fail("T145", "sched ext"); return; }

    /* (a) notif handle closed mid-fault → resume-kill still resolves. */
    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T145", why); return; }
    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd a"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery a"; }
    struct it_fault f;
    if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info a"; }
    it_close(&n_h);                       /* handler endpoint gone */
    if (ok && it_invoke0(it_child_tcb((long)proc_h), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "child died on handler close"; }
    if (ok && it_fault_kill(0) != 0) {
        ok = 0; why = "post-close resume kill";
    }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit a"; }
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    /* (b) notif handle closed mid-fault → PROCESS_KILL resolves via teardown. */
    if (ok && !it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T145", why); return; }
    if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd b"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery b"; }
    it_close(&n_h);
    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "process kill"; }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit b"; }
    /*
     * Ledger A-22: what teardown clears is the ANSWER, not the supervisor's
     * copy of the message.
     *
     * The record used to live in the kernel and be read back on demand, so
     * "did teardown clear it" was a question about kernel state.  It is a
     * message this task received and owns now — the kernel has no business
     * reaching into it — and the property that actually matters survives
     * intact: the reply capability no longer answers anybody, because the
     * thread it was bound to is gone.
     */
    if (ok && it_fault_resume(0) != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "record survived teardown";
    }
    (void)it_fault_kill(0);
    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext3(s3a) || !it_sched_ext5(f1) ||
               !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0)                   { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]   != e0[IT_SI_LIVE])   { ok = 0; why = "handle leak"; }
    /* Two faults, two reply capabilities — one each, and no more. */
    if (ok && e1[IT_SI_REPLY]  != e0[IT_SI_REPLY] + 2u) { ok = 0; why = "kreply drift"; }
    if (ok && s3a[IT_S3_NOTIF] != s3b[IT_S3_NOTIF]) { ok = 0; why = "notif obj leak"; }
    if (ok && s3a[IT_S3_EP]    != s3b[IT_S3_EP])    { ok = 0; why = "ep obj leak"; }
    if (ok) it_pass("T145"); else it_fail("T145", why);
}

/* ── T146: process kill while a fault is pending ────────────────────────────
 * The supervisor kills the whole process while its task sits in
 * TASK_BLOCKED_FAULT.  Teardown must clear the fault record, cancel nothing
 * that isn't there, and leave zero residue; the handler's LATE response after
 * the kill fails clean:
 *   - EXCEPTION_RESUME after the kill → NOT_FOUND (no matching blocked task);
 *   - FAULT_INFO after the kill → WOULD_BLOCK (record cleared by teardown);
 *   - task/handle/KReply/mapping books at baseline.
 * Invariants: F12, F15, F17, F18, F19, F20. */
void test_t146(void) {
    uint32_t e0[14], e1[14], v0[5], v1[5], f0[6], f1[6];
    uint32_t t0 = 0, t1 = 0;
    int ok = 1;
    const char *why = "kill while pending";
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext4(v0) || !it_sched_ext5(f0) ||
        !it_task_live(&t0)) { it_fail("T146", "sched ext"); return; }

    handle_id_t ep_h, proc_h, n_h, w_h;
    if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { it_fail("T146", why); return; }

    if (it_lp_cmd_va(ep_h, LP_CMD_FAULT_WRITE, T14X_BAD_VA) != 0) { ok = 0; why = "cmd"; }
    if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery"; }
    struct it_fault f;
    if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }
    if (ok && f.error != (PF_ERR_W | PF_ERR_U)) { ok = 0; why = "write err bits"; } /* not-present user write */

    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }

    /* Late handler response: clean failures, nothing stale.
     *
     * A-22: answering a thread that is gone is NOT_FOUND — the reply object is
     * still a perfectly good capability, it simply has no caller bound any
     * more.  DROPPING it late succeeds and does nothing, which is the correct
     * outcome and used to be an error: a handler giving up on a fault that has
     * already been resolved is not a failure, it is a handler tidying up. */
    if (ok && it_fault_resume(0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume not NOT_FOUND"; }
    if (ok && it_fault_kill(0) != 0) { ok = 0; why = "late drop refused"; }
    if (ok && it_fault_info(0u, &f) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived kill";
    }
    (void)0;

    it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);
    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext4(v1) || !it_sched_ext5(f1) ||
               !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    /* A-22: a thread destroyed with a fault outstanding IS a kill resolution
     * — the fault ended, and it ended by the thread ceasing to exist.  It used
     * to move only when a handler said action=1, and teardown clearing the
     * record was counted as a cleanup and nothing else, so "the fault was
     * never answered" and "the fault was answered with a kill" looked the same
     * from outside. */
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "resume-kill count moved"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 1u)   { ok = 0; why = "cleanup count"; }
    if (ok && t1 != t0)                  { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]  != e0[IT_SI_LIVE])  { ok = 0; why = "handle leak"; }
    /* Ledger A-22: a fault issues EXACTLY ONE reply capability — the
     * authority to resume the thread, and nothing else.  This used to assert
     * the counter did not move at all, because a fault issued no capability
     * and answering one was a syscall anybody holding the TCB could make. */
    if (ok && e1[IT_SI_REPLY] != e0[IT_SI_REPLY] + 1u) { ok = 0; why = "kreply drift"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping drift"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace drift"; }
    if (ok) it_pass("T146"); else it_fail("T146", why);
}
void test_t147(void) {
    uint32_t e0[14], e1[14], s3b[6], s3a[6], v0[5], v1[5], f0[6], f1[6];
    uint32_t t0 = 0, t1 = 0;
    it_quiesce_reaper();
    if (!it_sched_ext(e0) || !it_sched_ext3(s3b) || !it_sched_ext4(v0) ||
        !it_sched_ext5(f0) || !it_task_live(&t0)) { it_fail("T147", "sched ext"); return; }
    g_fz_seed = T147_SEED;
    int ok = 1;
    const char *why = "fault stress";
    uint32_t i = 0;

    for (i = 0; ok && i < T147_ROUNDS; i++) {
        /* Primary faulting child. */
        handle_id_t ep_h, proc_h, n_h, w_h;
        if (!it_fault_spawn(&ep_h, &proc_h, &n_h, &w_h, &why)) { ok = 0; break; }

        uint32_t kind = fz_rand() % 4u;
        uint32_t cmd  = (kind == 1u) ? LP_CMD_FAULT_WRITE
                      : (kind == 3u) ? LP_CMD_FAULT_EXEC
                                     : LP_CMD_FAULT_READ;
        uint64_t va   = (kind == 0u) ? T14X_BAD_VA
                      : (kind == 2u) ? T14X_KERN_VA
                                     : 0;
        if (it_lp_cmd_va(ep_h, cmd, va) != 0) { ok = 0; why = "cmd"; }
        if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no delivery"; }
        struct it_fault f;
        if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }
        if (ok && f.vector != 14u) { ok = 0; why = "vector"; }

        /* Second child suspended in fault at the same time: two live blocked
         * fault frames must coexist without corrupting each other. */
        handle_id_t ep2, pr2, n2, w2;
        int have2 = 0;
        if (ok && (fz_rand() & 1u)) {
            if (!it_fault_spawn_mbox(1u, &ep2, &pr2, &n2, &w2, &why)) { ok = 0; break; }
            have2 = 1;
            if (it_lp_cmd_va(ep2, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "cmd2"; }
            if (ok && !it_fault_wait_ep((long)n2, 1u)) { ok = 0; why = "no delivery 2"; }
        }

        /* Resolve the primary child. */
        uint32_t res = fz_rand() % 4u;
        if (ok && res == 0u) {
            /* resume → refault → kill (also re-proves F16 under churn). */
            if (it_fault_resume(0) != 0) {
                ok = 0; why = "resume";
            }
            if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no refault"; }
            if (ok && it_fault_kill(0) != 0) {
                ok = 0; why = "refault kill";
            }
        } else if (ok && res == 1u) {
            if (it_fault_kill(0) != 0) {
                ok = 0; why = "resume kill";
            }
        } else if (ok && res == 2u) {
            if (it_kill((long)proc_h) != 0) { ok = 0; why = "proc kill"; }
        } else if (ok) {
            it_close(&n_h);   /* handler drop first, then resolve via proc cap */
            if (it_fault_kill(0) != 0) {
                ok = 0; why = "post-close kill";
            }
        }
        if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "exit"; }
        /* A-22: whatever ended the fault — a reply, a refusal, or the thread
         * being destroyed under it — nothing can be answered for it now.  One
         * assertion covers all three because they all end the same way: the
         * reply capability has no caller. */
        if (ok && it_fault_resume(0) != (long)IRIS_ERR_NOT_FOUND) {
            ok = 0; why = "stale record";
        }
        (void)it_fault_kill(0);
        it_fault_close4(&ep_h, &proc_h, &n_h, &w_h);

        /* Resolve the second child (kill via whichever authority remains). */
        if (have2) {
            struct it_fault f2;
            if (ok && it_fault_info(1u, &f2) != 0) { ok = 0; why = "fault info 2"; }
            if (ok && ((fz_rand() & 1u)
                       ? it_fault_kill(1)
                       : it_kill((long)pr2)) != 0) {
                ok = 0; why = "resolve 2";
            }
            if (ok && it_lp_wait_exit(pr2) != 0) { ok = 0; why = "exit 2"; }
            (void)it_fault_kill(1);
            it_fault_close4(&ep2, &pr2, &n2, &w2);
        }

        /* Occasionally interleave a non-faulting child (own-text read). */
        if (ok && (fz_rand() & 1u)) {
            handle_id_t ep3, pr3, n3, w3;
            if (!it_fault_spawn_mbox(2u, &ep3, &pr3, &n3, &w3, &why)) { ok = 0; break; }
            if (it_lp_cmd_va(ep3, LP_CMD_FAULT_READ, 0) != 0) { ok = 0; why = "cmd3"; }
            if (ok) {
                long ec = it_lp_wait_exit(pr3);
                if ((ec >> 8) != (LP_EXIT_MARKER >> 8)) { ok = 0; why = "clean child"; }
            }
            it_fault_close4(&ep3, &pr3, &n3, &w3);
        }

        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && (!it_sched_ext(e1) || !it_sched_ext3(s3a) || !it_sched_ext4(v1) ||
               !it_sched_ext5(f1) || !it_task_live(&t1))) { ok = 0; why = "ext final"; }
    if (ok && t1 != t0)                    { ok = 0; why = "task live drift"; }
    if (ok && e1[IT_SI_LIVE]   != e0[IT_SI_LIVE])   { ok = 0; why = "handle leak"; }
    /* A-22: every fault this churn produced issued exactly one reply
     * capability, so the counter moves by at LEAST the number of rounds — a
     * floor rather than a fixed number, because the mix is seeded. */
    if (ok && e1[IT_SI_REPLY]  < e0[IT_SI_REPLY] + T147_ROUNDS) { ok = 0; why = "kreply drift"; }
    if (ok && s3a[IT_S3_NOTIF] != s3b[IT_S3_NOTIF]) { ok = 0; why = "notif obj leak"; }
    if (ok && s3a[IT_S3_EP]    != s3b[IT_S3_EP])    { ok = 0; why = "ep obj leak"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "mapping drift"; }
    if (ok && v1[IT_S4_VSLIVE]  != v0[IT_S4_VSLIVE])  { ok = 0; why = "vspace drift"; }
    if (ok && f1[IT_S5_DELIVER] < f0[IT_S5_DELIVER] + T147_ROUNDS) { ok = 0; why = "delivery floor"; }

    if (ok) it_pass("T147");
    else {
        fz_note("T147", T147_SEED, i);
        it_fail("T147", why);
    }
}

struct it_snap it_snap_take(void) {
    struct it_snap s;
    uint32_t e[14], w3[6], w4[5], w5[5];
    s.ok = 0;
    if (!it_task_live(&s.task) || !it_sched_ext(e) || !it_sched_ext3(w3) ||
        !it_sched_ext4(w4) || !it_sched_ext5(w5)) return s;
    s.hlive = e[IT_SI_LIVE]; s.ghwm = e[IT_SI_GHWM]; s.hmax = e[IT_SI_MAX];
    s.proclive = e[IT_SI_PROCLIVE]; s.reply = e[IT_SI_REPLY];
    s.ut = w3[IT_S3_UNTYPED]; s.fr = w3[IT_S3_FRAME]; s.ep = w3[IT_S3_EP];
    s.no = w3[IT_S3_NOTIF]; s.cn = w3[IT_S3_CNODE];
    s.vs = w4[IT_S4_VSLIVE]; s.map = w4[IT_S4_MAPLIVE];
    s.fdeliver = w5[IT_S5_DELIVER]; s.fclean = w5[IT_S5_CLEAN];
    s.ok = 1;
    return s;
}

/* Assert every LIVE gauge returned to baseline; *why names the first drift.
 * Cumulative counters are intentionally excluded. */
int it_snap_baseline(const struct it_snap *a, const struct it_snap *b,
                            const char **why) {
    if (!a->ok || !b->ok)          { *why = "snap read"; return 0; }
    if (b->task != a->task)        { *why = "task live drift"; return 0; }
    if (b->proclive != a->proclive){ *why = "proc live drift"; return 0; }
    if (b->hlive != a->hlive)      { *why = "handle leak"; return 0; }
    if (b->ut != a->ut)            { *why = "untyped drift"; return 0; }
    if (b->fr != a->fr)            { *why = "frame drift"; return 0; }
    if (b->ep != a->ep)            { *why = "endpoint drift"; return 0; }
    if (b->no != a->no)            { *why = "notif drift"; return 0; }
    if (b->cn != a->cn)            { *why = "cnode drift"; return 0; }
    if (b->vs != a->vs)            { *why = "vspace drift"; return 0; }
    if (b->map != a->map)          { *why = "mapping drift"; return 0; }
    /* The global handle high-water rule (ghwm*4 <= max) is a monotonic gauge,
     * not a per-test balance — it is asserted by T095/T112 in their own
     * contexts.  Applying it here would fire on HWM inherited from the heavy
     * fault-stress tests, so the fuzz baseline checks LIVE gauges only. */
    return 1;
}

/* Lifecycle-reliable subset of the baseline, for tests that SPAWN child
 * processes.  Loading a service via svc_load creates transient child-owned
 * objects (bootstrap endpoint, segment frames, CNode) whose reaping is
 * deferred, so the per-type KEndpoint/KFrame/KCNode/KUntyped/KNotification
 * OBJECT counts are not a reliable per-test balance across a child spawn —
 * the canonical cross-process churn test (T114) omits them for the same
 * reason.  The gauges that DO return exactly to baseline after child teardown
 * are task-live, process-live, handle-live, the KReply balance and the VM
 * books; those are what the fault suite (T145–T147) already proved reliable. */
int it_snap_baseline_live(const struct it_snap *a, const struct it_snap *b,
                                 const char **why) {
    if (!a->ok || !b->ok)          { *why = "snap read"; return 0; }
    if (b->task != a->task)        { *why = "task live drift"; return 0; }
    if (b->proclive != a->proclive){ *why = "proc live drift"; return 0; }
    if (b->hlive != a->hlive)      { *why = "handle leak"; return 0; }
    if (b->vs != a->vs)            { *why = "vspace drift"; return 0; }
    if (b->map != a->map)          { *why = "mapping drift"; return 0; }
    if (b->fr != a->fr)            { *why = "frame drift"; return 0; }
    return 1;
}

void it_fz_note(const char *t, uint32_t seed, uint32_t iter, uint32_t op) {
    it_serial_write("[IRIS][TEST] FZ ");
    it_serial_write(t);
    it_serial_write(" seed="); it_log_num(seed);
    it_serial_write(" iter="); it_log_num(iter);
    it_serial_write(" op="); it_log_num(op);
    it_serial_write("\n");
}

/* Boundary values invalid in BOTH namespaces the dual resolver consults —
 * the handle table AND the CSpace CNode — so they are safe to feed to
 * mutating syscalls without aliasing a real capability:
 *   - as a handle_id_t: all lack the HANDLE_TAG bit except 0xFFFFFFFF, whose
 *     slot is 1023 ≥ HANDLE_TABLE_MAX (256) → out of table range;
 *   - as a CPtr: each addresses a root slot followed by bits that cannot be
 *     descended into (the root slot holds no CNode), which is a malformed
 *     CPtr → INVALID_ARG.
 * That second clause is a property of the resolver, not of the numbers.  It
 * used to be "all are > 1023, and the CPtr range stops at 1024", which stopped
 * being true twice over: CPtrs own the low 31 bits now, and the resolver used
 * to DISCARD the leftover bits, so 4095 quietly aliased root slot 255 — this
 * suite's serial KIoPort.  T295 pins the injectivity these values rely on.
 * Small integers (0,1,2,…) are deliberately EXCLUDED: as CPtrs they alias
 * real caps (CPtr 1 = svcmgr EP), so honouring them is correct, not a bug. */
const long it_fz_bad_handles[] = {
    4095L, 0x1FFFFL, 0x7FFFFFFFL, (long)0xFFFFFFFFUL,
};

/* ── T148: syscall table / retired syscall fuzz ─────────────────────────────
 * Invoke every non-live syscall number — retired, reserved, never-assigned,
 * and out-of-range high — with garbage arguments, plus a batch of huge
 * numbers whose low bits alias a live handler (the dispatch is an exact-match
 * switch, so 0x1_0000_0000 | live must NOT alias).  Every one must return
 * NOT_SUPPORTED and touch nothing.  No live number is ever fuzzed here (SYS_EXIT
 * etc. would self-destruct — the table below is holes only).
 * Invariants: X1, X9, X15, X16, X18, X23. */
void test_t148(void) {
    /* Holes in 0..107 (every number NOT routed by syscall_dispatch). */
    /* Holes plus the routed-but-RETIRED numbers, which are indistinguishable
     * from a hole at the ABI: both answer NOT_SUPPORTED and touch nothing.
     * 43 = SYS_IOPORT_RESTRICT and 90 = SYS_CNODE_FETCH joined them in Stage 4
     * (both were handle producers with no CSpace form and no callers), and
     * 45 = SYS_BOOTCAP_RESTRICT in Stage 5. */
    static const long retired[] = {
        0, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 18, 23, 24, 25, 30, 31, 34,
        37, 38, 41, 42, 43, 44, 46, 59, 63, 72, 78, 79, 80, 90,
        /* Stage 4 closeout: the handle namespace's own surface. */
        15, 22, 52, 53, 81, 87, 89, 95,
        /* Stage 5 Step 2: 45 = SYS_BOOTCAP_RESTRICT.  Narrowing a boot
         * capability by cloning a weaker copy of it has no meaning once each
         * capability carries exactly one authority.
         * Stage 5 Step 4: 48 = SYS_THREAD_CREATE.  A thread carved from the
         * kernel's static pool, authorised by nothing and identified by a
         * global id, is replaced by a TCB retyped from an Untyped and
         * configured with CSpace/VSpace capabilities.
         * Stage 7: 58 = SYS_THREAD_START, the LAST pool-born execution path —
         * a spawned process's first thread.  It survived Step 4 only because
         * a spawner could not name its child's CSpace and VSpace; Stage 6-pure
         * made it retype both, so the child's first thread is composed the
         * same way any other is. */
        45, 48, 58,
    };
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "retired fuzz";
    g_fz_seed = 0x21C0DE01u;

    for (int i = 0; ok && i < (int)(sizeof(retired) / sizeof(retired[0])); i++) {
        long a0 = (long)fz_rand(), a1 = (long)fz_rand(), a2 = (long)fz_rand();
        if (it_sys3(retired[i], a0, a1, a2) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "retired not NOT_SUPPORTED";
            it_fz_note("T148", g_fz_seed, (uint32_t)i, (uint32_t)retired[i]);
            break;
        }
    }
    /* High/unassigned range 114..400 (111 = SYS_UNTYPED_RETYPE2, 112 =
     * SYS_UNTYPED_QUERY, Phase S2's 113 = SYS_SC_BIND are live; 107..110 remain
     * live from Fases 25/26/29). */
    /* Phase S3: 114-116 are SYS_CSPACE_MINT/REVOKE/MINT_INTO.  Phase S4/Stage 4:
     * 117-118 are SYS_CAP_IDENTIFY/SYS_CAP_SAME_OBJECT — the CSpace-native
     * introspection that replaces SYS_HANDLE_TYPE/SAME_OBJECT.  Stage 5
     * Step 4: 119-121 are SYS_CSPACE_SELF / SYS_TCB_CONFIGURE /
     * SYS_TCB_WRITE_REGS — execution for a TCB retyped from an Untyped.
     * Stage 6-pure Step 1: 122 is SYS_VSPACE_MAP_TABLE, which installs a page
     * table the holder retyped.  Stage 7 Steps 8/10: 123-125 are
     * SYS_TCB_FAULT_INFO, SYS_TCB_WATCH and SYS_TCB_EXIT_CODE — a fault read
     * off the thread that took it, and a death observed on the thread that
     * dies.  Stage 7 Step 12: 126 is SYS_TCB_SET_FAULT_HANDLER — faults armed
     * on the execution that takes them.  Stage 8-cap: 127 is
     * SYS_CSPACE_SET_GUARD, which installs a guard on a CNode capability
     * (ledger D-2).  Stage 8-mcs: 128 is SYS_TCB_SET_TIMEOUT_HANDLER — a
     * thread's budget exhaustion delivered as a fault to a temporal
     * supervisor.  Stage 8-mcs: 129 is SYS_REPLY_RECV — seL4's ReplyRecv,
     * which a passive server needs so it never crosses the gap between giving
     * its donated time back and blocking again.  Stage 8-cap: 130 is
     * SYS_TCB_SET_IPC_BUFFER — a thread's bulk-payload buffer becomes a frame
     * it owns instead of 256 bytes inside its TCB (ledger D-4).  Stage 5: 131
     * is SYS_IOPORT_CONTROL_NARROW — the kernel's hardcoded port whitelist
     * becomes a range carried ON the authority, so who may claim which ports
     * is something a supervisor decides rather than something the kernel
     * asserts for everyone.  Stage 6: 132 is
     * SYS_UNTYPED_SET_DEVICE_BUDGET — a DEVICE untyped cannot hold the headers
     * of objects carved from it (MMIO is not storage), so the holder names the
     * RAM that pays for them (ledger D-9).  The first UNASSIGNED number moves
     * Stage 6: 133 is SYS_FRAMEBUFFER_INFO — the geometry alone, separated
     * from the VMO its predecessor fabricated in the same call.  Stage 6/D-5:
     * 134 is SYS_INITRD_FRAME — a boot image as a FRAME rather than a KVMO,
     * which is how the loader and vfs stopped speaking a second memory ABI to
     * read a file the kernel already had.  Ledger A-21: 135 is
     * SYS_ASID_POOL_ASSIGN — an address space gets its hardware identifier
     * from a POOL somebody holds, so building one and making one RUNNABLE
     * became two grants instead of a kernel-side bitmap nobody could name.
     * The first UNASSIGNED number moves up to 136.
     *
     * This loop caught the guard syscall the moment it landed, which is what
     * it is for: growing the syscall surface has to be a deliberate, visible
     * act rather than something a diff can do quietly. */
    for (long n = 144; ok && n <= 400; n++) {
        if (it_sys3(n, (long)fz_rand(), (long)fz_rand(), (long)fz_rand())
            != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "high not NOT_SUPPORTED";
            it_fz_note("T148", g_fz_seed, (uint32_t)n, (uint32_t)n);
            break;
        }
    }
    /* Numbers whose low 32 bits alias a live handler must NOT dispatch it —
     * the switch matches the full 64-bit value.  (SYS_GETPID=2 chosen: a live
     * dispatch would return >= 0, not NOT_SUPPORTED.) */
    if (ok) {
        long alias[] = { (long)0x100000002LL, (long)0x200000002LL, (long)-1L };
        for (int i = 0; ok && i < 3; i++) {
            if (it_sys3(alias[i], 0, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
                ok = 0; why = "alias dispatched live handler";
            }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T148"); else it_fail("T148", why);
}
