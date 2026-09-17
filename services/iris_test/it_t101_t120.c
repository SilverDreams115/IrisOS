/*
 * it_t101_t120.c — tests T101 through T120.
 *
 * The suite's numbering is chronological, not thematic: T101 was written
 * stages before T120, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T101 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
/* ── T101: cross-process receive-slot death cleanup ─────────────────────────
 * A child killed while blocked with a declared receive-slot leaves a clean
 * endpoint (no dead waiter), the sender's cap survives an attempted
 * delivery (WOULD_BLOCK, handle intact), and the handle books show no
 * staged-cap leak and no runaway high-water. */
void test_t101(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T101", "sched ext"); return; }
    long ep = it_ep_create();
    long n  = it_notify_create();
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
    handle_id_t proc_h = HANDLE_INVALID;
    int ok = 1;
    const char *why = "death cleanup";

    if (ep < 0 || n < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
        ok = 0; why = "spawn";
    }
    if (ok && it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "cmd"; }
    it_settle(2);   /* child re-blocks with slot 40 declared */

    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_alive((long)proc_h) != 0) {
        ok = 0; why = "still alive";
    }

    /* Sender does not lose its cap on the failed delivery attempt. */
    if (ok) {
        long d = it_xfer_dup( n,
                         (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
        else {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0x99;
            m.cap = (uint32_t)d;
            m.cap_rights = RIGHT_WRITE;
            if (iris_msg_nb_send((long)ep_h, &m) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
            if (ok && !it_slot_is_notif(d)) {

                ok = 0; why = "cap consumed";
            }
            handle_id_t dh = (handle_id_t)d;
            it_close(&dh);
        }
    }

    it_close(&proc_h);
    it_close(&n_h);
    it_close(&ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) {
        ok = 0; why = "staged leak";
    }
    /* No runaway high-water: still bounded by the T095 rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) {
        ok = 0; why = "hwm runaway";
    }
    if (ok) it_pass("T101"); else it_fail("T101", why);
}

/* ── T102: a child that declares no slot receives no capability ─────────────
 * This used to assert the opposite — that a child receiving with slot 0 got
 * the transferred cap as a handle and could invoke it across the process
 * boundary.  Stage 4 retired that delivery, and the guarantee that replaces it
 * is asserted here across a REAL process boundary, which is where it matters:
 * a receiver that names no destination gets the message and not the
 * capability, its exit code says so, and nothing is left half-transferred —
 * the sender is not blocked, the child runs to completion, and the parent's
 * live-handle count returns exactly to baseline.
 *
 * Two children, because the original failure mode this guarded against was a
 * per-child residue that only showed up on the second pass. */
void test_t102(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T102", "sched ext"); return; }
    int ok = 1;
    const char *why = "slotless children";

    for (int i = 0; ok && i < 2; i++) {
        long ep = it_ep_create();
        long n  = it_notify_create();
        handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || n < 0 ||
            lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_lp_cmd_rslot(ep_h, 0u) != 0) { ok = 0; why = "cmd"; }
        /* The SEND itself succeeds: the failure is closed at delivery, not
         * reported to the sender, exactly as an occupied slot behaves. */
        if (ok && it_lp_send_cap(ep_h, n) != 0) { ok = 0; why = "send cap"; }
        if (ok) {
            long ec = it_lp_wait_exit(proc_h);
            if (ec != 0) { ok = 0; why = "cap delivered without a slot"; }
        }
        /* The notification was never signalled — nobody could have. */
        /* The child has exited, so one tick proves nothing ever signalled. */
        if (ok) {
            uint64_t bits = 0;
            if (it_wait_timeout( n, (long)(uintptr_t)&bits, 1)
                != (long)IRIS_ERR_TIMED_OUT) { ok = 0; why = "phantom signal"; }
        }
        it_close(&proc_h);
        it_close(&n_h);
        it_close(&ep_h);
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* Not one of those deliveries went to a handle. */
    if (ok && after[IT_SI_HANDDEL] != before[IT_SI_HANDDEL]) {
        ok = 0; why = "hand count";
    }
    if (ok) it_pass("T102"); else it_fail("T102", why);
}

/* ── A1.10: staged-cap atomicity for blocking IPC paths (T103–T106) ─────────
 * The A1.9 rule locked for EP_NB_SEND — "a failed delivery never consumes
 * the source cap" — extended to the blocking paths: a sender canceled while
 * queued (endpoint close), an EP_CALL canceled before rendezvous, and a
 * reply that loses the one-shot race all keep their source cap; endpoint
 * close with multiple staged waiters releases every staging ref exactly
 * once.  Two-thread pattern (T019/T020 style): the victim blocks with an
 * attached cap, the main thread cancels, and the source handle is probed
 * with SYS_HANDLE_TYPE afterwards. */

static handle_id_t  g_t103_ep_h   = HANDLE_INVALID;
static long         g_t103_dup    = -1;
static volatile int g_t103_done   = 0;
static          int g_t103_result = 0;
static uint8_t      g_t103_stack[8192];

static void t103_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x103;
    m.cap = (uint32_t)g_t103_dup;
    m.cap_rights = RIGHT_WRITE;
    long r = iris_msg_send((long)g_t103_ep_h, &m);
    g_t103_result = (int)r;
    g_t103_done   = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T103: blocking send canceled before delivery preserves source cap ──────
 * A sender blocks in EP_SEND with an attached cap (no receiver ever shows
 * up); the endpoint is then closed.  The sender must wake with CLOSED, its
 * source handle must still be alive (never consumed — the A1.10 two-phase
 * commit), no cap can have appeared anywhere, and the handle books must
 * return exactly to baseline (no staged-ref leak). */
void test_t103(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T103", "sched ext"); return; }
    g_t103_done = 0; g_t103_result = 0;
    int ok = 1;
    const char *why = "blocking send cancel";

    long ep = it_ep_create();
    long n  = it_notify_create();
    g_t103_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T103", "create"); return; }

    /* Phase S4 (Step 2): the staged SOURCE is a CSpace slot. */
    g_t103_dup = it_xfer_dup(n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (g_t103_dup < 0) { ok = 0; why = "xfer slot"; }

    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t103_sender;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t103_stack + sizeof(g_t103_stack))) & ~0xFULL;
        /* returns a task id, not a handle — nothing to close */
        if (it_thread_create(entry, rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    if (ok) {
        it_settle(5);           /* sender queues with staged cap */
        it_close(&g_t103_ep_h);          /* last ref → close → wakes sender */
        for (int i = 0; i < 200 && !g_t103_done; i++)
            it_settle(1);
        if (!g_t103_done || g_t103_result != (int)IRIS_ERR_CLOSED) {
            ok = 0; why = "not CLOSED";
        }
    }

    /* Source cap preserved: the source SLOT still resolves (never consumed —
     * the cancel path aborts staging without deleting it). */
    if (ok && !it_slot_is_notif(g_t103_dup)) { ok = 0; why = "cap consumed"; }
    /* And it still works: signal through it, observe on the original. */
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(g_t103_dup, INV_NOTIFY_SIGNAL, 1) != 0 ||
            it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 1u) { ok = 0; why = "cap dead"; }
    }

    if (g_t103_dup >= 0) it_slot_delete((uint32_t)g_t103_dup);
    it_close(&n_h);
    it_close(&g_t103_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* Exact balance: +1 = the exited thread's KTcb handle, which stays with
     * Step 4: thread creation no longer publishes a KTcb handle into the
     * process by construction, so the expected delta is ZERO rather than one.
     * The property is unchanged — anything above the baseline is a staged
     * leak; only the baseline moved, because the producer it accounted for is
     * retired. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok) it_pass("T103"); else it_fail("T103", why);
}

static handle_id_t  g_t104_ep_h   = HANDLE_INVALID;
static long         g_t104_dup    = -1;
static volatile int g_t104_done   = 0;
static          int g_t104_result = 0;
static uint8_t      g_t104_stack[8192];

static void t104_caller(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label               = 0x104;
    m.cap        = (uint32_t)g_t104_dup;
    m.cap_rights = RIGHT_WRITE;
    long r = iris_msg_call((long)g_t104_ep_h, &m);
    g_t104_result = (int)r;
    g_t104_done   = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T104: EP_CALL canceled before server receive preserves attached cap ────
 * A caller blocks in EP_CALL carrying a transferred cap (attached_cap);
 * the endpoint closes before any server ever receives.  The caller must
 * wake with CLOSED, keep its source cap, and no KReply may have been
 * created (the reply-caps counter stays flat — reply cleanup is trivially
 * correct because rendezvous never happened). */
void test_t104(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T104", "sched ext"); return; }
    g_t104_done = 0; g_t104_result = 0;
    int ok = 1;
    const char *why = "ep_call cancel";

    long ep = it_ep_create();
    long n  = it_notify_create();
    g_t104_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T104", "create"); return; }

    /* Phase S4 (Step 2): the staged EP_CALL source is a CSpace slot. */
    g_t104_dup = it_xfer_dup(n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
    if (g_t104_dup < 0) { ok = 0; why = "xfer slot"; }

    if (ok) {
        uint64_t entry = (uint64_t)(uintptr_t)t104_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t104_stack + sizeof(g_t104_stack))) & ~0xFULL;
        /* returns a task id, not a handle — nothing to close */
        if (it_thread_create(entry, rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    if (ok) {
        it_settle(5);           /* caller queues (SEND, call mode) */
        it_close(&g_t104_ep_h);
        for (int i = 0; i < 200 && !g_t104_done; i++)
            it_settle(1);
        if (!g_t104_done || g_t104_result != (int)IRIS_ERR_CLOSED) {
            ok = 0; why = "not CLOSED";
        }
    }

    if (ok && !it_slot_is_notif(g_t104_dup)) { ok = 0; why = "cap consumed"; }


    if (g_t104_dup >= 0) it_slot_delete((uint32_t)g_t104_dup);
    it_close(&n_h);
    it_close(&g_t104_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* +1 = the exited thread's KTcb handle (stays with the process, Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY]) {
        ok = 0; why = "ghost kreply";
    }
    if (ok) it_pass("T104"); else it_fail("T104", why);
}

static handle_id_t  g_t105_ep_h   = HANDLE_INVALID;
static volatile int g_t105_done   = 0;
static          int g_t105_result = 0;
static uint8_t      g_t105_stack[8192];

static void t105_caller(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label    = 0x105;
    long r = iris_msg_call((long)g_t105_ep_h, &m);
    g_t105_result = (int)(r == 0 && m.label == 0x5A5AULL);
    g_t105_done   = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T105: reply cap transfer failure is atomic ─────────────────────────────
 * EP_REPLY supports one attached cap (Phase 7.1).  The deterministic failed
 * delivery is the lost one-shot race: reply once without a cap (consumes
 * the KReply), then reply AGAIN with an attached cap.  The second reply
 * must fail NOT_FOUND, the server must KEEP its source cap (A1.10 — before,
 * this path destroyed it), and the first reply's one-shot semantics and
 * bookkeeping stay intact. */
void test_t105(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T105", "sched ext"); return; }
    g_t105_done = 0; g_t105_result = 0;
    int ok = 1;
    const char *why = "reply cap atomic";

    long ep = it_ep_create();
    long n  = it_notify_create();
    g_t105_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T105", "create"); return; }

    handle_id_t reply_h = HANDLE_INVALID;
    {
        uint64_t entry = (uint64_t)(uintptr_t)t105_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t105_stack + sizeof(g_t105_stack))) & ~0xFULL;
        /* returns a task id, not a handle — nothing to close */
        if (it_thread_create(entry, rsp, 0) < 0) {
            ok = 0; why = "thread";
        }
    }

    /* Serve the call: the explicit reply object (slot 95) is staged via
     * recv arg2 and echoed back in attached_handle (Phase S1). */
    if (ok && it_reply_create_at(95) < 0) { ok = 0; why = "reply create"; }
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        if ((m.reply = 95, iris_msg_recv((long)g_t105_ep_h, &m)) != 0 ||
            m.label != 0x105ULL || m.got_cap == IRIS_MSG_NO_CAP) {
            ok = 0; why = "recv call";
        } else {
            reply_h = (handle_id_t)m.got_cap;
        }
    }

    /* First reply (no cap) succeeds and unblocks the caller. */
    if (ok) {
        struct iris_msg rm;
        iris_msg_zero(&rm);
        rm.label = 0x5A5A;
        if (iris_msg_reply((long)reply_h, &rm) != 0) {
            ok = 0; why = "first reply";
        }
        for (int i = 0; ok && i < 200 && !g_t105_done; i++)
            it_settle(1);
        if (ok && (!g_t105_done || !g_t105_result)) { ok = 0; why = "caller"; }
    }

    /* Second reply WITH a cap loses the one-shot race: NOT_FOUND and the
     * server's source cap must survive un-consumed. */
    long d = -1;
    if (ok) {
        d = it_xfer_dup( n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
    }
    if (ok) {
        struct iris_msg rm;
        iris_msg_zero(&rm);
        rm.label           = 0xDEAD;
        rm.cap = (uint32_t)d;
        rm.cap_rights = RIGHT_WRITE;
        if (iris_msg_reply((long)reply_h, &rm) !=
            (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "not one-shot"; }
        if (ok && !it_slot_is_notif(d)) { ok = 0; why = "cap consumed"; }

    }

    if (d >= 0) { handle_id_t dh = (handle_id_t)d; it_close(&dh); }
    it_close(&n_h);
    it_close(&g_t105_ep_h);
    it_slot_delete(95);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* +1 = the exited thread's KTcb handle (stays with the process, Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok) it_pass("T105"); else it_fail("T105", why);
}

static handle_id_t  g_t106_ep_h = HANDLE_INVALID;
static long         g_t106_dup[2]    = { -1, -1 };
static volatile int g_t106_done[2]   = { 0, 0 };
static          int g_t106_result[2] = { 0, 0 };
static uint8_t      g_t106_stack_a[8192];
static uint8_t      g_t106_stack_b[8192];

static void t106_send_idx(int idx) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x106;
    m.cap = (uint32_t)g_t106_dup[idx];
    m.cap_rights = RIGHT_WRITE;
    long r = iris_msg_send((long)g_t106_ep_h, &m);
    g_t106_result[idx] = (int)r;
    g_t106_done[idx]   = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void t106_sender_a(void) { t106_send_idx(0); }
static void t106_sender_b(void) { t106_send_idx(1); }

/* ── T106: endpoint close cancels staged waiters cleanly ────────────────────
 * TWO senders queue on the same endpoint, each with its own staged cap;
 * the endpoint closes.  Both must wake with CLOSED, both source caps must
 * survive with their owners, and the books must balance exactly (each
 * staging ref released exactly once — a double-release would show up as a
 * refcount crash or a negative live delta). */
void test_t106(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T106", "sched ext"); return; }
    g_t106_done[0] = g_t106_done[1] = 0;
    g_t106_result[0] = g_t106_result[1] = 0;
    int ok = 1;
    const char *why = "close staged waiters";

    long ep = it_ep_create();
    long n  = it_notify_create();
    g_t106_ep_h = (handle_id_t)ep;
    handle_id_t n_h = (handle_id_t)n;
    if (ep < 0 || n < 0) { it_fail("T106", "create"); return; }

    for (int i = 0; ok && i < 2; i++) {
        /* Phase S4 (Step 2): each staged source is its own CSpace slot. */
        g_t106_dup[i] = it_xfer_dup(n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (g_t106_dup[i] < 0) { ok = 0; why = "xfer slot"; }
    }

    if (ok) {
        uint64_t ea = (uint64_t)(uintptr_t)t106_sender_a;
        uint64_t eb = (uint64_t)(uintptr_t)t106_sender_b;
        uint64_t ra = ((uint64_t)(uintptr_t)(g_t106_stack_a + sizeof(g_t106_stack_a))) & ~0xFULL;
        uint64_t rb = ((uint64_t)(uintptr_t)(g_t106_stack_b + sizeof(g_t106_stack_b))) & ~0xFULL;
        /* returns task ids, not handles — nothing to close */
        long ta = it_thread_create(ea, ra, 0);
        long tb = it_thread_create(eb, rb, 0);
        if (ta < 0 || tb < 0) { ok = 0; why = "thread"; }
    }

    if (ok) {
        it_settle(5);           /* both senders queue staged caps */
        it_close(&g_t106_ep_h);
        for (int i = 0; i < 200 && !(g_t106_done[0] && g_t106_done[1]); i++)
            it_settle(1);
        if (!g_t106_done[0] || !g_t106_done[1] ||
            g_t106_result[0] != (int)IRIS_ERR_CLOSED ||
            g_t106_result[1] != (int)IRIS_ERR_CLOSED) {
            ok = 0; why = "not CLOSED";
        }
    }

    for (int i = 0; ok && i < 2; i++) {
        if (!it_slot_is_notif(g_t106_dup[i])) { ok = 0; why = "cap consumed"; }

    }

    for (int i = 0; i < 2; i++) {
        if (g_t106_dup[i] >= 0) it_slot_delete((uint32_t)g_t106_dup[i]);
    }
    it_close(&n_h);
    it_close(&g_t106_ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* +2 = the two exited threads' KTcb handles (stay with the process, Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok) it_pass("T106"); else it_fail("T106", why);
}

/* ── A1.11: deterministic IPC fuzz/stress harness (T107–T112) ───────────────
 *
 * Goal: break IPC/lifecycle/cap-transfer if a bug exists, reproducibly.
 * Design:
 *   - xorshift32 PRNG with a FIXED per-test seed: the operation sequence is
 *     identical on every run; a failure logs "FZ <test> seed=<s> iter=<i>".
 *   - bounded iterations; bounded retry/poll loops (no unbounded waits);
 *   - synchronization is the blocking-endpoint rendezvous itself: command
 *     handoff over a control endpoint blocks until the worker is at its
 *     recv, so no fragile external timing is needed.  The only sleeps are
 *     the short "let the worker reach its blocking syscall" pauses already
 *     used by T019/T020/T101/T103.
 *   - CSpace slots CANNOT be deleted from userland (no root-CNode accessor
 *     by design), so delivered/occupied slots are allocated monotonically
 *     from a budgeted window (FZ_SLOT_BASE..FZ_SLOT_LIMIT) — deterministic
 *     and bounded; the budget check fails loudly if a test overdraws.
 *   - every test snapshots the A1.7 counters before/after and asserts
 *     exact live-handle balance (KTcb handles from worker threads are a
 *     documented +N, as in A1.10), directional slot/handle-delivery deltas
 *     (>=: background services also move the global counters), and the
 *     T095 high-water rule (global_hwm * 4 <= max).
 *
 * Invariants (docs/architecture/ipc-stress-invariants.md): I1 no authority
 * without cap; I2 no fallback after ACCESS_DENIED; I3 no silent slot
 * overwrite; I4 occupied slot fails/degrades per contract; I5 sender keeps
 * cap without delivery commit; I6 receiver gains nothing on failure; I7 no
 * staged-cap leak; I8 no double release; I9 reply one-shot; I10 second
 * reply keeps server cap; I11 legacy slot-0 delivery; I12 slot delivery is
 * an invocable CPtr; I13 NOT_FOUND installs nothing; I14 close wakes
 * waiters; I15 death leaves no dead waiters; I16 live back to baseline;
 * I17 hwm bounded; I18 delivery counters move as expected. */

/* xorshift32 — deterministic, seeded per test. */
uint32_t g_fz_seed;
uint32_t fz_rand(void) {
    uint32_t x = g_fz_seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_fz_seed = x;
    return x;
}
static uint32_t g_fz_slot_next = FZ_SLOT_BASE;
static uint32_t fz_slot_alloc(void) {
    if (g_fz_slot_next >= FZ_SLOT_LIMIT) return 0;   /* budget blown → caller fails */
    return g_fz_slot_next++;
}

/* Failure locator: printed ONLY on failure, right before it_fail. */
void fz_note(const char *t, uint32_t seed, uint32_t iter) {
    it_serial_write("[IRIS][TEST] FZ ");
    it_serial_write(t);
    it_serial_write(" seed=");
    it_log_num(seed);
    it_serial_write(" iter=");
    it_log_num(iter);
    it_serial_write("\n");
}

static handle_id_t       g_fz_ctl[2]  = { HANDLE_INVALID, HANDLE_INVALID };
handle_id_t       g_fz_data_ep = HANDLE_INVALID;
static volatile long     g_fz_res[2];
static volatile uint32_t g_fz_att[2];      /* where a delivered cap landed */
static volatile uint32_t g_fz_attcap[2];   /* the reply object it was owed */
static volatile int      g_fz_done[2];
static uint8_t           g_fz_stk[2][8192];

static void fz_worker(int idx) {
    for (;;) {
        struct iris_msg c;
        iris_msg_zero(&c);
        if (iris_msg_recv((long)g_fz_ctl[idx], &c) != 0) break;
        uint32_t op = (uint32_t)c.words[0];
        if (op == FZ_OP_EXIT) break;

        struct iris_msg m;
        iris_msg_zero(&m);
        long r = -1;
        if (op == FZ_OP_RECV) {
            m.recv_slot = (uint32_t)c.words[1];   /* slot hint (0 = legacy) */
            r = iris_msg_recv((long)g_fz_data_ep, &m);
        } else if (op == FZ_OP_SEND_CAP) {
            m.label = c.words[3];
            if (c.words[1]) {
                m.cap = (uint32_t)c.words[1];
                m.cap_rights = (uint32_t)c.words[2];
            }
            r = iris_msg_send((long)g_fz_data_ep, &m);
        } else if (op == FZ_OP_CALL) {
            m.label = 0xF2;
            if (c.words[1]) {
                m.cap        = (uint32_t)c.words[1];
                m.cap_rights = (uint32_t)c.words[2];
            }
            m.recv_slot = (uint32_t)c.words[3]; /* reply slot (0 = legacy) */
            r = iris_msg_call((long)g_fz_data_ep, &m);
        }
        /* A-33: "did a capability arrive, and where?" is two facts now.  The
         * MessageInfo says whether one landed (seL4's `extraCaps`) and the
         * receiver already knows the slot, because it declared it; `got_cap`
         * is the reply object, which is a different question entirely. */
        g_fz_att[idx]    = m.got_caps ? (uint32_t)m.recv_slot
                                      : (uint32_t)IRIS_MSG_NO_CAP;
        g_fz_attcap[idx] = (uint32_t)m.got_cap;
        g_fz_res[idx]    = r;
        __asm__ volatile ("" ::: "memory");
        g_fz_done[idx]   = 1;
    }
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void fz_worker0(void) { fz_worker(0); }
static void fz_worker1(void) { fz_worker(1); }

/* Start `n` workers (1 or 2).  Returns 1 on success. */
static int fz_workers_start(int n) {
    static void (*const entries[2])(void) = { fz_worker0, fz_worker1 };
    for (int i = 0; i < n; i++) {
        /* The control endpoint outlives every test: the workers run until
         * FZ_OP_EXIT.  It gets a FIXED slot, not a pool — the rotating pool is
         * for objects that die with their test, and a wrap onto this one would
         * delete it, kill the worker's EP_RECV and block the main thread on a
         * worker that no longer exists. */
        uint32_t ctl = IT_FZ_CTL_SLOT + (uint32_t)i;
        it_slot_delete(ctl);
        if (it_retype2_at((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_ENDPOINT,
                          ctl, 1u, 0) != 0) return 0;
        g_fz_ctl[i] = (handle_id_t)ctl;
        uint64_t entry = (uint64_t)(uintptr_t)entries[i];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_fz_stk[i] + sizeof(g_fz_stk[i]))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) return 0;
    }
    return 1;
}

/* Send a command to worker `idx`; blocks until the worker picks it up. */
static int fz_cmd(int idx, uint32_t op, uint64_t a, uint64_t b, uint64_t c) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = 0xFC;
    m.words[0]   = op;
    m.words[1]   = a;
    m.words[2]   = b;
    m.words[3]   = c;
    m.word_count = 4u;
    g_fz_done[idx] = 0;
    return iris_msg_send((long)g_fz_ctl[idx], &m) == 0;
}

/* Bounded wait for worker `idx` to publish a result. */
static int fz_wait(int idx) {
    IT_AWAIT(g_fz_done[idx], 4000);
    return g_fz_done[idx];
}

static void fz_workers_stop(int n) {
    for (int i = 0; i < n; i++) {
        if (g_fz_ctl[i] != HANDLE_INVALID) {
            (void)fz_cmd(i, FZ_OP_EXIT, 0, 0, 0);
            it_close(&g_fz_ctl[i]);
        }
    }
}

/* Dup a WRITE|TRANSFER cap of `src` for staging (returns handle or -err). */
/* Phase S4 (Step 2): a transfer source is a CSpace slot, not a handle. */
static long fz_dup_xfer(long src) {
    return it_xfer_dup(src, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
}

void test_t107(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T107", "sched ext"); return; }
    g_fz_seed = T107_SEED;
    int ok = 1;
    const char *why = "rslot stress";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0;

    long ep    = it_ep_create_slot();   /* data endpoint */
    long n     = it_notify_create_slot();     /* transferable notification */
    long ep2   = it_ep_create_slot();   /* transferable endpoint */
    /* Stage 4: invoked as a CPtr; never materialised into a handle. */
    const long selfp = (it_invoke0((long)IRIS_CPTR_TEST_PROC, INV_CAP_IDENTIFY) >= 0)
                       ? (long)IRIS_CPTR_TEST_PROC : -1;
    handle_id_t n_h = (handle_id_t)n, ep2_h = (handle_id_t)ep2;
    g_fz_data_ep = (handle_id_t)ep;
    if (ep < 0 || n < 0 || ep2 < 0 || selfp < 0) { it_fail("T107", "create"); return; }
    if (!fz_workers_start(1)) { it_fail("T107", "worker"); return; }

    for (it_n = 0; ok && it_n < T107_ITERS; it_n++) {
        uint32_t pick = fz_rand() % 8u;

        if (pick == 0u) {
            /* NB send, no receiver → WOULD_BLOCK; nothing changes. */
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = 0xF0;
            if (iris_msg_nb_send((long)g_fz_data_ep, &m) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "nb empty"; }

        } else if (pick == 1u || pick == 7u) {
            /* Blocking EP_SEND of a cap into a FRESH declared slot.
             * pick 1 = notification cap, pick 7 = endpoint cap. */
            uint32_t s = fz_slot_alloc();
            long src = (pick == 1u) ? n : ep2;
            long d = fz_dup_xfer(src);
            if (s == 0u || d < 0) { ok = 0; why = "slot/dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, s, 0, 0)) { ok = 0; why = "cmd"; break; }
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0xF1;
            m.cap = (uint32_t)d;
            m.cap_rights = RIGHT_WRITE;
            if (iris_msg_send((long)g_fz_data_ep, &m) != 0) {
                ok = 0; why = "send";
            }
            if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != 0 || g_fz_att[0] != s)) {
                ok = 0; why = "slot landing";
            }
            /* I12: the CPtr is invocable. */
            if (ok && pick == 1u &&
                it_invoke1((long)s, INV_NOTIFY_SIGNAL, 1) != 0) {
                ok = 0; why = "cptr signal";
            }
            if (ok && pick == 1u) {
                uint64_t bits = 0;
                if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
                    bits == 0u) { ok = 0; why = "signal lost"; }
            }
            if (ok && pick == 7u) {
                struct iris_msg pm;
                iris_msg_zero(&pm);     /* clean probe: no stale attached cap */
                pm.label = 0xF9;
                if (iris_msg_nb_send((long)s, &pm) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "cptr ep"; }
            }
            /* Ledger A-29: COPY semantics — the sender KEEPS what it sent,
             * as seL4 does, and the receiver's capability is a derivation
             * child of this slot.  It used to be a move, and the delivery
             * installed the child and then deleted its parent. */
            if (ok && it_invoke0(d, INV_CAP_IDENTIFY) < 0) {
                ok = 0; why = "source lost on transfer";
            }
            it_xfer_release(d);          /* meant as a give-away: drop it */
            exp_slot++;

        } else if (pick == 2u) {
            /* NB_SEND of a cap to a slot-declared receiver (bounded retry
             * until the worker is queued). */
            uint32_t s = fz_slot_alloc();
            long d = fz_dup_xfer(n);
            if (s == 0u || d < 0) { ok = 0; why = "slot/dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, s, 0, 0)) { ok = 0; why = "cmd"; break; }
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0xF3;
            m.cap = (uint32_t)d;
            m.cap_rights = RIGHT_WRITE;
            long r = (long)IRIS_ERR_WOULD_BLOCK;
            for (int i = 0; i < 400 && r == (long)IRIS_ERR_WOULD_BLOCK; i++) {
                r = iris_msg_nb_send((long)g_fz_data_ep, &m);
                if (r == (long)IRIS_ERR_WOULD_BLOCK) it_sys0(SYS_YIELD);
            }
            if (r != 0) { ok = 0; why = "nb send"; }
            if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != 0 || g_fz_att[0] != s)) {
                ok = 0; why = "nb slot landing";
            }
            /* A-29: NB_SEND transfers by copy too — same rule, same tree. */
            if (ok && it_invoke0(d, INV_CAP_IDENTIFY) < 0) {
                ok = 0; why = "nb source lost on transfer";
            }
            it_xfer_release(d);
            exp_slot++;

        } else if (pick == 3u) {
            /* Occupied slot: recv fails fast; sender's NB attempt finds no
             * waiter; source kept; occupant untouched (I3, I4, I5). */
            uint32_t s = fz_slot_alloc();
            if (s == 0u) { ok = 0; why = "slot budget"; break; }
            if (it_invoke2(n, INV_CSPACE_MINT, IT_MINT_SELF((long)s), (long)RIGHT_WRITE) != 0) { ok = 0; why = "premint"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, s, 0, 0)) { ok = 0; why = "cmd"; break; }
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_ALREADY_EXISTS) {
                ok = 0; why = "occupied not rejected";
            }
            long d = fz_dup_xfer(n);
            if (ok && d >= 0) {
                struct iris_msg m;
                iris_msg_zero(&m);
                m.label           = 0xF4;
                m.cap = (uint32_t)d;
                m.cap_rights = RIGHT_WRITE;
                if (iris_msg_nb_send((long)g_fz_data_ep, &m) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
                if (ok && !it_slot_is_notif(d)) {

                    ok = 0; why = "cap consumed on fail";
                }
                it_slot_delete((uint32_t)d);
            }
            /* I3: the occupant is still exactly our pre-minted cap. */
            if (ok) {
                if (it_invoke1((long)s, INV_CAP_SAME_OBJECT, n) != 1) {
                    ok = 0; why = "occupant changed";
                }
            }

        } else if (pick == 4u) {
            /* Invalid (out-of-range) slot declaration → INVALID_ARG fail-
             * fast; the endpoint is untouched. */
            if (!fz_cmd(0, FZ_OP_RECV, 300u, 0, 0)) { ok = 0; why = "cmd"; break; }
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_INVALID_ARG) {
                ok = 0; why = "invalid slot";
            }
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = 0xF5;
            if (ok && iris_msg_nb_send((long)g_fz_data_ep, &m) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ep touched"; }

        } else if (pick == 5u) {
            /* Slot 0 = no destination.  Stage 4 retired handle
             * materialisation, so the receive SUCCEEDS and carries no
             * capability (I1).  The send is not an error — the failure is
             * closed at delivery, exactly like an occupied slot — and the
             * staged source is consumed either way, so the check that matters
             * is that no capability appeared anywhere. */
            long d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; why = "dup"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "cmd"; break; }
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0xF6;
            m.cap = (uint32_t)d;
            m.cap_rights = RIGHT_WRITE;
            if (iris_msg_send((long)g_fz_data_ep, &m) != 0) {
                ok = 0; why = "slotless send";
            }
            if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != 0 ||
                       g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP)) {
                ok = 0; why = "cap delivered without a slot";
            }
            /* I2/I3: nothing landed, so nothing was consumed — the source
             * slot still holds the capability the sender staged. */
            if (ok && !it_cdt_alive(d)) { ok = 0; why = "source consumed"; }
            it_slot_delete((uint32_t)d);
            exp_hand++;
            exp_hand++;

        } else {
            /* pick 6: staging without RIGHT_TRANSFER → ACCESS_DENIED; the
             * blocked receiver gains NOTHING and then gets a clean plain
             * message (I1, I2, I6); the degraded dup survives. */
            /* Phase S4 (Step 2): a SOURCE SLOT without RIGHT_TRANSFER. */
            long bad = it_xfer_slot_norights(n, IT_XFER_SLOT_D,
                                             (uint32_t)RIGHT_WRITE);
            if (bad < 0) { ok = 0; why = "bad slot"; break; }
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "cmd"; break; }
            it_settle(2);   /* worker re-blocks on the data ep */
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label           = 0xF7;
            m.cap = (uint32_t)bad;
            m.cap_rights = RIGHT_WRITE;
            if (iris_msg_nb_send((long)g_fz_data_ep, &m) !=
                (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no ACCESS_DENIED"; }
            if (ok && !it_slot_is_notif(bad)) {
                ok = 0; why = "bad slot consumed";
            }
            /* unblock the still-waiting receiver with a plain message */
            if (ok) {
                iris_msg_zero(&m);
                m.label = 0xF8;
                if (iris_msg_send((long)g_fz_data_ep, &m) != 0) {
                    ok = 0; why = "plain send";
                }
                if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
                if (ok && (g_fz_res[0] != 0 ||
                           g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP)) {
                    ok = 0; why = "ghost cap";
                }
            }
            it_slot_delete((uint32_t)bad);
        }
    }

    fz_workers_stop(1);
    it_close(&n_h);
    it_close(&ep2_h);
    it_close(&g_fz_data_ep);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +1 = the worker's KTcb handle (Ph96, A1.10 note). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* I18: directional counter deltas (>=: background services also count). */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    /* Stage 4: handle delivery is retired, so this counter is a structural
     * zero — exp_hand counts the deliveries that found NO destination, and
     * none of them may have landed in a handle table. */
    if (ok && after[IT_SI_HANDDEL] != before[IT_SI_HANDDEL]) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T107"); }
    else    { fz_note("T107", T107_SEED, it_n); it_fail("T107", why); }
}

void test_t108(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T108", "sched ext"); return; }
    g_fz_seed = T108_SEED;
    int ok = 1;
    const char *why = "close/cancel stress";
    uint32_t it_n = 0;

    long n = it_notify_create();
    handle_id_t n_h = (handle_id_t)n;
    if (n < 0) { it_fail("T108", "create"); return; }
    /* One reusable declared slot: every cancellation must leave it EMPTY,
     * so the same slot serves every pick-3 round (that IS the assert). */
    uint32_t rslot = fz_slot_alloc();
    if (rslot == 0u) { it_close(&n_h); it_fail("T108", "slot budget"); return; }
    if (!fz_workers_start(2)) { it_close(&n_h); it_fail("T108", "worker"); return; }

    for (it_n = 0; ok && it_n < T108_ROUNDS; it_n++) {
        long ep = it_ep_create();
        if (ep < 0) { ok = 0; why = "ep create"; break; }
        g_fz_data_ep = (handle_id_t)ep;
        uint32_t pick = fz_rand() % 5u;

        if (pick == 0u || pick == 1u) {
            /* One waiter with a staged cap: blocking EP_SEND (pick 0) or
             * EP_CALL (pick 1); close cancels it before any rendezvous. */
            long d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; why = "dup"; break; }
            int sent = (pick == 0u)
                ? fz_cmd(0, FZ_OP_SEND_CAP, (uint64_t)d, RIGHT_WRITE, 0x108)
                : fz_cmd(0, FZ_OP_CALL,     (uint64_t)d, RIGHT_WRITE, 0);
            if (!sent) { ok = 0; why = "cmd"; break; }
            it_settle(5);            /* waiter queues its staged cap */
            it_close(&g_fz_data_ep);
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_CLOSED) {
                ok = 0; why = "not CLOSED";
            }
            /* I5/I7: no delivery commit → the source cap survives. */
            if (ok && !it_slot_is_notif(d)) { ok = 0; why = "cap consumed"; }

            { handle_id_t dh = (handle_id_t)d; it_close(&dh); }

        } else if (pick == 2u) {
            /* TWO staged waiters (send + call) canceled by one close: every
             * staging ref released exactly once (I8). */
            long da = fz_dup_xfer(n), db = fz_dup_xfer(n);
            if (da < 0 || db < 0) { ok = 0; why = "dup2"; break; }
            if (!fz_cmd(0, FZ_OP_SEND_CAP, (uint64_t)da, RIGHT_WRITE, 0x208) ||
                !fz_cmd(1, FZ_OP_CALL,     (uint64_t)db, RIGHT_WRITE, 0)) {
                ok = 0; why = "cmd2"; break;
            }
            it_settle(5);            /* both queue staged caps */
            it_close(&g_fz_data_ep);
            if (!fz_wait(0) || !fz_wait(1)) { ok = 0; why = "worker hang"; }
            if (ok && (g_fz_res[0] != (long)IRIS_ERR_CLOSED ||
                       g_fz_res[1] != (long)IRIS_ERR_CLOSED)) {
                ok = 0; why = "not CLOSED x2";
            }
            if (ok && (!it_slot_is_notif(da) ||

                       !it_slot_is_notif(db))) {

                ok = 0; why = "cap consumed x2";
            }
            { handle_id_t dh = (handle_id_t)da; it_close(&dh); }
            { handle_id_t dh = (handle_id_t)db; it_close(&dh); }

        } else if (pick == 3u) {
            /* Receiver with a DECLARED slot canceled by close: wakes CLOSED,
             * gains nothing (I6), and the slot stays empty — the next pick-3
             * round re-declares the very same slot. */
            if (!fz_cmd(0, FZ_OP_RECV, rslot, 0, 0)) { ok = 0; why = "cmd"; break; }
            it_settle(5);            /* receiver blocks, slot declared */
            it_close(&g_fz_data_ep);
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_CLOSED) {
                ok = 0; why = "recv not CLOSED";
            }
            if (ok && g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                ok = 0; why = "ghost cap";
            }
            if (ok && it_invoke0((long)rslot, INV_CAP_IDENTIFY) >= 0) {
                ok = 0; why = "slot not empty";
            }

        } else {
            /* Legacy (slot 0) receiver canceled by close. */
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "cmd"; break; }
            it_settle(5);
            it_close(&g_fz_data_ep);
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_CLOSED) {
                ok = 0; why = "legacy not CLOSED";
            }
            if (ok && g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                ok = 0; why = "legacy ghost cap";
            }
        }
        it_close(&g_fz_data_ep);   /* no-op on the already-closed rounds */
    }

    /* Post-stress health: a fresh endpoint still rendezvouses normally. */
    if (ok) {
        long ep = it_ep_create();
        if (ep < 0) { ok = 0; why = "final ep"; }
        else {
            g_fz_data_ep = (handle_id_t)ep;
            if (!fz_cmd(0, FZ_OP_RECV, 0, 0, 0)) { ok = 0; why = "final cmd"; }
            if (ok) {
                it_settle(2);        /* receiver re-blocks on data ep */
                struct iris_msg m;
                iris_msg_zero(&m);
                m.label = 0x308;
                if (iris_msg_send((long)g_fz_data_ep, &m) != 0 ||
                    !fz_wait(0) || g_fz_res[0] != 0 ||
                    g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                    ok = 0; why = "ep not reusable";
                }
            }
            it_close(&g_fz_data_ep);
        }
    }

    fz_workers_stop(2);
    it_close(&n_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +2 = the two workers' KTcb handles (Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* No call ever rendezvoused → not one KReply minted (T104 rule). */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY]) {
        ok = 0; why = "ghost kreply";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T108"); }
    else    { fz_note("T108", T108_SEED, it_n); it_fail("T108", why); }
}

void test_t109(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T109", "sched ext"); return; }
    g_fz_seed = T109_SEED;
    int ok = 1;
    const char *why = "reply one-shot stress";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0, exp_reply = 0;

    long ep    = it_ep_create_slot();
    long n     = it_notify_create_slot();
    /* Stage 4: invoked as a CPtr; never materialised into a handle. */
    const long selfp = (it_invoke0((long)IRIS_CPTR_TEST_PROC, INV_CAP_IDENTIFY) >= 0)
                       ? (long)IRIS_CPTR_TEST_PROC : -1;
    handle_id_t n_h = (handle_id_t)n;
    g_fz_data_ep = (handle_id_t)ep;
    if (ep < 0 || n < 0 || selfp < 0) { it_fail("T109", "create"); return; }
    /* Occupied reply-slot fixture: pre-minted once, occupied forever. */
    uint32_t occ = fz_slot_alloc();
    if (occ == 0u ||
        it_invoke2(n, INV_CSPACE_MINT, IT_MINT_SELF((long)occ), (long)RIGHT_WRITE) != 0) {
        it_fail("T109", "occ fixture"); return;
    }
    if (!fz_workers_start(1)) { it_fail("T109", "worker"); return; }
    /* Phase S1: ONE reusable explicit reply object serves every rendezvous
     * round — free→staged→bound→free per call (S18 under stress). */
    if (it_reply_create_at(96) < 0) { it_fail("T109", "reply create"); return; }

    for (it_n = 0; ok && it_n < T109_ITERS; it_n++) {
        uint32_t pick = fz_rand() % 4u;

        if (pick == 3u) {
            /* Call declaring the OCCUPIED reply slot: fail-fast before any
             * send (I3/I4) — no message lands, no KReply is minted, and the
             * occupant is still exactly the fixture cap. */
            if (!fz_cmd(0, FZ_OP_CALL, 0, 0, occ)) { ok = 0; why = "cmd"; break; }
            if (!fz_wait(0)) { ok = 0; why = "worker hang"; }
            if (ok && g_fz_res[0] != (long)IRIS_ERR_ALREADY_EXISTS) {
                ok = 0; why = "occupied not rejected";
            }
            if (ok) {
                struct iris_msg m;
                iris_msg_zero(&m);
                if (iris_msg_nb_recv((long)g_fz_data_ep, &m) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ghost msg"; }
            }
            if (ok) {
                if (it_invoke1((long)occ, INV_CAP_SAME_OBJECT, n) != 1) {
                    ok = 0; why = "occupant changed";
                }
            }
            continue;
        }

        /* Rendezvous rounds: the worker calls, we serve. */
        uint32_t s = 0;
        if (pick == 1u) {
            s = fz_slot_alloc();
            if (s == 0u) { ok = 0; why = "slot budget"; break; }
        }
        if (!fz_cmd(0, FZ_OP_CALL, 0, 0, s)) { ok = 0; why = "cmd"; break; }

        struct iris_msg m;
        iris_msg_zero(&m);
        if ((m.reply = 96, iris_msg_recv((long)g_fz_data_ep, &m)) != 0 ||
            m.label != 0xF2ULL ||
            m.got_cap != 96u) {   /* Phase S1: our reply CPtr echoed */
            ok = 0; why = "recv call"; break;
        }
        handle_id_t reply_h = (handle_id_t)m.got_cap;
        exp_reply++;                       /* exactly one KReply per rendezvous */

        long d = -1;
        if (pick != 0u) {
            d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; why = "dup"; }
        }

        /* First reply: plain (pick 0) or carrying the dup (picks 1-2). */
        if (ok) {
            struct iris_msg rm;
            iris_msg_zero(&rm);
            rm.label = 0x5109;
            if (d >= 0) {
                rm.cap = (uint32_t)d;
                rm.cap_rights = RIGHT_WRITE;
            }
            if (iris_msg_reply((long)reply_h, &rm) != 0) {
                ok = 0; why = "first reply";
            }
        }
        if (ok && !fz_wait(0)) { ok = 0; why = "worker hang"; }
        if (ok && g_fz_res[0] != 0) { ok = 0; why = "caller err"; }

        if (ok && pick == 0u) {
            /* Plain reply delivers no cap. */
            if (g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) { ok = 0; why = "ghost cap"; }
        } else if (ok && pick == 1u) {
            /* I12: the reply cap landed at the declared slot, invocable. */
            if (g_fz_att[0] != s) { ok = 0; why = "slot landing"; }
            uint64_t bits = 0;
            if (ok && (it_invoke1((long)s, INV_NOTIFY_SIGNAL, 1) != 0 ||
                       it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
                       bits != 1u)) { ok = 0; why = "cptr dead"; }
            /* A-29: the sender keeps its copy (seL4's transfer is a COPY). */
            if (ok && it_invoke0(d, INV_CAP_IDENTIFY) < 0) {
                ok = 0; why = "source lost on reply transfer";
            }
            it_xfer_release(d);
            if (ok) { d = -1; exp_slot++; }
        } else if (ok) {
            /* I1 on the reply path: no declared slot, no capability. */
            if (g_fz_att[0] != (uint32_t)IRIS_MSG_NO_CAP) {
                ok = 0; why = "reply cap delivered without a slot";
            }
            /* I2/I3: the delivery found no destination, so the staged source
             * is NOT consumed — the sender keeps exactly what it had.  This is
             * the same shape as an occupied destination slot. */
            if (ok && !it_cdt_alive(d)) { ok = 0; why = "source consumed"; }
            it_slot_delete((uint32_t)d);
            if (ok) { d = -1; exp_hand++; }
        }

        /* SECOND reply — one-shot must hold (I9); when it carries a cap the
         * server keeps it (I10, the A1.10 T105 rule under stress). */
        if (ok) {
            long d2 = -1;
            if (pick == 0u) {
                d2 = fz_dup_xfer(n);
                if (d2 < 0) { ok = 0; why = "dup2"; }
            }
            if (ok) {
                struct iris_msg rm;
                iris_msg_zero(&rm);
                rm.label = 0xDEAD;
                if (d2 >= 0) {
                    rm.cap = (uint32_t)d2;
                    rm.cap_rights = RIGHT_WRITE;
                }
                if (iris_msg_reply((long)reply_h, &rm) !=
                    (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "not one-shot"; }
                if (ok && d2 >= 0 && !it_slot_is_notif(d2)) {
                    ok = 0; why = "second reply ate cap";
                }
            }
            if (d2 >= 0) it_slot_delete((uint32_t)d2);
        }
        if (d >= 0) it_slot_delete((uint32_t)d);
        it_close(&reply_h);
    }

    fz_workers_stop(1);
    it_close(&n_h);
    it_close(&g_fz_data_ep);
    it_slot_delete(96);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance; +1 = the worker's KTcb handle (Ph96). */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* Reply BINDINGS balance EXACTLY: one per rendezvous, zero per fail-fast
     * (Phase S1: the counter tracks bindings of the reusable reply object). */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY] + exp_reply) {
        ok = 0; why = "reply count";
    }
    /* I18: directional delivery deltas. */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    /* Stage 4: handle delivery is retired, so this counter is a structural
     * zero — exp_hand counts the deliveries that found NO destination, and
     * none of them may have landed in a handle table. */
    if (ok && after[IT_SI_HANDDEL] != before[IT_SI_HANDDEL]) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T109"); }
    else    { fz_note("T109", T109_SEED, it_n); it_fail("T109", why); }
}

void test_t110(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T110", "sched ext"); return; }
    g_fz_seed = T110_SEED;
    int ok = 1;
    const char *why = "svcmgr rslot stress";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_reply = 0, exp_log = 0;

    long ep    = it_ep_create_slot();
    long nf    = it_notify_create_slot();
    /* Stage 4: invoked as a CPtr; never materialised into a handle. */
    const long selfp = (it_invoke0((long)IRIS_CPTR_TEST_PROC, INV_CAP_IDENTIFY) >= 0)
                       ? (long)IRIS_CPTR_TEST_PROC : -1;
    handle_id_t ep_h = (handle_id_t)ep, nf_h = (handle_id_t)nf;
    if (ep < 0 || nf < 0 || selfp < 0) { it_fail("T110", "create"); return; }

    /* Fixtures: one slot that must stay EMPTY across every NOT_FOUND round
     * and one pre-minted slot that is occupied forever. */
    uint32_t nfslot = fz_slot_alloc();
    uint32_t occ    = fz_slot_alloc();
    if (nfslot == 0u || occ == 0u ||
        it_invoke2(nf, INV_CSPACE_MINT, IT_MINT_SELF((long)occ), (long)RIGHT_WRITE) != 0) {
        it_fail("T110", "fixtures"); return;
    }

    char name[5] = { 'f', 'z', '.', 'a', '\0' };
    long ids[3]  = { -1, -1, -1 };
    int  reg[3]  = { 0, 0, 0 };
    struct iris_msg msg;

    /* Registered-name lookup into a declared slot: invocable, then released.
     * It used to run slotless and assert a handle >= 1024; Stage 4 retired
     * that delivery, so the destination is now explicit. */
    #define T110_LEGACY_OK()                                                  \
        do {                                                                  \
            it_slot_delete((uint32_t)IT_LOOKUP_TMP);                          \
            if (it_lookup_name_slot(name, (uint32_t)IT_LOOKUP_TMP, &msg) != 0 ||\
                msg.label != IRIS_EP_REPLY_OK ||                              \
                msg.got_cap != (uint32_t)IT_LOOKUP_TMP) {             \
                ok = 0; why = "slot lookup";                                  \
            } else {                                                          \
                struct iris_msg p;                                             \
                iris_msg_zero(&p);                                         \
                p.label = 0x110;                                              \
                if (iris_msg_nb_send((long)msg.got_cap, &p)                \
                            != (long)IRIS_ERR_WOULD_BLOCK) {                  \
                    ok = 0; why = "legacy cap dead";                          \
                }                                                             \
                it_slot_delete((uint32_t)IT_LOOKUP_TMP);                      \
                exp_slot++;                                                   \
            }                                                                 \
            exp_reply++; exp_log++;                                           \
        } while (0)

    /* Unregistered-name lookup into the reusable slot: ERR + slot empty. */
    #define T110_NOTFOUND()                                                   \
        do {                                                                  \
            if (it_lookup_name_slot(name, nfslot, &msg) != 0 ||               \
                msg.label != IRIS_EP_REPLY_ERR ||                             \
                msg.words[0] != (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND) {     \
                ok = 0; why = "not-found lookup";                             \
            } else if (it_invoke0((long)nfslot, INV_CAP_IDENTIFY) >= 0) {      \
                ok = 0; why = "ghost cap";                                    \
            }                                                                 \
            exp_reply++; exp_log++;                                           \
        } while (0)

    for (it_n = 0; ok && it_n < T110_ITERS; it_n++) {
        uint32_t pick = fz_rand() % 6u;
        uint32_t i    = fz_rand() % 3u;
        name[3] = (char)('a' + i);

        if (pick == 0u) {
            /* Register, or re-register while live → BUSY (rejected cap is
             * closed by svcmgr — the final live-balance check proves it). */
            long r = it_register_ep(name, ep_h);
            exp_reply++;
            if (!reg[i]) {
                if (r < 0) { ok = 0; why = "register"; }
                else { ids[i] = r; reg[i] = 1; }
            } else if (r != -(long)(uint32_t)IRIS_ERR_BUSY) {
                ok = 0; why = "re-register not BUSY";
            }

        } else if (pick == 1u) {
            /* Slot lookup: fresh slot on success (occupied forever after —
             * the CPtr is the proof), reusable slot on NOT_FOUND. */
            if (reg[i]) {
                uint32_t s = fz_slot_alloc();
                if (s == 0u) { ok = 0; why = "slot budget"; break; }
                if (it_lookup_name_slot(name, s, &msg) != 0 ||
                    msg.label != IRIS_EP_REPLY_OK ||
                    msg.got_cap != s) { ok = 0; why = "slot lookup"; }
                exp_reply++; exp_log++;
                if (ok) {
                    struct iris_msg p;
                    iris_msg_zero(&p);
                    p.label = 0x110;
                    if (iris_msg_nb_send((long)s, &p) !=
                        (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "cptr dead"; }
                    exp_slot++;
                }
            } else {
                T110_NOTFOUND();
            }

        } else if (pick == 2u) {
            if (reg[i]) T110_LEGACY_OK(); else T110_NOTFOUND();

        } else if (pick == 3u) {
            /* Unregister removes authority: the very next lookup fails. */
            if (reg[i]) {
                if (it_unregister_id((uint32_t)ids[i]) != 0) {
                    ok = 0; why = "unregister";
                }
                exp_reply++;
                reg[i] = 0;
                if (ok) T110_NOTFOUND();
            } else {
                T110_NOTFOUND();
            }

        } else if (pick == 4u) {
            /* Occupied-slot lookup: fail-fast BEFORE any send (no KReply,
             * no reply counter tick), then legacy still works. */
            if (reg[i]) {
                if (it_lookup_name_slot(name, occ, &msg) !=
                    (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occ not rejected"; }
                if (ok) T110_LEGACY_OK();
            } else {
                T110_NOTFOUND();
            }

        } else {
            /* Full unregister → register cycle: the svcmgr pool slot is
             * freed and reused with no ghost of the previous generation. */
            if (reg[i]) {
                if (it_unregister_id((uint32_t)ids[i]) != 0) {
                    ok = 0; why = "cycle unreg";
                }
                exp_reply++;
                reg[i] = 0;
                if (ok) T110_NOTFOUND();
            }
            if (ok) {
                long r = it_register_ep(name, ep_h);
                exp_reply++;
                if (r < 0) { ok = 0; why = "cycle register"; }
                else { ids[i] = r; reg[i] = 1; }
            }
            if (ok) T110_LEGACY_OK();
        }
    }

    /* Teardown: unregister the survivors; every name must end NOT_FOUND. */
    for (uint32_t i = 0; i < 3u; i++) {
        name[3] = (char)('a' + i);
        if (ok && reg[i]) {
            if (it_unregister_id((uint32_t)ids[i]) != 0) { ok = 0; why = "final unreg"; }
            exp_reply++;
            reg[i] = 0;
        }
        if (ok) T110_NOTFOUND();
    }
    #undef T110_LEGACY_OK
    #undef T110_NOTFOUND

    it_close(&ep_h);
    it_close(&nf_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance — svcmgr consumed/closed every staged cap. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* One KReply per svcmgr rendezvous + one per served lookup (svcmgr_log
     * console EP_CALL), zero per fail-fast (exact). */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY] + exp_reply + exp_log) {
        it_serial_write("[IRIS][TEST] T110 reply delta=");
        it_log_num(after[IT_SI_REPLY] - before[IT_SI_REPLY]);
        it_serial_write(" exp=");
        it_log_num(exp_reply + exp_log);
        it_serial_write("\n");
        ok = 0; why = "reply count";
    }
    /* I18: directional delivery deltas (registers also move SLOTDEL —
     * svcmgr's own pool receive-slots — hence >=). */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    /* Stage 4: handle delivery is retired, so this counter is a structural
     * zero — exp_hand counts the deliveries that found NO destination, and
     * none of them may have landed in a handle table. */
    if (ok && after[IT_SI_HANDDEL] != before[IT_SI_HANDDEL]) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T110"); }
    else    { fz_note("T110", T110_SEED, it_n); it_fail("T110", why); }
}

static int t111_round(uint32_t kind, uint32_t *exp_slot, uint32_t *exp_hand,
                      const char **why) {
    int ok = 1;
    long ep = it_ep_create_slot();
    long n  = it_notify_create_slot();
    long e2 = (kind == 1u) ? it_ep_create_slot() : -1;
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
    handle_id_t e2_h = (kind == 1u) ? (handle_id_t)e2 : HANDLE_INVALID;
    handle_id_t proc_h = HANDLE_INVALID;

    if (ep < 0 || n < 0 || (kind == 1u && e2 < 0) ||
        lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; *why = "spawn"; }

    if (ok && it_lp_cmd_rslot(ep_h, (kind == 3u) ? 0u : T099_CHILD_SLOT) != 0) {
        ok = 0; *why = "cmd";
    }

    if (ok && (kind == 0u || kind == 3u)) {
        /* Deliver a notification cap.  kind 0 declares a slot: the child
         * invokes the cap through it (bits 1) and its exit code reports the
         * CPtr.  kind 3 declares NOTHING: Stage 4 retired handle
         * materialisation, so the child receives the message without the
         * capability, exits 0, and nobody signals — the parent must not
         * block waiting for a signal that cannot come. */
        if (it_lp_send_cap(ep_h, n) != 0) { ok = 0; *why = "send cap"; }
        if (ok && kind == 0u) {
            uint64_t bits = 0;
            if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
                bits != 1u) { ok = 0; *why = "x-proc signal"; }
        }
        if (ok) {
            long ec = it_lp_wait_exit(proc_h);
            if (kind == 0u && ec != (long)T099_CHILD_SLOT) {
                ok = 0; *why = "cptr landing";
            }
            if (kind == 3u && ec != 0) {
                ok = 0; *why = "cap delivered without a slot";
            }
        }
        /* The child is dead by now, so one tick is enough to prove nothing
         * ever signalled: there is no longer anybody who could. */
        if (ok && kind == 3u) {
            uint64_t bits = 0;
            if (it_wait_timeout( n, (long)(uintptr_t)&bits, 1)
                != (long)IRIS_ERR_TIMED_OUT) { ok = 0; *why = "phantom signal"; }
        }
        if (ok) { if (kind == 0u) (*exp_slot)++; else (*exp_hand)++; }

    } else if (ok && kind == 1u) {
        /* Deliver an endpoint cap into the child's declared slot: the exit
         * code proves the CSpace landing (the child's blind NOTIFY_SIGNAL
         * on it fails and is ignored by design). */
        if (it_lp_send_cap(ep_h, e2) != 0) { ok = 0; *why = "send ep cap"; }
        if (ok && it_lp_wait_exit(proc_h) != (long)T099_CHILD_SLOT) {
            ok = 0; *why = "ep cptr landing";
        }
        /* Child death released its CSpace ref; the parent's endpoint is
         * still alive and clean (no waiter, no corruption). */
        if (ok) {
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0x111;
            if (iris_msg_nb_send((long)e2_h, &p) !=
                (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; *why = "parent ep broken"; }
        }
        if (ok) (*exp_slot)++;

    } else if (ok) {
        /* Kill the child while it blocks with its slot declared: the wait
         * dies with it (no dead waiter) and a sender's delivery attempt
         * fails WITHOUT consuming the source cap. */
        it_settle(2);      /* child re-blocks, slot 40 declared */
        if (it_kill((long)proc_h) != 0) {
            ok = 0; *why = "kill";
        }
        if (ok && it_alive((long)proc_h) != 0) {
            ok = 0; *why = "still alive";
        }
        if (ok) {
            long d = fz_dup_xfer(n);
            if (d < 0) { ok = 0; *why = "dup"; }
            else {
                struct iris_msg m;
                iris_msg_zero(&m);
                m.label           = 0x211;
                m.cap = (uint32_t)d;
                m.cap_rights = RIGHT_WRITE;
                if (iris_msg_nb_send((long)ep_h, &m) !=
                    (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; *why = "dead waiter"; }
                if (ok && !it_slot_is_notif(d)) {

                    ok = 0; *why = "cap consumed";
                }
                handle_id_t dh = (handle_id_t)d;
                it_close(&dh);
            }
        }
    }

    if (!ok && proc_h != HANDLE_INVALID)
        (void)it_kill((long)proc_h);
    it_close(&proc_h);
    it_close(&e2_h);
    it_close(&n_h);
    it_close(&ep_h);
    return ok;
}

void test_t111(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T111", "sched ext"); return; }
    g_fz_seed = T111_SEED;
    int ok = 1;
    const char *why = "x-proc rslot fuzz";
    uint32_t it_n = 0;
    uint32_t exp_slot = 0, exp_hand = 0;

    /* Coverage-forced prefix: every scenario class exactly once. */
    for (it_n = 0; ok && it_n < 4u; it_n++)
        ok = t111_round(it_n, &exp_slot, &exp_hand, &why);
    /* PRNG tail. */
    for (; ok && it_n < 4u + T111_TAIL; it_n++)
        ok = t111_round(fz_rand() % 4u, &exp_slot, &exp_hand, &why);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    /* I16: exact balance — no helper threads here, so delta must be 0. */
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    /* I18: directional cross-process delivery deltas. */
    if (ok && after[IT_SI_SLOTDEL] < before[IT_SI_SLOTDEL] + exp_slot) {
        ok = 0; why = "slot count";
    }
    /* Stage 4: handle delivery is retired, so this counter is a structural
     * zero — exp_hand counts the deliveries that found NO destination, and
     * none of them may have landed in a handle table. */
    if (ok && after[IT_SI_HANDDEL] != before[IT_SI_HANDDEL]) {
        ok = 0; why = "hand count";
    }
    /* I17: T095 high-water rule. */
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T111"); }
    else    { fz_note("T111", T111_SEED, it_n); it_fail("T111", why); }
}

void test_t112(void) {
    uint32_t before[14], after[14];
    if (!it_sched_ext(before)) { it_fail("T112", "sched ext"); return; }
    int ok = 1;
    const char *why = "spawn/exit churn";
    uint32_t i = 0;

    for (i = 0; ok && i < T112_CYCLES; i++) {
        long ep = it_ep_create();
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
            ok = 0; why = "spawn";
            it_close(&ep_h);
            break;
        }
        /* Natural exit: a plain send releases the child's first recv. */
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = 0x112;
        if (iris_msg_send((long)ep_h, &m) != 0) {
            ok = 0; why = "send";
        }
        if (ok && it_lp_wait_exit(proc_h) != (long)LP_EXIT_MARKER) {
            ok = 0; why = "exit code";
        }
        it_close(&proc_h);
        it_close(&ep_h);
        /* No pause here: the immediate respawn IS the race being locked. */
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "leak"; }
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) { it_pass("T112"); }
    else {
        it_serial_write("[IRIS][TEST] T112 cycle=");
        it_log_num(i);
        it_serial_write("\n");
        it_fail("T112", why);
    }
}

/*
 * Drain the deferred-reap queue before a lifecycle baseline snapshot.
 *
 * A prior test's self-exited children release their last reference only when
 * the reaper runs, and the reaper runs inside a DISPATCH — so "wait for the
 * reaper" means "wait for the processor that owes me a dispatch to make one".
 *
 * It was 200 yields, and the comment said what made that work: "on
 * single-CPU".  There, every yield was a dispatch, and 200 of them drained any
 * backlog because the backlog could only be on the one core doing the
 * yielding.  With four processors a dying thread is reaped by ITS core, which
 * this thread's yields do not reach at all — they are 200 fast syscalls on a
 * different core, and they can all complete before the other core has taken a
 * single timer interrupt.
 *
 * So the wait is on the CONDITION instead of on a count of yields: the live
 * task total stops moving, sampled a tick apart so the other processors have
 * actually run in between.  Two consecutive equal samples end it; the cap ends
 * it if the number never settles, which turns "quiesced" back into "waited a
 * while" rather than hanging the suite.
 */
void it_quiesce_reaper(void) {
    /*
     * One real tick first, and it is the part the yields cannot replace: a
     * thread that died on another processor is not even IN the reap ring yet.
     * It gets there when its own core next dispatches, and a tick is what
     * guarantees that has happened everywhere.
     */
    it_settle(1);

    /* Then drain.  Yields are still how this thread hands its core to the
     * reaper, and the ring's DEPTH is the answer rather than a count of
     * attempts — on one processor it is empty after the first burst, which is
     * what the old two hundred yields were really doing. */
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t w6[5];
        for (int y = 0; y < 64; y++) (void)it_sys0(SYS_YIELD);
        if (!it_sched_ext6(w6)) return;        /* no gauge: the yields stand */
        if (w6[IT_S6_DEATHS_PENDING] == 0u) return;
        it_settle(1);
    }
}

/* ── Phase 16: lifecycle/process hardening (T113–T118) ───────────────────────
 * The A1.11 deferred-reap fix (task.awaiting_reap) closed the one real bug in
 * this area; T113–T118 LOCK the surviving lifecycle contracts so a future
 * regression fails loudly.  Instrumentation: the Phase 16 SCHED_INFO words —
 * live TASK count (it_task_live), live PROCESS count (IT_SI_PROCLIVE) and the
 * deferred-reap queue high-water (IT_SI_REAPHWM).  Because a killed/exited
 * child's KProcess stays live until the PARENT closes its proc handle, every
 * test closes all child handles BEFORE the final snapshot, so proc-live must
 * return exactly to baseline. */

/* ── T113: caller death mid-EP_CALL with a live reply cap ───────────────────
 * A child EP_CALLs the parent; the parent receives (minting the one-shot
 * KReply) and then KILLS the child while it is BLOCKED_REPLY.  The server's
 * reply must fail NOT_FOUND (the caller is gone), a second reply carrying an
 * attached cap must ALSO fail NOT_FOUND without consuming the server's cap,
 * no KReply is left dangling, no waiter survives, and both handle- and
 * process-live counts return to baseline (exactly one KReply was created).
 * Invariants: I5-I10, I15, I16. */
void test_t113(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T113", "sched ext"); return; }
    int ok = 1;
    const char *why = "caller death mid-call";

    long ep = it_ep_create();
    long n  = it_notify_create();      /* source cap for the 2nd reply */
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n;
    handle_id_t proc_h = HANDLE_INVALID;
    handle_id_t reply_h = HANDLE_INVALID;

    if (ep < 0 || n < 0 || lp_spawn_child(ep_h, &proc_h) < 0) {
        ok = 0; why = "spawn";
    }
    /* Drive the child into EP_CALL(cmd_ep); it queues as a caller. */
    if (ok && it_lp_cmd(ep_h, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }

    /* Serve the call: receive it and capture the reply cap.  The blocking
     * EP_RECV is the rendezvous — no timing needed. */
    if (ok && it_reply_create_at(97) < 0) { ok = 0; why = "reply create"; }
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        if ((m.reply = 97, iris_msg_recv((long)ep_h, &m)) != 0 ||
            m.label != 0x5CULL ||
            m.got_cap == (uint32_t)IRIS_MSG_NO_CAP) {
            ok = 0; why = "recv call";
        } else {
            reply_h = (handle_id_t)m.got_cap;
        }
    }

    /* Kill the caller while it is BLOCKED_REPLY: cancel clears r->caller. */
    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_alive((long)proc_h) != 0) {
        ok = 0; why = "still alive";
    }

    /* First reply (no cap) → the one-shot has no caller: NOT_FOUND. */
    if (ok) {
        struct iris_msg rm;
        iris_msg_zero(&rm);
        rm.label = 0x5A5A;
        if (iris_msg_reply((long)reply_h, &rm) !=
            (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "reply not NOT_FOUND"; }
    }

    /* Second reply WITH an attached cap → still NOT_FOUND, and the server's
     * source cap must survive un-consumed (A1.10 rule under caller death). */
    if (ok) {
        long d = it_xfer_dup( n, (uint32_t)(RIGHT_WRITE | RIGHT_TRANSFER));
        if (d < 0) { ok = 0; why = "dup"; }
        else {
            struct iris_msg rm;
            iris_msg_zero(&rm);
            rm.label           = 0xDEAD;
            rm.cap = (uint32_t)d;
            rm.cap_rights = RIGHT_WRITE;
            if (iris_msg_reply((long)reply_h, &rm) !=
                (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "2nd reply"; }
            if (ok && !it_slot_is_notif(d)) {

                ok = 0; why = "server cap consumed";
            }
            handle_id_t dh = (handle_id_t)d; it_close(&dh);
        }
    }

    it_slot_delete(97);  /* last reply cap → close(no caller) → destroy */
    it_close(&proc_h);    /* drop the parent's ref → child KProcess freed */
    it_close(&n_h);
    it_close(&ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    /* Exactly one KReply was created (at the rendezvous) and none leaked. */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY] + 1u) {
        ok = 0; why = "reply count";
    }
    if (ok) it_pass("T113"); else it_fail("T113", why);
}
void test_t114(void) {
    uint32_t before[14], after[14];
    uint32_t tl_before = 0, tl_after = 0;
    it_quiesce_reaper();
    if (!it_sched_ext(before) || !it_task_live(&tl_before)) {
        it_fail("T114", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "reap pressure";
    uint32_t i = 0;

    handle_id_t ep[T114_LIVE];
    handle_id_t pr[T114_LIVE];
    for (uint32_t s = 0; s < T114_LIVE; s++) { ep[s] = HANDLE_INVALID; pr[s] = HANDLE_INVALID; }

    /* Prime: four concurrent children, each parked in its first EP_RECV. */
    for (uint32_t s = 0; ok && s < T114_LIVE; s++) {
        long e = it_ep_create();
        if (e < 0) { ok = 0; why = "prime ep"; break; }
        ep[s] = (handle_id_t)e;
        if (lp_spawn_child(ep[s], &pr[s]) < 0) { ok = 0; why = "prime spawn"; }
    }

    /* Churn: tear one child down (alt exit/kill) and respawn it at once. */
    for (i = 0; ok && i < T114_CHURN; i++) {
        uint32_t s = i % T114_LIVE;
        if ((i & 1u) == 0u) {
            /* Natural exit: unblock the child's recv, confirm the marker. */
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = 0x114;
            if (iris_msg_send((long)ep[s], &m) != 0) {
                ok = 0; why = "exit send"; break;
            }
            if (it_lp_wait_exit(pr[s]) != (long)LP_EXIT_MARKER) {
                ok = 0; why = "exit code"; break;
            }
        } else {
            /* External kill while the child is blocked in its first recv. */
            if (it_kill((long)pr[s]) != 0) {
                ok = 0; why = "kill"; break;
            }
            if (it_alive((long)pr[s]) != 0) {
                ok = 0; why = "kill status"; break;
            }
        }
        it_close(&pr[s]);
        it_close(&ep[s]);

        /* Immediate respawn into the freed slot — the reuse-before-reap race. */
        long e = it_ep_create();
        if (e < 0) { ok = 0; why = "churn ep NO_MEMORY"; break; }
        ep[s] = (handle_id_t)e;
        if (lp_spawn_child(ep[s], &pr[s]) < 0) {
            ok = 0; why = "churn spawn NO_MEMORY"; break;
        }
    }

    /* Drain the survivors (natural exit). */
    for (uint32_t s = 0; s < T114_LIVE; s++) {
        if (pr[s] != HANDLE_INVALID) {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = 0x114;
            (void)iris_msg_send((long)ep[s], &m);
            (void)it_lp_wait_exit(pr[s]);
        }
        it_close(&pr[s]);
        it_close(&ep[s]);
    }

    it_quiesce_reaper();   /* let deferred reaps of self-exited children drain */
    if (ok && (!it_sched_ext(after) || !it_task_live(&tl_after))) {
        ok = 0; why = "sched ext 2";
    }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }
    /* The deferred reaper kept up: queue depth never neared its bound. */
    if (ok && after[IT_SI_REAPHWM] >= 8u) { ok = 0; why = "reap backlog"; }
    if (ok && after[IT_SI_GHWM] * 4u > after[IT_SI_MAX]) { ok = 0; why = "hwm"; }

    if (ok) it_pass("T114");
    else {
        it_serial_write("[IRIS][TEST] T114 iter=");
        it_log_num(i);
        it_serial_write("\n");
        it_fail("T114", why);
    }
}

/* ── T115: process death with active endpoint waiters ───────────────────────
 * A child is killed while blocked as an endpoint WAITER in each of the three
 * blocking states — EP_RECV (declared receive-slot), EP_SEND, EP_CALL.  In
 * every case the endpoint must retain no dead waiter (an NB probe from the
 * opposite direction returns WOULD_BLOCK), no KReply is minted for the
 * send/call cases (no rendezvous ever happened), and handle/process books
 * return to baseline.  Invariants: I6, I14, I15, I16. */
void test_t115(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T115", "sched ext"); return; }
    int ok = 1;
    const char *why = "death with waiters";

    /* kind 0 = EP_RECV waiter, 1 = EP_SEND waiter, 2 = EP_CALL waiter. */
    for (uint32_t kind = 0; ok && kind < 3u; kind++) {
        long ep = it_ep_create();
        handle_id_t ep_h = (handle_id_t)ep;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; break; }

        uint32_t cmd = (kind == 0u) ? LP_CMD_RSLOT_RECV
                     : (kind == 1u) ? LP_CMD_SEND_BLOCK
                                    : LP_CMD_CALL_BLOCK;
        if (kind == 0u) {
            if (it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "cmd recv"; }
        } else {
            if (it_lp_cmd(ep_h, cmd) != 0) { ok = 0; why = "cmd"; }
        }
        it_settle(3);      /* child reaches its blocking syscall */

        if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
        if (ok && it_alive((long)proc_h) != 0) {
            ok = 0; why = "still alive";
        }

        /* No dead waiter remains.  For the recv waiter, probe with NB_SEND;
         * for the send/call waiters, probe with NB_RECV. */
        if (ok) {
            struct iris_msg p;
            iris_msg_zero(&p);
            p.label = 0x115;
            long probe = (kind == 0u)
                ? iris_msg_nb_send((long)ep_h, &p)
                : iris_msg_nb_recv((long)ep_h, &p);
            if (probe != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "dead waiter"; }
        }

        it_close(&proc_h);
        it_close(&ep_h);
    }

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    /* No caller ever rendezvoused → not one KReply was minted. */
    if (ok && after[IT_SI_REPLY] != before[IT_SI_REPLY]) { ok = 0; why = "ghost kreply"; }
    if (ok) it_pass("T115"); else it_fail("T115", why);
}
void test_t116(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T116", "sched ext"); return; }
    int ok = 1;
    const char *why = "death with cspace/vmo";

    long ep  = it_ep_create_slot();   /* shared endpoint */
    long n   = it_notify_create_slot();      /* shared notification */
    long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);   /* shared VMO */
    handle_id_t ep_h = (handle_id_t)ep, n_h = (handle_id_t)n, vmo_h = (handle_id_t)vmo;
    handle_id_t cmd_ep_h = HANDLE_INVALID, proc_h = HANDLE_INVALID;

    /* Command endpoint keeps the child parked; the shared caps go into its
     * CSpace / handle table below. */
    long cep = it_ep_create_slot();
    cmd_ep_h = (handle_id_t)cep;
    if (ep < 0 || n < 0 || vmo < 0 || cep < 0 ||
        lp_spawn_child_cn(1u, cmd_ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; }

    /* Mint all three caps into the child's CSpace.  The VMO used to go
     * through SYS_VMO_SHARE into the child's handle table; what this test
     * measures — that killing a child holding live caps releases every one of
     * them — does not depend on which namespace held them, and the CSpace
     * form is the only one left. */
    if (ok && it_invoke2(ep, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), (long)T116_EP_SLOT), (long)RIGHT_WRITE) != 0) { ok = 0; why = "mint ep"; }
    if (ok && it_invoke2(n, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), (long)T116_N_SLOT), (long)RIGHT_WRITE) != 0) { ok = 0; why = "mint n"; }
    if (ok && it_invoke2(vmo, INV_CSPACE_MINT, IT_MINT_INTO(IT_CHILD_CN_CPTR(0), (long)T116_VMO_SLOT), (long)(RIGHT_READ | RIGHT_DUPLICATE)) != 0) {
        ok = 0; why = "share vmo";
    }

    /* Kill the child while it holds all three live caps. */
    if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
    if (ok && it_alive((long)proc_h) != 0) {
        ok = 0; why = "still alive";
    }

    /* The parent's objects survived the child's teardown. */
    if (ok) {
        struct iris_msg p;
        iris_msg_zero(&p);
        p.label = 0x116;
        if (iris_msg_nb_send((long)ep_h, &p) !=
            (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "endpoint dead"; }
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_SIGNAL, 4) != 0 ||
            it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 4u) { ok = 0; why = "notif dead"; }
    }
    if (ok && it_invoke0(vmo, INV_CAP_IDENTIFY) != (long)IRIS_HANDLE_TYPE_FRAME) {
        ok = 0; why = "frame dead";
    }

    it_close(&proc_h);
    it_close(&cmd_ep_h);
    it_close(&vmo_h);
    it_close(&n_h);
    it_close(&ep_h);

    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    if (ok) it_pass("T116"); else it_fail("T116", why);
}

/* ── T117: death-notification and watch consistency ─────────────────────────
 * Three children die by different routes — natural exit, external kill, and
 * blocked-then-killed (fault-in-IPC analogue) — each watched on its own
 * notification bit of a shared KNotification.  Every death must set its bit
 * exactly once (final mask == 0b111), STATUS must read dead and EXIT_CODE be
 * retrievable for all, a repeated KILL must be idempotent (0), and a watch
 * registered AFTER death must fire immediately (the already-dead emit path).
 * Invariants: I15 + the death-notification exactly-once contract. */
void test_t117(void) {
    uint32_t before[14], after[14];
    it_quiesce_reaper();
    if (!it_sched_ext(before)) { it_fail("T117", "sched ext"); return; }
    int ok = 1;
    const char *why = "death notify";

    long n = it_notify_create();
    handle_id_t n_h = (handle_id_t)n;
    handle_id_t ep[3]  = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    handle_id_t pr[3]  = { HANDLE_INVALID, HANDLE_INVALID, HANDLE_INVALID };
    if (n < 0) { it_fail("T117", "notif"); return; }

    for (uint32_t k = 0; ok && k < 3u; k++) {
        long e = it_ep_create();
        if (e < 0) { ok = 0; why = "ep"; break; }
        ep[k] = (handle_id_t)e;
        if (lp_spawn_child(ep[k], &pr[k]) < 0) { ok = 0; why = "spawn"; break; }
        /* Watch each child on its own bit of the shared notification. */
        if (it_invoke2(it_child_tcb((long)pr[k]), INV_TCB_WATCH, n, (long)(1u << k)) != 0) {
            ok = 0; why = "watch";
        }
    }

    /* child 0: natural exit; child 1: kill; child 2: block then kill. */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = 0x117;
        if (iris_msg_send((long)ep[0], &m) != 0) { ok = 0; why = "exit send"; }
    }
    if (ok && it_kill((long)pr[1]) != 0) { ok = 0; why = "kill1"; }
    if (ok && it_lp_cmd(ep[2], LP_CMD_SEND_BLOCK) != 0) { ok = 0; why = "cmd2"; }
    if (ok) {
        it_settle(3);
        if (it_kill((long)pr[2]) != 0) { ok = 0; why = "kill2"; }
    }

    /* Collect the three death bits (bounded waits; each death signals once). */
    if (ok) {
        uint64_t seen = 0;
        for (int iter = 0; iter < 8 && seen != 0x7u; iter++) {
            uint64_t bits = 0;
            if (it_wait_timeout( n, (long)(uintptr_t)&bits,
                        1000000000LL) == 0)
                seen |= bits;
        }
        if (seen != 0x7u) { ok = 0; why = "missing death bit"; }
    }

    /* STATUS dead for all; EXIT_CODE retrievable; child 0 kept its marker. */
    for (uint32_t k = 0; ok && k < 3u; k++) {
        if (it_alive((long)pr[k]) != 0) { ok = 0; why = "status alive"; }
        if (ok && it_invoke0(it_child_tcb((long)pr[k]), INV_TCB_EXIT_CODE) < 0) {
            ok = 0; why = "exit code";
        }
    }
    if (ok && it_invoke0(it_child_tcb((long)pr[0]), INV_TCB_EXIT_CODE) != (long)LP_EXIT_MARKER) {
        ok = 0; why = "exit0 marker";
    }

    /* Idempotent kill on an already-dead child → 0. */
    if (ok && it_kill((long)pr[1]) != 0) { ok = 0; why = "kill not idempotent"; }

    /* A watch armed AFTER death fires immediately (already-dead emit path). */
    if (ok) {
        long n2 = it_notify_create();
        handle_id_t n2_h = (handle_id_t)n2;
        if (n2 < 0) { ok = 0; why = "notif2"; }
        else {
            if (it_invoke2(it_child_tcb((long)pr[0]), INV_TCB_WATCH, n2, 0x20) != 0) {
                ok = 0; why = "late watch";
            }
            if (ok) {
                uint64_t bits = 0;
                if (it_wait_timeout( n2, (long)(uintptr_t)&bits,
                            1000000000LL) != 0 || bits != 0x20u) {
                    ok = 0; why = "late watch silent";
                }
            }
            it_close(&n2_h);
        }
    }

    for (uint32_t k = 0; k < 3u; k++) { it_close(&pr[k]); it_close(&ep[k]); }
    it_close(&n_h);

    it_quiesce_reaper();
    if (ok && !it_sched_ext(after)) { ok = 0; why = "sched ext 2"; }
    if (ok && after[IT_SI_LIVE] != before[IT_SI_LIVE]) { ok = 0; why = "handle leak"; }
    if (ok && after[IT_SI_PROCLIVE] != before[IT_SI_PROCLIVE]) {
        ok = 0; why = "proc leak";
    }
    if (ok) it_pass("T117"); else it_fail("T117", why);
}
static uint8_t g_t118_stk[4096];
static void t118_thread(void) {
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void test_t118(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t pl_before = 0, pl_after = 0;
    uint32_t w[14];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext(w)) { it_fail("T118", "sched ext"); return; }
    pl_before = w[IT_SI_PROCLIVE];
    int ok = 1;
    const char *why = "live count churn";
    uint32_t i = 0;

    for (i = 0; ok && i < T118_ROUNDS; i++) {
        /* (a) process self-exit */
        {
            long e = it_ep_create();
            handle_id_t e_h = (handle_id_t)e;
            handle_id_t p_h = HANDLE_INVALID;
            if (e < 0 || lp_spawn_child(e_h, &p_h) < 0) { ok = 0; why = "spawn exit"; }
            else {
                struct iris_msg m;
                iris_msg_zero(&m);
                m.label = 0x118;
                (void)iris_msg_send((long)e_h, &m);
                (void)it_lp_wait_exit(p_h);
            }
            it_close(&p_h);
            it_close(&e_h);
        }
        /* (b) process external-kill */
        if (ok) {
            long e = it_ep_create();
            handle_id_t e_h = (handle_id_t)e;
            handle_id_t p_h = HANDLE_INVALID;
            if (e < 0 || lp_spawn_child(e_h, &p_h) < 0) { ok = 0; why = "spawn kill"; }
            else {
                it_settle(1);
                (void)it_kill((long)p_h);
            }
            it_close(&p_h);
            it_close(&e_h);
        }
        /* (c) in-process thread self-exit (task slot reaped; KTcb persists). */
        if (ok) {
            uint64_t entry = (uint64_t)(uintptr_t)t118_thread;
            uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t118_stk + sizeof(g_t118_stk))) & ~0xFULL;
            if (it_thread_create(entry, rsp, 0) < 0) {
                ok = 0; why = "thread create";
            }
            /* let the thread run to exit and be reaped before the next round */
            it_settle(1);
        }
    }

    /* Give the deferred reaper time to drain the final departures. */
    it_quiesce_reaper();

    if (ok && (!it_task_live(&tl_after) || !it_sched_ext(w))) { ok = 0; why = "sched ext 2"; }
    pl_after = w[IT_SI_PROCLIVE];

    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }
    if (ok && pl_after != pl_before) { ok = 0; why = "proc-live drift"; }
    if (ok && w[IT_SI_REAPHWM] >= 8u) { ok = 0; why = "reap backlog"; }

    if (ok) it_pass("T118");
    else {
        it_serial_write("[IRIS][TEST] T118 round=");
        it_log_num(i);
        it_serial_write(" tl b/a=");
        it_log_num(tl_before);
        it_serial_write("/");
        it_log_num(tl_after);
        it_serial_write("\n");
        it_fail("T118", why);
    }
}
uint8_t           g_sh_stk[SH_NWORK][8192];
volatile int      g_sh_done[SH_NWORK];
volatile uint32_t g_sh_prog[SH_NWORK];   /* per-worker progress counter */
handle_id_t       g_sh_ep = HANDLE_INVALID; /* shared block/release ep   */
volatile uint32_t g_sh_mode;             /* selects the worker script    */
volatile uint32_t g_sh_iters;            /* yields per worker (churn/spin)*/
handle_id_t       g_sh_sc = HANDLE_INVALID; /* SC to bind (SH_MODE_SC)    */

static void sh_worker(uint32_t idx) {
    uint32_t mode  = g_sh_mode;
    uint32_t iters = g_sh_iters;

    if (mode == SH_MODE_SPIN) {
        for (uint32_t k = 0; k < iters; k++) {
            it_sys0(SYS_YIELD);
            g_sh_prog[idx] = k + 1u;
        }
    } else if (mode == SH_MODE_CHURN) {
        for (uint32_t k = 0; k < iters; k++) {
            it_sys0(SYS_YIELD);
            g_sh_prog[idx] = k + 1u;
        }
        struct iris_msg m;
        iris_msg_zero(&m);
        (void)iris_msg_recv((long)g_sh_ep, &m); /* block until released */
    } else if (mode == SH_MODE_SC) {
        (void)it_invoke0((long)g_sh_sc, INV_SC_SET_ON_CALLER);      /* bind → task ref */
        for (uint32_t k = 0; k < 6u; k++) { it_sys0(SYS_YIELD); g_sh_prog[idx] = k + 1u; }
    } else { /* SH_MODE_YIELD_BLOCK */
        for (uint32_t k = 0; k < 6u; k++) it_sys0(SYS_YIELD);
        struct iris_msg m;
        iris_msg_zero(&m);
        (void)iris_msg_recv((long)g_sh_ep, &m); /* block until released */
        for (uint32_t k = 0; k < 6u; k++) it_sys0(SYS_YIELD);
        g_sh_prog[idx] = 1u;
    }

    g_sh_done[idx] = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void sh_worker0(void) { sh_worker(0); }
static void sh_worker1(void) { sh_worker(1); }
static void sh_worker2(void) { sh_worker(2); }
static void sh_worker3(void) { sh_worker(3); }
void (*const g_sh_entries[SH_NWORK])(void) = {
    sh_worker0, sh_worker1, sh_worker2, sh_worker3
};

/* Start n workers (n ≤ SH_NWORK).  Returns 1 on success (all created). */
int sh_start(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { g_sh_done[i] = 0; g_sh_prog[i] = 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t entry = (uint64_t)(uintptr_t)g_sh_entries[i];
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_sh_stk[i] + sizeof(g_sh_stk[i]))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) return 0;
    }
    return 1;
}

/* Bounded wait until all n workers set their done flag. */
int sh_wait_all(uint32_t n) {
    for (int i = 0; i < 20000; i++) {
        int all = 1;
        for (uint32_t w = 0; w < n; w++) if (!g_sh_done[w]) { all = 0; break; }
        if (all) return 1;
        it_sys0(SYS_YIELD);
    }
    return 0;
}

/* Bounded wait until all n workers reach at least `target` progress. */
static int sh_wait_prog(uint32_t n, uint32_t target) {
    for (int i = 0; i < 20000; i++) {
        int all = 1;
        for (uint32_t w = 0; w < n; w++) if (g_sh_prog[w] < target) { all = 0; break; }
        if (all) return 1;
        it_sys0(SYS_YIELD);
    }
    return 0;
}

/* Release n workers blocked on g_sh_ep, one rendezvous send each. */
static int sh_release(uint32_t n) {
    for (uint32_t w = 0; w < n; w++) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = 0x1719;
        if (iris_msg_send((long)g_sh_ep, &m) != 0) return 0;
    }
    return 1;
}
void test_t119(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t e0[14], e1[14];
    uint32_t s2b[4], s2a[4];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext(e0) || !it_sched_ext2(s2b)) {
        it_fail("T119", "sched ext"); return;
    }
    int ok = 1;
    const char *why = "state churn";
    uint32_t i = 0;

    for (i = 0; ok && i < T119_ROUNDS; i++) {
        /* (a) in-process worker threads: block on a shared endpoint, wake by
         * rendezvous, then self-exit. */
        g_sh_mode = SH_MODE_YIELD_BLOCK;
        g_sh_iters = 0u;
        long ep = it_ep_create();
        if (ep < 0) { ok = 0; why = "ep create"; break; }
        g_sh_ep = (handle_id_t)ep;

        if (!sh_start(SH_NWORK)) { ok = 0; why = "thread create"; }
        /* let workers reach EP_RECV */
        if (ok) it_settle(1);
        if (ok && !sh_release(SH_NWORK)) { ok = 0; why = "release"; }
        if (ok && !sh_wait_all(SH_NWORK)) { ok = 0; why = "worker stuck"; }
        it_close(&g_sh_ep);

        /* (b) lifecycle_probe child externally killed while alive. */
        if (ok) {
            long ce = it_ep_create();
            handle_id_t ce_h = (handle_id_t)ce;
            handle_id_t p_h = HANDLE_INVALID;
            if (ce < 0 || lp_spawn_child(ce_h, &p_h) < 0) { ok = 0; why = "spawn"; }
            else {
                it_settle(1);
                (void)it_kill((long)p_h);
                if (it_alive((long)p_h) != 0) { ok = 0; why = "kill"; }
            }
            it_close(&p_h);
            it_close(&ce_h);
        }
        it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && (!it_task_live(&tl_after) || !it_sched_ext(e1) || !it_sched_ext2(s2a))) {
        ok = 0; why = "sched ext 2";
    }
    if (ok && tl_after != tl_before)                       { ok = 0; why = "task-live drift"; }
    if (ok && e1[IT_SI_PROCLIVE] != e0[IT_SI_PROCLIVE])    { ok = 0; why = "proc-live drift"; }
    if (ok && e1[IT_SI_REAPHWM] >= 8u)                     { ok = 0; why = "reap backlog"; }
    if (ok && s2a[IT_S2_SCLIVE] != s2b[IT_S2_SCLIVE])      { ok = 0; why = "sc-live drift"; }
    if (ok && s2a[IT_S2_YIELD] <= s2b[IT_S2_YIELD])        { ok = 0; why = "no yield progress"; }

    if (ok) it_pass("T119");
    else {
        it_serial_write("[IRIS][TEST] T119 round=");
        it_log_num(i);
        it_serial_write(" tl b/a=");
        it_log_num(tl_before);
        it_serial_write("/");
        it_log_num(tl_after);
        it_serial_write("\n");
        it_fail("T119", why);
    }
}
void test_t120(void) {
    uint32_t tl_before = 0, tl_after = 0;
    uint32_t s2b[4], s2a[4];
    it_quiesce_reaper();
    if (!it_task_live(&tl_before) || !it_sched_ext2(s2b)) { it_fail("T120", "sched ext"); return; }
    int ok = 1;
    const char *why = "run-queue churn";

    g_sh_mode  = SH_MODE_CHURN;
    g_sh_iters = T120_ITERS;
    long ep = it_ep_create_slot();
    if (ep < 0) { it_fail("T120", "ep create"); return; }
    g_sh_ep = (handle_id_t)ep;

    if (!sh_start(SH_NWORK)) { ok = 0; why = "thread create"; }

    /* Wait until every worker finished its full yield loop and parked in
     * EP_RECV — proves none was lost mid-churn. */
    if (ok && !sh_wait_prog(SH_NWORK, T120_ITERS)) { ok = 0; why = "worker lost"; }

    /* Each worker must have advanced EXACTLY T120_ITERS (no more, no less). */
    for (uint32_t w = 0; ok && w < SH_NWORK; w++)
        if (g_sh_prog[w] != T120_ITERS) { ok = 0; why = "progress mismatch"; }

    if (ok && !sh_release(SH_NWORK))  { ok = 0; why = "release"; }
    if (ok && !sh_wait_all(SH_NWORK)) { ok = 0; why = "worker stuck"; }
    it_close(&g_sh_ep);
    it_quiesce_reaper();

    if (ok && (!it_task_live(&tl_after) || !it_sched_ext2(s2a))) { ok = 0; why = "sched ext 2"; }
    /* yield counter advanced by at least the work we forced. */
    if (ok && (s2a[IT_S2_YIELD] - s2b[IT_S2_YIELD]) < SH_NWORK * T120_ITERS) {
        ok = 0; why = "yield accounting";
    }
    /* run queue actually held several concurrent runnable tasks, bounded. */
    if (ok && (s2a[IT_S2_RQHWM] < 2u || s2a[IT_S2_RQHWM] > TASK_MAX_HINT)) {
        ok = 0; why = "rq hwm implausible";
    }
    /* S4 guard trips stay bounded under pure-yield churn (no wakeup races). */
    if (ok && (s2a[IT_S2_DUPENQ] - s2b[IT_S2_DUPENQ]) > 64u) {
        ok = 0; why = "duplicate enqueue storm";
    }
    if (ok && tl_after != tl_before) { ok = 0; why = "task-live drift"; }

    if (ok) it_pass("T120");
    else {
        it_serial_write("[IRIS][TEST] T120 rqhwm=");
        it_log_num(s2a[IT_S2_RQHWM]);
        it_serial_write(" dup=");
        it_log_num(s2a[IT_S2_DUPENQ] - s2b[IT_S2_DUPENQ]);
        it_serial_write("\n");
        it_fail("T120", why);
    }
}

/* ── T121: IPC blocking scheduler invariants ────────────────────────────────
 * A worker thread is blocked in each of the three endpoint states — EP_RECV,
 * EP_SEND, EP_CALL — and then the endpoint's last handle is CLOSED out from
 * under it.  Each waiter must wake with exactly IRIS_ERR_CLOSED and leave the
 * scheduler with no residue.  Then a lifecycle_probe child is KILLED while
 * blocked as an EP_RECV waiter and the endpoint must retain no dead waiter (an
 * NB_SEND probe returns WOULD_BLOCK).  After all of it: task-live and
 * process-live return to baseline, no ghost KReply was minted (no rendezvous
 * ever happened), and the reaper drained.
 * Invariants: S2, S13, S14, S15. */
volatile long g_t121_res[3];
/* Dedicated single-shot workers, one per blocking endpoint state.  Each blocks
 * on g_sh_ep, records its wake-up result, then self-exits.  The recv worker
 * uses the legacy (slotless) path; the receive-slot path is exercised by the
 * kill-child leg below (it_lp_cmd_rslot), so T121 covers both. */
static void t121_recv(void) {
    struct iris_msg m; iris_msg_zero(&m);
    g_t121_res[0] = iris_msg_recv((long)g_sh_ep, &m);
    g_sh_done[0] = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void t121_send(void) {
    struct iris_msg m; iris_msg_zero(&m); m.label = 0x121;
    g_t121_res[1] = iris_msg_send((long)g_sh_ep, &m);
    g_sh_done[0] = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
static void t121_call(void) {
    struct iris_msg m; iris_msg_zero(&m); m.label = 0x121;
    g_t121_res[2] = iris_msg_call((long)g_sh_ep, &m);
    g_sh_done[0] = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void (*const g_t121_entries[3])(void) = { t121_recv, t121_send, t121_call };
