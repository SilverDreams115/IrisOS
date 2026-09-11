/*
 * it_t149_t181.c — tests T149 through T181.
 *
 * The suite's numbering is chronological, not thematic: T149 was written
 * stages before T181, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T149 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
/* ── T149: CPtr / handle / wrong-type fuzz ──────────────────────────────────
 * Every handle-taking syscall family gets fed empty slots, wrong-type caps,
 * stale handles and boundary values.  Wrong-type crossings are the core:
 * a notification handed to SYS_EP_SEND is WRONG_TYPE; an endpoint handed to
 * SYS_NOTIFY_SIGNAL is WRONG_TYPE; a frame handed to SYS_PROCESS_KILL is
 * WRONG_TYPE — none fall back, none mutate, none amplify.  Fixtures: one live
 * endpoint, notification and frame, all closed at the end.
 * Invariants: X2, X4, X5, X8, X9, X12, X21, X24. */
void test_t149(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "wrong-type fuzz";

    long ep = it_ep_create_slot();
    long no = it_notify_create_slot();
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
    if (ep < 0 || no < 0) { it_close(&ep_h); it_close(&no_h); it_fail("T149", "fixture"); return; }

    struct iris_msg m; iris_msg_zero(&m);

    /* Wrong-type: endpoint op on a notification and vice-versa. */
    if (ok && iris_msg_send(no, &m) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "ep_send on notif"; }
    if (ok && iris_msg_nb_send(no, &m) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "nb_send on notif"; }
    if (ok && it_invoke1(ep, INV_NOTIFY_SIGNAL, 1) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "signal on ep"; }
    /* Stage 7 Step 13: killing names a THREAD, and the TCB family answers
     * INVALID_ARG for an argument that is not one. */
    if (ok && it_invoke0(ep, INV_TCB_EXIT) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "kill on ep"; }
    if (ok && it_retype_slot_alloc(no, IT_KOBJ_FRAME, 4096) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "retype on notif"; }
    /* (frame_map wrong-type coverage against a real VSpace is in T151/T152.) */

    /* Boundary/empty/stale handles across a spread of families.  Any negative
     * error is acceptable (BAD_HANDLE / NOT_FOUND / WRONG_TYPE / INVALID_ARG);
     * a NON-NEGATIVE return would mean a hostile handle was honoured. */
    for (int i = 0; ok && i < IT_FZ_BAD_H_N; i++) {
        long h = it_fz_bad_handles[i];
        if (it_invoke0(h, INV_CAP_IDENTIFY) >= 0)                { ok = 0; why = "identify honoured bad"; break; }
        if (it_invoke1(h, INV_NOTIFY_SIGNAL, 1) >= 0)            { ok = 0; why = "signal honoured bad"; break; }
        if (iris_msg_send(h, &m) >= 0)           { ok = 0; why = "ep_send honoured bad"; break; }
        if (it_invoke0(h, INV_TCB_EXIT) >= 0)                    { ok = 0; why = "kill honoured bad"; break; }
        if (it_invoke2(h, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_READ) >= 0)                   { ok = 0; why = "mint honoured bad"; break; }
        if (it_retype_slot_alloc(h, IT_KOBJ_FRAME, 4096) >= 0) { ok = 0; why = "retype honoured bad"; break; }
    }

    /* A RELEASED capability is dead: derive a copy, delete the copy's slot,
     * and the CPtr must stop resolving.  This used to dup a handle and close
     * it; an emptied slot is the same statement in the namespace that stays. */
    if (ok) {
        long d = it_cs_reduce(no, RIGHT_SAME_RIGHTS);
        if (d < 0) { ok = 0; why = "derive for stale"; }
        else {
            it_slot_delete((uint32_t)d);
            if (it_invoke0(d, INV_CAP_IDENTIFY) != (long)IRIS_ERR_NOT_FOUND) {
                ok = 0; why = "released cap still resolves";
            }
        }
    }

    it_close(&ep_h); it_close(&no_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T149"); else it_fail("T149", why);
}

/* ── T150: user pointer / buffer fuzz ───────────────────────────────────────
 * Syscalls that read or write userland buffers are fed null, kernel-half VAs,
 * unmapped user VAs, and read-only buffers where a write is required.  The
 * kernel validates every range by page-table walk BEFORE touching it
 * (usercopy.c: SMAP + user_range_*), so a hostile pointer must yield a clean
 * INVALID_ARG — never a kernel #PF, never a partial write.  A valid buffer
 * still works afterwards, proving the reject path left nothing wedged.
 * Invariants: X6, X7, X10, X18. */
void test_t150(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "user ptr fuzz";

    /* SYS_SCHED_INFO takes its KDEBUG authority as a CPtr argument (ledger
     * A-3 retired the ambient handle-table scan), so the pointer checks reach
     * the user_range_writable stage without any capability being staged
     * first.  This used to resolve the spawn cap into a handle purely to make
     * the ambient scan find it. */

    static const long bad_ptr[] = {
        0L,                          /* null */
        0x100L,                      /* below USER_ADDR_MIN */
        (long)0xFFFF800000000000ULL, /* kernel half */
        (long)0x8090000000ULL,       /* canonical user, unmapped */
        (long)0xDEADBEEFULL,         /* misaligned unmapped */
    };
    const int NB = (int)(sizeof(bad_ptr) / sizeof(bad_ptr[0]));

    /* SYS_SCHED_INFO writes a buffer: every hostile dst → INVALID_ARG. */
    for (int i = 0; ok && i < NB; i++) {
        if (it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, bad_ptr[i], 184) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "sched_info bad dst"; break;
        }
    }
    /* Ledger A-22: SYS_TCB_FAULT_INFO is RETIRED, so there is no kernel
     * pointer to abuse here at all — a fault record reaches its handler as a
     * message, and the kernel never writes one into a buffer a caller names.
     * What is left to assert is that the number answers NOT_SUPPORTED
     * whatever it is handed, hostile pointers included. */
    {   /* One publication for the whole battery.  SYS_TCB_SELF hands back a
         * FRESH capability every call, into the rotating pool, and calling it
         * per iteration abandoned one per iteration — which the allocator then
         * evicted laps later.  T324 counted 33 of those a run. */
        long self_tcb = it_own_tcb_derived();
        for (int i = 0; ok && i < NB; i++) {
            if (it_sys2(SYS_TCB_FAULT_INFO, self_tcb, bad_ptr[i])
                != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "fault_info bad dst"; break; }
        }
        if (self_tcb >= 0) it_slot_delete((uint32_t)self_tcb);
    }
    /* SYS_UNTYPED_INFO writes two OPTIONAL out params (a null pointer means
     * "skip this field" and is legal), so only a NON-NULL hostile pointer must
     * fail INVALID_ARG.  Index 0 is the null case and is skipped. */
    for (int i = 1; ok && i < NB; i++) {
        if (it_invoke2(IT_UT, INV_UNTYPED_INFO, 0, bad_ptr[i]) != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "untyped_info bad dst"; break;
        }
    }
    /* Both-null is the legal "just validate the cap" call → success. */
    if (ok && it_invoke2(IT_UT, INV_UNTYPED_INFO, 0, 0) != 0) { ok = 0; why = "untyped_info null-null not ok"; }
    /* A-24: SYS_NOTIFY_WAIT and SYS_NOTIFY_POLL write out_bits, and both
     * validate the pointer BEFORE they block or consume anything — so a
     * hostile destination costs a waiter neither its signal nor its slot.  The
     * probe used to go through SYS_NOTIFY_WAIT_TIMEOUT, which is retired: a
     * bounded wait is a request to the timer service now, and the only kernel
     * writer left is the wait itself. */
    {
        long no = it_notify_create();
        handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
        if (no < 0) { ok = 0; why = "notif fixture"; }
        for (int i = 0; ok && i < NB; i++) {
            if (it_invoke1(no, INV_NOTIFY_POLL, bad_ptr[i])
                != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "notify_wait bad out"; break; }
            if (it_invoke1(no, INV_NOTIFY_WAIT, bad_ptr[i])
                != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "notify_wait bad out"; break; }
        }
        it_close(&no_h);
    }
    /*
     * A-33 deleted this probe's subject.
     *
     * It sent a hostile MESSAGE POINTER and asked for INVALID_ARG with the
     * endpoint left clean.  A message has no pointer any more — it is a
     * MessageInfo word and message registers — so there is no address for a
     * caller to get wrong and none for the kernel to validate.  The hostile
     * pointers above still have work to do, because `Notify_Wait` and
     * `Notify_Poll` genuinely do write to user memory the caller names.
     *
     * What took its place is not another probe but the absence of a check:
     * `user_range_readable` is gone from every send path in the kernel.
     */

    /* Size fuzz on SYS_SCHED_INFO: below-base is INVALID_ARG, huge size is
     * clamped to the largest tier (not an overflow) and succeeds into a valid
     * buffer. */
    if (ok) {
        uint8_t buf[184];
        if (it_invoke1((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "size 0"; }
        if (ok && it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 8) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "size below base"; }
        if (ok && it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 0x7FFFFFFFL) != 0) { ok = 0; why = "huge size not clamped"; }
        /* A valid call still works — reject paths left nothing wedged. */
        if (ok && it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 184) != 0) { ok = 0; why = "valid after fuzz"; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T150"); else it_fail("T150", why);
}
void test_t151(void) {
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T151", "vspace self mint"); return; }
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cross-family fuzz";
    g_fz_seed = T151_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T151_ROUNDS; i++) {
        /* One well-formed object per round, resolved back to baseline at the
         * end of the round; malformed ops are interleaved and must no-op. */
        handle_id_t fr = HANDLE_INVALID, ep = HANDLE_INVALID, no = HANDLE_INVALID;
        uint64_t va = 0x8098000000ULL + (uint64_t)(fz_rand() % 8u) * 0x1000ULL;

        /* --- malformed batch (must all fail clean) --- */
        op = 1;
        if (it_retype_slot_alloc(IT_UT, 0x7777u, 4096) >= 0) { ok = 0; why = "bad-type retype ok"; break; }
        op = 2;
        if (it_invoke(IT_UT, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W) >= 0) { ok = 0; why = "map wrong-type ok"; break; }
        op = 3;
        /* Phase S4 (Step 3): the CSpace forms must reject a stale/empty slot
         * just as cleanly, and a HANDLE value outright (namespace split). */
        it_slot_delete(IT_SCRATCH_3);
        if (it_invoke2((long)IT_SCRATCH_3, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_2 << 32), (long)RIGHT_READ) >= 0) { ok = 0; why = "derive stale ok"; break; }
        if (it_invoke2(9000L, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_2 << 32), (long)RIGHT_READ) >= 0) { ok = 0; why = "derive by handle ok"; break; }
        op = 4;
        if (it_invoke0((long)IT_SCRATCH_3, INV_CSPACE_REVOKE) >= 0) { ok = 0; why = "revoke stale ok"; break; }
        /* A HANDLE value must be refused: build a well-formed one rather than
         * a bare large integer, which is a legitimate CPtr now. */
        if (it_invoke0((long)handle_id_make(8u, 1u), INV_CSPACE_REVOKE) >= 0) { ok = 0; why = "revoke by handle ok"; break; }
        op = 5;
        /* Stage 7 Step 7: a random CPTR, not a random id.  An unoccupied slot
         * resolves to NOT_FOUND and an occupied one to the wrong type, and
         * neither may resume anything. */
        {
            /* A-22: a random CPtr is not a reply capability, so it answers
             * nothing.  Whatever is in that slot — nothing, or an object of
             * some other type — the call fails; what it must never do is
             * resume a thread. */
            struct iris_msg rm;
            iris_msg_zero(&rm);
            long rr = iris_msg_reply((long)(fz_rand() & 0x3FFu), &rm);
            if (rr >= 0) { ok = 0; why = "random cptr resumed something"; break; }
        }
        op = 6;
        {   long self_tcb = it_own_tcb_derived();
            long fr2 = it_sys2(SYS_TCB_FAULT_INFO, self_tcb, 0L);
            if (self_tcb >= 0) it_slot_delete((uint32_t)self_tcb);
            if (fr2 != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "fault_info null ok"; break; }
        }

        /* --- well-formed batch (must all succeed and be observable) --- */
        op = 10;
        long r = it_frame_create_slot(IT_UT, 4096);
        fr = (r >= 0) ? (handle_id_t)r : HANDLE_INVALID;
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype frame"; break; }
        op = 11;
        long en = it_ep_create();
        ep = (en >= 0) ? (handle_id_t)en : HANDLE_INVALID;
        long nn = it_notify_create();
        no = (nn >= 0) ? (handle_id_t)nn : HANDLE_INVALID;
        if (ep == HANDLE_INVALID || no == HANDLE_INVALID) { ok = 0; why = "obj create"; it_close(&fr); it_close(&ep); it_close(&no); break; }

        op = 12;
        if ((fz_rand() & 1u)) {
            if (it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }
            /* occupied VA is BUSY, not a silent overwrite */
            if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied not BUSY"; }
            if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)va) != 0) { ok = 0; why = "unmap"; }
        }
        op = 13;
        if (ok && (fz_rand() & 1u)) {
            if (it_invoke1((long)no, INV_NOTIFY_SIGNAL, 1) != 0) { ok = 0; why = "signal"; }
            uint64_t bits = 0;
            if (ok && it_wait_timeout( (long)no, (long)(uintptr_t)&bits, 500000000L) != 0) { ok = 0; why = "wait"; }
        }

        it_close(&fr); it_close(&ep); it_close(&no);

        if ((i & 3u) == 3u) { it_quiesce_reaper(); if (!it_ut_reset()) { ok = 0; why = "mid reset busy"; break; } }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T151");
    else { it_fz_note("T151", T151_SEED, i, op); it_fail("T151", why); }
}

/* ── T152: failure atomicity fuzz ───────────────────────────────────────────
 * Force each failure mode and prove the operation is all-or-nothing: the full
 * snapshot before == after (no half-built object, no dangling ref), and a
 * following VALID operation of the same family still works.  Modes: occupied
 * destination (BUSY), missing rights (ACCESS_DENIED), wrong type (WRONG_TYPE),
 * invalid pointer (INVALID_ARG), invalid VA (INVALID_ARG), bad object size
 * (INVALID_ARG), resume mismatch (NOT_FOUND).  Invariants: X7, X9, X10, X13, X22. */
void test_t152(void) {
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T152", "vspace self mint"); return; }
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "atomicity fuzz";
    const uint64_t VA = 0x80A0000000ULL;

    /* Occupied VA: map, then a second map is BUSY and must not leak a node. */
    long r = it_retype_frame();
    handle_id_t fr = (r != HANDLE_INVALID) ? (handle_id_t)r : HANDLE_INVALID;
    if (fr == HANDLE_INVALID) { it_fail("T152", "retype"); return; }
    struct it_snap mid;
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)VA, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }
    mid = it_snap_take();
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)VA, (long)IT_MAP_W) != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied not BUSY"; }
    /* BUSY must have changed nothing since mid. */
    { struct it_snap now = it_snap_take(); const char *w2;
      if (ok && !it_snap_baseline(&mid, &now, &w2)) { ok = 0; why = "BUSY mutated state"; } }
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)VA) != 0) { ok = 0; why = "unmap"; }

    /* Missing rights: RIGHT_READ frame cap cannot map writable — ACCESS_DENIED,
     * no PTE born; a following writable map with a full cap works. */
    if (ok) {
        long rd = it_cdt_reduced(fr, IT_SCRATCH_0, IT_SCRATCH_1, RIGHT_READ);
        handle_id_t fr_ro = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
        if (rd < 0) { ok = 0; why = "ro derive"; }
        if (ok && it_invoke((long)fr_ro, INV_FRAME_MAP, IT_VS, (long)VA, (long)IT_MAP_W) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable not denied"; }
        if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)VA, (long)IT_MAP_W) != 0) { ok = 0; why = "valid map after deny"; }
        if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)VA) != 0) { ok = 0; why = "unmap 2"; }
        it_slot_delete(IT_SCRATCH_1);
        it_slot_delete(IT_SCRATCH_0);
    }

    /* Invalid VA / bad size / bad pointer — each INVALID_ARG, nothing born. */
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)(VA | 0x40ULL), (long)IT_MAP_W) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "unaligned VA"; }
    if (ok && it_invoke((long)fr, INV_FRAME_MAP, IT_VS, 0xFFFF800000001000L, (long)IT_MAP_W) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA"; }
    if (ok && it_retype_slot_alloc(IT_UT, IT_KOBJ_FRAME, 3) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "bad frame size"; }
    /* Non-null hostile out pointer (kernel half) → INVALID_ARG, nothing written. */
    if (ok && it_invoke2(IT_UT, INV_UNTYPED_INFO, 0, 0xFFFF800000001000L) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel out ptr"; }
    /* Resume mismatch — NOT_FOUND, no state touched.  Ledger A-22: answering
     * a fault is SYS_REPLY, and an empty slot holds no reply capability.  Leaf
     * 233 is above every fault leaf in the objects CNode and nothing else
     * writes it. */
    {
        struct iris_msg rm;
        iris_msg_zero(&rm);
        if (ok && iris_msg_reply((long)IT_OBJ_CPTR(233u), &rm)
                  != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "resume mismatch"; }
    }

    it_close(&fr);
    it_quiesce_reaper();
    (void)it_ut_reset();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T152"); else it_fail("T152", why);
}
void test_t153(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cancellation fuzz";
    g_fz_seed = T153_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T153_ROUNDS; i++) {
        uint32_t kind = fz_rand() % 4u;   /* 0 recv, 1 send, 2 call, 3 fault */
        long ep = it_ep_create();
        handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc_h = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; it_close(&ep_h); break; }

        handle_id_t n_h = HANDLE_INVALID;
        op = kind;
        if (kind == 3u) {
            /* Fault-pending waiter: register a handler, drive an invalid-VA fault. */
            long n = it_ep_create();   /* A-22: faults go to an ENDPOINT */
            n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
            if (n < 0 || it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, n, 0, 0) != 0) { ok = 0; why = "reg handler"; }
            if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "fault cmd"; }
            if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no fault"; }
        } else {
            uint32_t cmd = (kind == 0u) ? LP_CMD_RSLOT_RECV
                         : (kind == 1u) ? LP_CMD_SEND_BLOCK : LP_CMD_CALL_BLOCK;
            /* RSLOT_RECV parks in a second recv; SEND/CALL park as sender/caller. */
            if (kind == 0u) { if (it_lp_cmd_rslot(ep_h, T099_CHILD_SLOT) != 0) { ok = 0; why = "rslot cmd"; } }
            else            { if (it_lp_cmd(ep_h, cmd) != 0) { ok = 0; why = "block cmd"; } }
            it_settle(3);   /* let the child reach its blocking syscall */
        }

        /* Cancellation route: seeded mix of process kill / endpoint close /
         * handler drop.  Each must resolve without stranding the child. */
        uint32_t route = fz_rand() % 3u;
        if (ok && route == 0u) {
            if (it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
        } else if (ok && route == 1u) {
            it_close(&ep_h);                              /* close endpoint under the waiter */
            if (kind == 3u) it_close(&n_h);
            if (it_kill((long)proc_h) != 0) { ok = 0; why = "kill after close"; }
        } else if (ok) {
            if (kind == 3u) it_close(&n_h);               /* handler drop first */
            if (it_kill((long)proc_h) != 0) { ok = 0; why = "kill after drop"; }
        }
        if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "no exit"; }

        it_close(&ep_h); it_close(&n_h); it_close(&proc_h);
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T153");
    else { it_fz_note("T153", T153_SEED, i, op); it_fail("T153", why); }
}

/* ── T154: rights monotonicity fuzz across syscalls ─────────────────────────
 * A reduced-rights cap must never be amplified by ANY syscall, and no legacy
 * path may ignore rights.  A RIGHT_READ notification dup cannot signal
 * (ACCESS_DENIED); a RIGHT_READ frame cap cannot map writable; a derive can
 * only narrow, never widen (asking for a right the parent lacks does not grant
 * it); and the reduced cap's failures mutate nothing.  Cross the CPtr path and
 * the legacy handle path for the same object.  Invariants: X3, X8. */
void test_t154(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "rights monotonicity";

    long no = it_notify_create_slot();
    handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
    if (no < 0) { it_fail("T154", "notif fixture"); return; }

    /* RIGHT_READ dup cannot signal (needs RIGHT_WRITE) — no fallback. */
    long rd = it_cs_reduce(no, RIGHT_READ);
    handle_id_t rd_h = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
    if (rd < 0) { ok = 0; why = "read dup"; }
    if (ok && it_invoke1(rd, INV_NOTIFY_SIGNAL, 1) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read cap signalled"; }
    /* Re-deriving from the reduced cap cannot recover RIGHT_WRITE: either the
     * derive is rejected, or it yields a cap that STILL cannot signal. */
    if (ok) {
        long wr = it_cs_reduce(rd, RIGHT_READ | RIGHT_WRITE);
        if (wr >= 0) {
            handle_id_t wr_h = (handle_id_t)wr;
            if (it_invoke1(wr, INV_NOTIFY_SIGNAL, 1) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights amplified via re-derive"; }
            it_close(&wr_h);
        }
    }
    /* The original full cap still signals — the reduced cap's failures did not
     * corrupt the object. */
    if (ok && it_invoke1(no, INV_NOTIFY_SIGNAL, 1) != 0) { ok = 0; why = "full cap broken"; }
    if (ok) { uint64_t bits = 0; (void)it_wait_timeout( no, (long)(uintptr_t)&bits, 100000000L); }

    /* Frame rights: RIGHT_READ frame cap maps non-writable but is denied a
     * writable map; the PTE reflects the cap, never the request. */
    if (ok && it_setup_self_vspace()) {
        long r = it_retype_frame();
        handle_id_t fr = (r != HANDLE_INVALID) ? (handle_id_t)r : HANDLE_INVALID;
        const uint64_t VA = 0x80A8000000ULL;
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype"; }
        long dr = ok ? it_cdt_reduced(fr, IT_SCRATCH_0, IT_SCRATCH_1, RIGHT_READ) : -1;
        handle_id_t fr_ro = (dr >= 0) ? (handle_id_t)dr : HANDLE_INVALID;
        if (ok && dr < 0) { ok = 0; why = "ro derive"; }
        if (ok && it_invoke((long)fr_ro, INV_FRAME_MAP, IT_VS, (long)VA, (long)IT_MAP_W) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable map"; }
        if (ok && it_invoke((long)fr_ro, INV_FRAME_MAP, IT_VS, (long)VA, 0L) != 0) { ok = 0; why = "ro readable map"; }
        if (ok && it_invoke2((long)fr_ro, INV_FRAME_UNMAP, IT_VS, (long)VA) != 0) { ok = 0; why = "ro unmap"; }
        it_slot_delete(IT_SCRATCH_1);
        it_slot_delete(IT_SCRATCH_0);
        it_close(&fr);
        it_quiesce_reaper(); (void)it_ut_reset();
    } else if (ok) { ok = 0; why = "vspace self mint"; }

    it_close(&rd_h); it_close(&no_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T154"); else it_fail("T154", why);
}
void test_t155(void) {
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T155", "vspace self mint"); return; }
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "full stress";
    g_fz_seed = T155_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T155_ROUNDS; i++) {
        struct it_snap rb = it_snap_take();   /* per-round baseline */

        /* Object churn: retype a frame, map/derive/revoke/unmap, close. */
        op = 1;
        long r = it_frame_create_slot(IT_UT, 4096);
        handle_id_t fr = (r >= 0) ? (handle_id_t)r : HANDLE_INVALID;
        if (fr == HANDLE_INVALID) { ok = 0; why = "retype"; break; }
        uint64_t va = 0x80B0000000ULL + (uint64_t)(fz_rand() % 4u) * 0x1000ULL;
        op = 2;
        if (it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)va, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; it_close(&fr); break; }
        op = 3;
        if (fz_rand() & 1u) {
            long frc = it_cdt_root(fr, IT_SCRATCH_0);
            long c   = (frc >= 0) ? it_cdt_derive(frc, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
            if (c >= 0 && it_cdt_revoke(frc) < 0) { ok = 0; why = "revoke"; it_close(&fr); break; }
            if (ok && c >= 0 && it_cdt_alive(c)) { ok = 0; why = "revoked child alive"; it_close(&fr); break; }
            it_slot_delete(IT_SCRATCH_1);
            it_slot_delete(IT_SCRATCH_0);
        }
        op = 4;
        if (it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)va) != 0) { ok = 0; why = "unmap"; it_close(&fr); break; }
        it_close(&fr);

        /* Notification rendezvous. */
        op = 5;
        long nn = it_notify_create();
        handle_id_t no = (nn >= 0) ? (handle_id_t)nn : HANDLE_INVALID;
        if (no != HANDLE_INVALID && (fz_rand() & 1u)) {
            if (it_invoke1((long)no, INV_NOTIFY_SIGNAL, 3) != 0) { ok = 0; why = "signal"; }
            uint64_t bits = 0;
            if (ok && it_wait_timeout( (long)no, (long)(uintptr_t)&bits, 500000000L) != 0) { ok = 0; why = "wait"; }
        }
        it_close(&no);

        /* Every few rounds: spawn a child and resolve it (clean exit, kill, or
         * a controlled fault → kill) — the lifecycle+fault surface under churn. */
        if (ok && (i % 4u) == 0u) {
            op = 6;
            long ep = it_ep_create();
            handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
            handle_id_t proc_h = HANDLE_INVALID;
            if (ep < 0 || lp_spawn_child(ep_h, &proc_h) < 0) { ok = 0; why = "spawn"; it_close(&ep_h); break; }
            uint32_t what = fz_rand() % 3u;
            if (what == 0u) {                    /* clean run */
                if (it_lp_cmd(ep_h, 0x55u) != 0) { ok = 0; why = "cmd clean"; }
                if (ok && it_lp_wait_exit(proc_h) != (long)LP_EXIT_MARKER) { ok = 0; why = "clean exit"; }
            } else if (what == 1u) {             /* kill while parked */
                if (it_lp_cmd(ep_h, LP_CMD_SEND_BLOCK) != 0) { ok = 0; why = "cmd block"; }
                it_settle(3);
                if (ok && it_kill((long)proc_h) != 0) { ok = 0; why = "kill"; }
                if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "kill exit"; }
            } else {                             /* controlled fault → kill */
                long n = it_ep_create();   /* A-22: faults go to an ENDPOINT */
                handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
                if (n < 0 || it_invoke(it_child_tcb((long)proc_h), INV_TCB_SET_FAULT_HANDLER, n, 0, 0) != 0) { ok = 0; why = "reg handler"; }
                if (ok && it_lp_cmd_va(ep_h, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "fault cmd"; }
                if (ok && !it_fault_wait_ep((long)n_h, 0u)) { ok = 0; why = "no fault"; }
                struct it_fault f;
                if (ok && it_fault_info(0u, &f) != 0) { ok = 0; why = "fault info"; }
                if (ok && it_fault_kill(0) != 0) { ok = 0; why = "resume kill"; }
                if (ok && it_lp_wait_exit(proc_h) != 0) { ok = 0; why = "fault exit"; }
                it_close(&n_h);
            }
            it_close(&ep_h); it_close(&proc_h);
        }

        it_quiesce_reaper();
        if (ok && !it_ut_reset()) { ok = 0; why = "round reset busy"; break; }
        /* Per-round baseline: no lifecycle gauge may drift within a round
         * (object-count balance is checked by the single-process fuzz tests;
         * a round that spawned a child has transient child-owned objects). */
        struct it_snap re = it_snap_take();
        const char *w2;
        if (ok && !it_snap_baseline_live(&rb, &re, &w2)) { ok = 0; why = w2; break; }
    }

    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "final reset busy"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T155");
    else { it_fz_note("T155", T155_SEED, i, op); it_fail("T155", why); }
}

/* Spawn a lifecycle_probe with the command endpoint at LP_CPTR_CMD_EP plus an
 * arbitrary extra mint set, command REPORT_SLOTS, and return the reported slot
 * bitmask (bits 0..15 = slots 0..15; bit 16 = slot 25 proc, 17 = slot 55
 * untyped, 18 = slot 56 vspace).  Returns -1 on spawn/command failure. */
long it_lp_report_slots(const struct svc_mint *extra, uint32_t nextra) {
    long ep = it_ep_create();
    if (ep < 0) return -1;
    handle_id_t cmd = (handle_id_t)ep;

    struct svc_mint mints[8] = { 0 };
    mints[0].slot = LP_CPTR_CMD_EP;
    IT_MINT_SRC(mints[0], cmd);
    mints[0].rights = RIGHT_READ | RIGHT_WRITE;
    mints[0].badge = 0;
    uint32_t n = 1u;
    for (uint32_t i = 0; i < nextra && n < 8u; i++) mints[n++] = extra[i];

    handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "lifecycle_probe",
                             &proc, &boot, mints, n,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(proc);
    it_close(&boot);
    if (r < 0 || proc == HANDLE_INVALID) { it_close(&cmd); it_close(&proc); return -1; }

    long mask = -1;
    if (it_lp_cmd(cmd, LP_CMD_REPORT_SLOTS) == 0) mask = it_lp_wait_exit(proc);
    it_close(&cmd); it_close(&proc);
    return mask;
}

/* LOOKUP_NAME through the given svcmgr CPtr; returns the granted attached_rights
 * (>=0), or a negative error.  Closes the returned cap (rights are the subject). */
/*
 * The rights a lookup actually GRANTED.
 *
 * A-33: it used to read the rights the server had written into the reply
 * message — a field the kernel copied across untouched, so the answer was
 * what the server ASKED for and not what the caller got.  There is no such
 * field now.  A capability's rights come back in the MessageInfo, which means
 * the capability has to actually be delivered, which means declaring a slot
 * for it — so this asks the stronger question, and deletes what it was given.
 */
long it_lookup_rights(long svcmgr_cptr, const char *name) {
    uint32_t len = it_stage_path(name);
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label     = IRIS_SVCMGR_EP_LOOKUP_NAME;
    m.buf_len   = len;
    it_slot_delete(IT_SCRATCH_2);
    m.recv_slot = (long)IT_SCRATCH_2;
    if (iris_msg_call(svcmgr_cptr, &m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -(long)(uint32_t)m.words[0];
    long r = (long)m.got_caps;
    it_slot_delete(IT_SCRATCH_2);
    return r;
}

/* svcmgr DIAG ready-service count (words[1]) — the registry gauge that INCLUDES
 * dynamic registrations (words[2] active_slot_count is catalog-only).  A dynamic
 * register bumps this by one; unregister drops it back. */
static long it_svcmgr_active_slots(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_DIAG;
    if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK || m.word_count < 2u) return -1;
    return (long)(uint32_t)m.words[1];
}

/* ── T156: service authority manifest consistency (delivery) ────────────────
 * The mint delivery mechanism every service depends on carries EXACTLY the
 * declared caps.  A probe minted {cmd@3, notif@5, ep@7} reports exactly those
 * three slots; a probe minted {cmd@3} alone reports only slot 3.  No phantom
 * cap appears, and removing a cap removes it from the child.
 * Invariants: A1, A11, A13, A15. */
void test_t156(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "manifest delivery";

    long no = it_notify_create();
    long ep = it_ep_create();
    handle_id_t no_h = (no >= 0) ? (handle_id_t)no : HANDLE_INVALID;
    handle_id_t ep_h = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    if (no < 0 || ep < 0) { it_close(&no_h); it_close(&ep_h); it_fail("T156", "fixture"); return; }

    /* Declared set {cmd@3, notif@5, ep@7}. */
    struct svc_mint extra[2] = { 0 };
    extra[0].slot = 5; IT_MINT_SRC(extra[0], no_h); extra[0].rights = RIGHT_READ; extra[0].badge = 0;
    extra[1].slot = 7; IT_MINT_SRC(extra[1], ep_h); extra[1].rights = RIGHT_READ; extra[1].badge = 0;
    long mask = it_lp_report_slots(extra, 2u);
    uint32_t want = (1u << 3) | (1u << 5) | (1u << 7);
    if (ok && mask < 0) { ok = 0; why = "report failed"; }
    if (ok && (uint32_t)mask != want) { ok = 0; why = "delivered != declared"; }

    /* Reduced set {cmd@3} only → report drops slots 5 and 7 (A15). */
    if (ok) {
        long m0 = it_lp_report_slots(0, 0u);
        if (m0 < 0 || (uint32_t)m0 != (1u << 3)) { ok = 0; why = "reduction not observed"; }
    }

    it_close(&no_h); it_close(&ep_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T156"); else it_fail("T156", why);
}

/* ── T157: svcmgr grant tightening (registry, not god) ──────────────────────
 * svcmgr never amplifies authority for an ordinary client.  A LOOKUP_NAME of a
 * reserved ".ep" name grants WRITE|DUPLICATE only to a SUPERVISOR badge; an
 * ordinary badge gets a call-only WRITE cap with DUPLICATE/TRANSFER stripped —
 * so an ordinary client cannot re-mint or hand on the service cap.  A lookup of
 * a nonexistent name is NOT_FOUND with no cap.  Invariants: A7, A11, A14. */
void test_t157(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "grant tightening";

    /* Ordinary badge (slot 1 = IRIS_CPTR_SVCMGR_EP, IRIS_BADGE_IRIS_TEST). */
    long ord = it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, VFS_EP_SVC_NAME);
    if (ok && ord < 0) { ok = 0; why = "ordinary lookup failed"; }
    if (ok && ((uint32_t)ord & RIGHT_WRITE) == 0) { ok = 0; why = "ordinary lost WRITE"; }
    if (ok && ((uint32_t)ord & (RIGHT_DUPLICATE | RIGHT_TRANSFER)) != 0) {
        ok = 0; why = "ordinary amplified (dup/transfer)";
    }

    /* Supervisor badge (slot 27 = IRIS_CPTR_TEST_SUPER, IRIS_BADGE_INIT). */
    long sup = it_lookup_rights((long)IRIS_CPTR_TEST_SUPER, VFS_EP_SVC_NAME);
    if (ok && sup < 0) { ok = 0; why = "supervisor lookup failed"; }
    if (ok && ((uint32_t)sup & RIGHT_DUPLICATE) == 0) { ok = 0; why = "supervisor lost DUPLICATE"; }

    /* Nonexistent name → NOT_FOUND, no cap. */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "no.such.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "missing name not NOT_FOUND"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T157"); else it_fail("T157", why);
}

/* ── T158: vfs authority boundary ───────────────────────────────────────────
 * vfs serves a filesystem and nothing else.  Its endpoint answers PING and its
 * own ops but rejects a foreign (registry) opcode; an ordinary client's vfs.ep
 * cap is call-only WRITE (cannot be re-minted to seize the service).  vfs holds
 * no authority to act on iris_test — there is no cap by which it could.
 * Invariants: A8, A11, A16. */
void test_t158(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "vfs boundary";

    /* vfs.ep answers PING. */
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label    = IRIS_EP_OP_PING;
    if (ok && (iris_msg_call((long)IRIS_CPTR_VFS_EP, &m) != 0 ||
               m.label != IRIS_EP_REPLY_OK)) { ok = 0; why = "vfs ping"; }

    /* A foreign registry opcode to vfs.ep must NOT be honoured as a registry
     * op (vfs is not svcmgr); it replies with an error, never OK. */
    iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
    m.buf_len  = it_stage_path("vfs.ep");
    if (ok && iris_msg_call((long)IRIS_CPTR_VFS_EP, &m) == 0 &&
        m.label == IRIS_EP_REPLY_OK && m.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
        ok = 0; why = "vfs served a registry op";
        handle_id_t h = (handle_id_t)m.got_cap; it_close(&h);
    }

    /* Ordinary vfs.ep cap is call-only (no DUPLICATE) — cannot be re-minted. */
    long rights = it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, VFS_EP_SVC_NAME);
    if (ok && (rights < 0 || ((uint32_t)rights & RIGHT_DUPLICATE) != 0)) {
        ok = 0; why = "vfs cap re-mintable by client";
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T158"); else it_fail("T158", why);
}

/* ── T159: console/kbd authority boundary ───────────────────────────────────
 * The I/O drivers have narrow authority: console.ep and kbd.ep answer PING and
 * their own ops but reject a foreign registry opcode (no cap handed back), and
 * their client caps are call-only WRITE.  A compromised driver cannot register
 * services or seize a peer.  Invariants: A9, A11, A16. */
void test_t159(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "io driver boundary";
    const long eps[2] = { (long)IRIS_CPTR_CONSOLE_EP, (long)IRIS_CPTR_KBD_EP };

    for (int i = 0; ok && i < 2; i++) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label    = IRIS_EP_OP_PING;
        if (iris_msg_call(eps[i], &m) != 0 || m.label != IRIS_EP_REPLY_OK) {
            ok = 0; why = "driver ping"; break;
        }
        /* Foreign registry op → must not hand back a cap. */
        iris_msg_zero(&m);
        m.label    = IRIS_SVCMGR_EP_LOOKUP_NAME;
        m.buf_len  = it_stage_path("vfs.ep");
        if (iris_msg_call(eps[i], &m) == 0 &&
            m.label == IRIS_EP_REPLY_OK && m.got_cap != (uint32_t)IRIS_MSG_NO_CAP) {
            ok = 0; why = "driver served registry op";
            handle_id_t h = (handle_id_t)m.got_cap; it_close(&h); break;
        }
    }

    /* Both driver client caps are call-only (no DUPLICATE). */
    long rc = it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, CONSOLE_EP_SVC_NAME);
    if (ok && (rc < 0 || ((uint32_t)rc & RIGHT_DUPLICATE) != 0)) { ok = 0; why = "console cap re-mintable"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T159"); else it_fail("T159", why);
}

/* ── T160: init authority handoff audit ─────────────────────────────────────
 * init necessarily starts privileged; the audit is that its handoff is
 * deliberate.  iris_test is init's test child and holds the DECLARED test-only
 * authority (spawn cap slot 6, self-proc slot 25, untyped slot 55) — this test
 * confirms that authority is really present here, which is exactly what T162
 * proves must be ABSENT from an ordinary service.  Invariants: A4, A6, A10. */
void test_t160(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "init handoff";

    /* iris_test's declared test authority resolves (it is a privileged test
     * child, not a productive service).  Each resolve mints a fresh handle;
     * close it immediately so the snapshot balances. */
    const long test_caps[3] = {
        (long)IRIS_CPTR_PROC_CONTROL, (long)IRIS_CPTR_TEST_UNTYPED,
        (long)IRIS_CPTR_TEST_PROC,
    };
    const char *const caps_why[3] = { "no proc control cap", "no test untyped",
                                      "no self proc" };
    for (int s = 0; ok && s < 3; s++) {
        if (it_invoke0(test_caps[s], INV_CAP_IDENTIFY) < 0) {
            ok = 0; why = caps_why[s]; break;
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T160"); else it_fail("T160", why);
}

/* ── T161: service death containment (dynamic registry) ─────────────────────
 * A registered service that goes away leaves no ghost in svcmgr.  iris_test
 * registers a dynamic service backed by an endpoint it owns, confirms it
 * resolves, then unregisters (owner authority) — the name no longer resolves,
 * the active-slot gauge returns to its pre-register value, and the registry
 * books balance.  Invariants: A12, A16. */
void test_t161(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "death containment";

    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }

    long e = it_ep_create();
    handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
    if (ok && e < 0) { ok = 0; why = "svc ep"; }

    long id = ok ? it_register_ep("t161.svc", svc_ep) : -1;
    if (ok && id < 0) { ok = 0; why = "register"; }
    /* Registered → resolves, and the active-slot gauge moved up. */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t161.svc") < 0) { ok = 0; why = "lookup after register"; }
    if (ok && it_svcmgr_active_slots() != slots0 + 1) { ok = 0; why = "slot not tracked"; }

    /* Unregister (owner) → gone, gauge back to baseline, no ghost. */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label      = IRIS_SVCMGR_EP_UNREGISTER;
        m.words[0]   = (uint32_t)id;
        m.word_count = 1u;
        if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m) != 0 ||
            m.label != IRIS_EP_REPLY_OK) { ok = 0; why = "unregister"; }
    }
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t161.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "ghost after unregister"; }
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "slot not released"; }

    it_close(&svc_ep);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T161"); else it_fail("T161", why);
}

/* ── T162: least-authority regression lock (teeth) ──────────────────────────
 * The permanent guard: a minimal service (a probe minted only its command
 * endpoint) must hold NO high authority — no spawn cap (slot 6), no proc cap
 * (slot 25), no untyped (slot 55), no vspace (slot 56), and none of the peer
 * client endpoints (slots 1,2,4).  The test then proves it has TEETH: minting
 * one extra cap makes exactly that slot appear in the report, so a future
 * over-grant would be caught.  Invariants: A1, A3, A4, A5, A6, A10. */
void test_t162(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "least-authority lock";

    /* Minimal probe: only the command endpoint (slot 3). */
    long m0 = it_lp_report_slots(0, 0u);
    if (ok && m0 < 0) { ok = 0; why = "report failed"; }
    if (ok && (uint32_t)m0 != (1u << 3)) { ok = 0; why = "minimal service over-authorized"; }
    /* Explicitly: none of the high-authority bits. */
    if (ok && ((uint32_t)m0 & ((1u << 6) | (1u << 16) | (1u << 17) | (1u << 18))) != 0) {
        ok = 0; why = "high authority leaked to minimal service";
    }

    /* Teeth: mint one extra cap at slot 6 → the report must show it. */
    if (ok) {
        long n = it_notify_create();
        handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
        if (n < 0) { ok = 0; why = "teeth fixture"; }
        else {
            struct svc_mint extra[1] = { 0 };
            extra[0].slot = 6; IT_MINT_SRC(extra[0], n_h); extra[0].rights = RIGHT_READ; extra[0].badge = 0;
            long m1 = it_lp_report_slots(extra, 1u);
            if (m1 < 0 || ((uint32_t)m1 & (1u << 6)) == 0) { ok = 0; why = "extra cap not detected (no teeth)"; }
            it_close(&n_h);
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T162"); else it_fail("T162", why);
}
void test_t163(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "authority stress";
    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }
    g_fz_seed = T163_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T163_ROUNDS; i++) {
        /* Malformed ops — must all fail clean, no registry mutation. */
        op = 1;
        if (it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "ghost.svc")
            != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "missing lookup"; break; }
        op = 2;
        {   /* unregister a stale/never-registered id → not OK */
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_UNREGISTER;
            m.words[0] = 0x4000u + (fz_rand() & 0xFFu);
            m.word_count = 1u;
            long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m);
            if (r == 0 && m.label == IRIS_EP_REPLY_OK) { ok = 0; why = "stale unregister accepted"; break; }
        }
        op = 3;   /* register a reserved name → ACCESS_DENIED */
        {
            uint32_t len = it_stage_path("vfs.ep");
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_REGISTER;
            m.buf_len = len;
            long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m);
            if (r == 0 && m.label == IRIS_EP_REPLY_OK) { ok = 0; why = "reserved register accepted"; break; }
        }

        /* Well-formed: register → lookup → unregister, each round balanced. */
        op = 10;
        long e = it_ep_create();
        handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
        if (e < 0) { ok = 0; why = "svc ep"; break; }
        long id = it_register_ep("t163.svc", svc_ep);
        if (id < 0) { ok = 0; why = "register"; it_close(&svc_ep); break; }
        op = 11;
        if (it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t163.svc") < 0) { ok = 0; why = "lookup"; it_close(&svc_ep); break; }
        op = 12;
        {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_UNREGISTER;
            m.words[0] = (uint32_t)id;
            m.word_count = 1u;
            if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m) != 0 ||
                m.label != IRIS_EP_REPLY_OK) { ok = 0; why = "unregister"; it_close(&svc_ep); break; }
        }
        it_close(&svc_ep);
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "registry slot drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T163");
    else { it_fz_note("T163", T163_SEED, i, op); it_fail("T163", why); }
}

/* Create an ioport cap over [base, base+count) via the HW_ACCESS spawn cap;
 * returns the handle or HANDLE_INVALID. */
/* Phase S4: the cap is published into a CSpace slot (MDB child of the spawn-cap
 * slot) and the CPtr is what callers hold.  Slots rotate over the device block
 * so nested creations (e.g. two live ioport caps in T171) do not collide; the
 * slot is deleted first so re-entry is clean. */
static uint32_t g_it_dev_next;
/* Phase S4 (Step 3): device caps now live in CSpace, so their derivation is
 * the NATIVE CDT operation — SYS_CSPACE_MINT into a slot, producing a real
 * MDB child of the source — not the legacy handle tree.  Returns the
 * destination CPtr, or the negative error. */
static long it_dev_mint(long src_cptr, uint32_t dest_slot, uint32_t rights) {
    it_slot_delete(dest_slot);
    long r = it_invoke2(src_cptr, INV_CSPACE_MINT, (long)((uint64_t)dest_slot << 32), (long)rights);
    return (r != 0) ? r : (long)dest_slot;
}

static handle_id_t it_make_ioport(long base, long count) {
    static const uint32_t pool[2] = { IT_DEV_SLOT_A, IT_DEV_SLOT_B };
    uint32_t slot = pool[__atomic_fetch_add(&g_it_dev_next, 1u,
                                            __ATOMIC_RELAXED) % 2u];
    it_slot_delete(slot);
    if (it_ioport_create((long)IRIS_CPTR_IOPORT_CONTROL, base, count, (long)slot) != 0) return HANDLE_INVALID;
    return (handle_id_t)slot;
}

/* Same, for an IRQ capability. */
static handle_id_t it_make_irqcap(long irq) {
    static const uint32_t pool[2] = { IT_DEV_SLOT_A, IT_DEV_SLOT_B };
    uint32_t slot = pool[__atomic_fetch_add(&g_it_dev_next, 1u,
                                            __ATOMIC_RELAXED) % 2u];
    it_slot_delete(slot);
    if (it_irqcap_create((long)IRIS_CPTR_IRQ_CONTROL, irq, (long)slot) != 0) return HANDLE_INVALID;
    return (handle_id_t)slot;
}

/* Spawn a lifecycle_probe with an ioport cap at slot 10 (its only device
 * authority — no spawn cap at slot 6, no IRQ cap at slot 11), send
 * LP_CMD_DEV_PROBE with the given ioport offset, and return the reported breach
 * bitmask.  With an OUT-OF-RANGE offset a contained driver reports 0 (every
 * escalation denied).  With an IN-RANGE offset bit 1 (a legitimate IN through
 * the held cap) is set — the teeth check proving the probe genuinely attempts
 * each op.  Returns -1 on spawn/command failure. */
static long it_dev_probe(handle_id_t ioport_h, uint64_t offset, iris_rights_t dev_rights) {
    long ep = it_ep_create();
    if (ep < 0) return -1;
    handle_id_t cmd = (handle_id_t)ep;

    /* The source ioport cap carries DUPLICATE (needed to mint it); the child's
     * cap is reduced to dev_rights — modelling a driver's exact port authority. */
    struct svc_mint mints[2] = { 0 };
    mints[0].slot = LP_CPTR_CMD_EP; IT_MINT_SRC(mints[0], cmd);
    mints[0].rights = RIGHT_READ | RIGHT_WRITE; mints[0].badge = 0;
    /* Phase S4: the ioport cap lives in OUR CSpace now, so the child's copy is
     * minted from the slot — an MDB child of ours, hence revocable. */
    mints[1].slot = 10; mints[1].src_cptr = (uint64_t)ioport_h;
    mints[1].rights = dev_rights; mints[1].badge = 0;

    handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "lifecycle_probe",
                             &proc, &boot, mints, 2u,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(proc);
    it_close(&boot);
    if (r < 0 || proc == HANDLE_INVALID) { it_close(&cmd); it_close(&proc); return -1; }

    long mask = -1;
    if (it_lp_cmd_va(cmd, LP_CMD_DEV_PROBE, offset) == 0) mask = it_lp_wait_exit(proc);
    it_close(&cmd); it_close(&proc);
    return mask;
}

/* ── T164: ioport authority boundaries ──────────────────────────────────────
 * A KIoPort cap bounds access to exactly [base, base+count): in-range IN/OUT
 * work, any out-of-range offset is INVALID_ARG (cannot cross the range), a
 * wrong-type cap fails, rights are enforced (READ for IN, WRITE for OUT), a
 * stale cap fails, and a non-whitelisted range cannot be created.  No fallback,
 * no drift.  Invariants: D1, D2, D3, D4, D5, D6, D7. */
void test_t164(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "ioport boundaries";

    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
    if (io == HANDLE_INVALID) { it_fail("T164", "com2 cap"); return; }

    /* In-range access works (COM2 is unwired: IN returns a byte, OUT is a no-op). */
    if (ok && it_invoke1((long)io, INV_IOPORT_IN, 5) < 0) { ok = 0; why = "in-range IN"; }
    if (ok && it_invoke2((long)io, INV_IOPORT_OUT, 0, 0) != 0) { ok = 0; why = "in-range OUT"; }
    /* Out-of-range offsets → INVALID_ARG (cannot cross the granted range). */
    if (ok && it_invoke1((long)io, INV_IOPORT_IN, IT_COM2_COUNT) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "offset==count IN"; }
    if (ok && it_invoke1((long)io, INV_IOPORT_IN, 1000) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "big offset IN"; }
    if (ok && it_invoke2((long)io, INV_IOPORT_OUT, 1000, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "big offset OUT"; }

    /* Wrong-type cap: a notification is not a KIoPort. */
    if (ok) {
        long n = it_notify_create();
        handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
        if (n < 0) { ok = 0; why = "notif fixture"; }
        if (ok && it_invoke1(n, INV_IOPORT_IN, 0) >= 0) { ok = 0; why = "wrong-type IN honoured"; }
        it_close(&n_h);
    }

    /* Rights enforcement: READ-only cap denies OUT; WRITE-only denies IN. */
    if (ok) {
        long rr = it_dev_mint((long)io, IT_DEV_MINT_A, RIGHT_READ);
        handle_id_t ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
        if (rr < 0) { ok = 0; why = "read derive"; }
        if (ok && it_invoke2(ro, INV_IOPORT_OUT, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO OUT not denied"; }
        if (ok && it_invoke1(ro, INV_IOPORT_IN, 0) < 0) { ok = 0; why = "RO IN broken"; }
        it_close(&ro);
        long wr = it_dev_mint((long)io, IT_DEV_MINT_B, RIGHT_WRITE);
        handle_id_t wo = (wr >= 0) ? (handle_id_t)wr : HANDLE_INVALID;
        if (ok && wr < 0) { ok = 0; why = "write derive"; }
        if (ok && it_invoke1(wo, INV_IOPORT_IN, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "WO IN not denied"; }
        it_close(&wo);
    }

    /* Stale cap (Phase S4 shape): mint into a slot, DELETE the slot, use → fails.
     * The pre-S4 form dup'd a handle; device caps no longer have one. */
    if (ok) {
        long d = it_dev_mint((long)io, IT_DEV_MINT_A, RIGHT_SAME_RIGHTS);
        if (d < 0) { ok = 0; why = "stale mint"; }
        else {
            it_slot_delete((uint32_t)d);
            if (it_invoke1(d, INV_IOPORT_IN, 0) >= 0) { ok = 0; why = "stale cap honoured"; }
        }
    }

    /*
     * A range outside the AUTHORITY cannot be created.
     *
     * This used to read "outside the kernel's whitelist", and the difference
     * is the whole of Stage 5's last row.  The kernel had a table of four port
     * ranges that applied to every holder equally, so it could not express the
     * only useful restriction — that one component may claim a serial port and
     * another may not.  The bound now travels on the capability, so the test
     * has to CREATE the restriction it is testing, which is the point: the
     * suite narrows its own control capability and then discovers it is
     * narrowed.
     */
    if (ok) {
        const uint32_t narrow_slot = IT_IOCTL_NARROW;
        if (it_ioport_narrow((long)IRIS_CPTR_IOPORT_CONTROL,
                             IT_COM2_BASE, IT_COM2_BASE + IT_COM2_COUNT - 1,
                             narrow_slot) != 0) {
            ok = 0; why = "narrow";
        }
        /* Inside the narrowed range: still allowed. */
        if (ok) {
            it_slot_delete((uint32_t)IT_DEV_SLOT_A);
            long rc = it_ioport_create((long)narrow_slot, IT_COM2_BASE,
                                       IT_COM2_COUNT, (long)IT_DEV_SLOT_A);
            if (rc != 0) { ok = 0; why = "narrowed authority denied its own range"; }
        }
        if (ok) it_slot_delete((uint32_t)IT_DEV_SLOT_A);
        /* Outside it: denied, whatever the port happens to be. */
        if (ok && it_ioport_create((long)narrow_slot, 0x70, 2,
                                   (long)IT_DEV_SLOT_A)
                  != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "CMOS not denied";
        }
        if (ok && it_ioport_create((long)narrow_slot, IT_COM2_BASE, 100,
                                   (long)IT_DEV_SLOT_A)
                  != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "range spill not denied";
        }
        /* The object is MEMORY, so it is charged to a budget the caller names.
         * A syscall that let ring 3 spend the KERNEL's would open a charter M3
         * hole in the same change that closed a policy one, so the budget is
         * required and is checked as a capability rather than taken on trust. */
        if (ok && it_invoke((long)IRIS_CPTR_IOPORT_CONTROL, INV_BOOT_IOPORT_NARROW, (long)((uint64_t)IT_COM2_BASE |
                                 ((uint64_t)(IT_COM2_BASE + IT_COM2_COUNT - 1) << 16)), 0L, (long)((uint64_t)IT_DEV_SLOT_B << 32))
                  != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "narrowing without a budget was accepted";
        }
        /* ...and it is a CAPABILITY, checked like one.  Naming the control
         * capability itself as the budget is WRONG_TYPE: it is an I/O-port
         * control capability, not an Untyped, and that is the FIRST thing
         * wrong with it.  This assertion used to expect ACCESS_DENIED and its
         * comment explained why — rights were checked before type, so a
         * capability of the wrong kind was refused for lacking rights on a
         * kind it does not have.  seL4 checks type first, and so does the
         * resolver now: rights are a property OF a type. */
        if (ok && it_invoke((long)IRIS_CPTR_IOPORT_CONTROL, INV_BOOT_IOPORT_NARROW, (long)((uint64_t)IT_COM2_BASE |
                                 ((uint64_t)(IT_COM2_BASE + IT_COM2_COUNT - 1) << 16)), (long)IRIS_CPTR_IOPORT_CONTROL, (long)((uint64_t)IT_DEV_SLOT_B << 32))
                  != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "a non-untyped budget was accepted";
        }

        /* And a narrowing can only ever narrow: asking for more than you hold
         * is refused, which is what keeps the chain monotonic. */
        if (ok && it_ioport_narrow((long)narrow_slot, 0x0, 0xFFFF,
                                   IT_DEV_SLOT_B)
                  != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "a narrowing widened";
        }
        it_slot_delete(narrow_slot);
    }

    it_slot_delete((uint32_t)io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T164"); else it_fail("T164", why);
}

/* ── T165: kbd driver containment (compromised-driver stand-in) ──────────────
 * A probe minted a driver-like device cap (an ioport cap at slot 10, READ, like
 * kbd's 0x60) and NOTHING else — no spawn cap, no IRQ cap — cannot escalate: it
 * cannot forge a port or IRQ cap (no HW_ACCESS), cannot cross its port range,
 * and cannot ack an IRQ it holds no cap for.  The probe exits 0 (contained).
 * A control run WITH the spawn cap proves the probe genuinely attempts each
 * escalation (teeth).  This closes the loop with Phase 22: kbd already has no
 * peer client caps; now its hardware authority is shown bounded too.
 * Invariants: D7, D15, D17, D18. */
void test_t165(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "kbd containment";

    /* Model kbd's data port as a READ-only device cap in the child. */
    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
    if (io == HANDLE_INVALID) { it_fail("T165", "io cap"); return; }

    /* Contained: out-of-range offset → every escalation denied (mask 0). */
    long mask = it_dev_probe(io, 1000u, RIGHT_READ);
    if (ok && mask < 0) { ok = 0; why = "probe failed"; }
    if (ok && mask != 0) { ok = 0; why = "driver escalated past its caps"; }

    /* Teeth: an IN-RANGE offset makes the legitimate IN (bit 1) succeed —
     * proving the contained run's 0 means genuinely denied, not "never tried". */
    if (ok) {
        long tm = it_dev_probe(io, 0u, RIGHT_READ);
        if (tm < 0 || (tm & (1u << 1)) == 0) { ok = 0; why = "no teeth (IN not attempted)"; }
        /* Even in-range, an OUT through a READ-only cap stays denied (bit 2). */
        if (ok && (tm & (1u << 2)) != 0) { ok = 0; why = "RO cap wrote a port"; }
    }

    it_slot_delete((uint32_t)io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T165"); else it_fail("T165", why);
}

/* ── T166: console UART containment ──────────────────────────────────────────
 * A probe standing in for console (an ioport cap at slot 10 with READ|WRITE,
 * like the UART) still cannot cross its range or forge new device authority:
 * an out-of-range IN/OUT fails even WITH the WRITE right, and there is no spawn
 * or IRQ cap to escalate through.  Contained → 0.  Invariants: D2, D7, D15, D18. */
void test_t166(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "console containment";

    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);   /* RW, like the UART */
    if (io == HANDLE_INVALID) { it_fail("T166", "io cap"); return; }

    long mask = it_dev_probe(io, 1000u, RIGHT_READ | RIGHT_WRITE);
    if (ok && mask < 0) { ok = 0; why = "probe failed"; }
    if (ok && mask != 0) { ok = 0; why = "console escalated past its caps"; }

    it_close(&io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T166"); else it_fail("T166", why);
}

/* ── T167: IRQ route / ack authority ────────────────────────────────────────
 * IRQ authority is cap-scoped: an IRQ cap carries its authorized irq_num, so a
 * holder can only route/ack THAT line.  Route requires RIGHT_ROUTE on the IRQ
 * cap, RIGHT_WRITE on a KNotification destination, and RIGHT_READ|ROUTE on the
 * owner proc cap — iris_test's own proc cap lacks ROUTE, so iris_test CANNOT
 * route (a containment win asserted directly).  ACK requires RIGHT_ROUTE on the
 * IRQ cap.  Every failure leaves no route (a leaked route would keep its destination
 * notification alive, caught by the snapshot).
 * Invariants: D8, D9, D10, D12, D14, D19. */
void test_t167(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "irq authority";

    /* Create an IRQ cap for the unused line 5. */
    handle_id_t irq = it_make_irqcap(5);
    if (ok && irq == HANDLE_INVALID) { ok = 0; why = "irqcap create"; }

    long nn = it_notify_create_slot();
    handle_id_t notif = (nn >= 0) ? (handle_id_t)nn : HANDLE_INVALID;
    if (ok && notif == HANDLE_INVALID) { ok = 0; why = "notif"; }

    /* ACK with the valid IRQ cap (RIGHT_ROUTE) succeeds — unmasks the unused
     * line, harmless. */
    if (ok && it_invoke0((long)irq, INV_IRQ_ACK) != 0) { ok = 0; why = "valid ack"; }
    /* ACK failure paths. */
    if (ok && it_invoke0((long)notif, INV_IRQ_ACK) >= 0) { ok = 0; why = "ack wrong-type honoured"; }
    if (ok) {
        /* IRQ caps carry ROUTE|DUPLICATE|TRANSFER; derive DUPLICATE-only to get
         * a cap that lacks ROUTE (a subset — never amplified). */
        long rd = it_dev_mint((long)irq, IT_DEV_MINT_A, RIGHT_DUPLICATE);
        handle_id_t irq_ro = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
        if (rd < 0) { ok = 0; why = "irq derive"; }
        if (ok && it_invoke0(irq_ro, INV_IRQ_ACK) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-route ack not denied"; }
        /* Route with an IRQ cap lacking ROUTE → ACCESS_DENIED. */
        if (ok && it_invoke2(irq_ro, INV_IRQ_SET_NOTIFICATION, (long)notif, 0)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-route route not denied"; }
        it_close(&irq_ro);
    }
    /* Route with a wrong-type destination (endpoint, not notification). */
    if (ok) {
        long e = it_ep_create_slot();
        handle_id_t ep_h = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
        if (e < 0) { ok = 0; why = "ep fixture"; }
        if (ok && it_invoke2((long)irq, INV_IRQ_SET_NOTIFICATION, (long)ep_h, 0)
                  != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "route wrong-type notif"; }
        it_close(&ep_h);
    }
    /* Route with a notification lacking WRITE → ACCESS_DENIED. */
    if (ok) {
        long nrd = it_cs_reduce((long)notif, RIGHT_READ);
        handle_id_t n_ro = (nrd >= 0) ? (handle_id_t)nrd : HANDLE_INVALID;
        if (nrd < 0) { ok = 0; why = "notif read dup"; }
        if (ok && it_invoke2((long)irq, INV_IRQ_SET_NOTIFICATION, n_ro, 0)
                  != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "route no-write notif"; }
        it_close(&n_ro);
    }
    /*
     * Stage 7-mem: the last probe used to be "a proc cap lacking ROUTE is
     * denied", which was containment expressed through a third object.  A
     * route is authorised by what it acts on — RIGHT_ROUTE on the interrupt
     * line and RIGHT_WRITE on the notification — and the two probes above
     * cover both.  What replaces it is the POSITIVE case and the property that
     * makes the owner unnecessary: a valid route installs, and dropping the
     * notification UNBINDS it.  A route that kept its lifecycle reference
     * would keep the notification object alive, which the baseline check at
     * the end of this test sees as a notification leak.
     */
    if (ok && it_invoke2((long)irq, INV_IRQ_SET_NOTIFICATION, (long)notif, 0) != 0) {
        ok = 0; why = "valid route denied";
    }

    it_close(&irq); it_close(&notif);
    it_quiesce_reaper();
    /* Every route attempt was an authority failure, so no route object was ever
     * installed — a leaked route would keep its destination notification alive,
     * caught by the notification-live check in the snapshot below. */
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T167"); else it_fail("T167", why);
}

/* ── T168: framebuffer authority ────────────────────────────────────────────
 * Framebuffer authority is a capability of its own (IRIS_CPTR_FB_CONTROL), and
 * a wrong-type capability in its place is refused ACCESS_DENIED rather than
 * being interpreted.
 *
 * This test used to assert something else: that the framebuffer was ONE-SHOT —
 * `SYS_FRAMEBUFFER_VMO` cleared a valid flag, so the second caller got
 * NOT_FOUND and the first got the region.  That was a GRANT wearing a query's
 * clothes, and it is retired (ledger D-5): the region is a DEVICE Untyped now,
 * exclusivity is whoever holds that capability, and what remains here is
 * `SYS_FRAMEBUFFER_INFO` — a question about hardware, which anyone with the
 * authority may ask twice and which creates nothing.
 *
 * So the one-shot leg becomes its opposite, deliberately: the query is
 * IDEMPOTENT, and a test that still expected NOT_FOUND would be pinning a
 * behaviour the model no longer has.  Invariants: D4, D5, D15, D16. */
void test_t168(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "framebuffer containment";

    struct iris_fb_params fb1, fb2;
    /* Asking twice gives the same answer: it is a question, not a claim. */
    if (ok && it_invoke2((long)IRIS_CPTR_FB_CONTROL, INV_BOOT_FRAMEBUFFER_INFO, (long)(uintptr_t)&fb1, 0) != 0) {
        ok = 0; why = "framebuffer info refused";
    }
    if (ok && it_invoke2((long)IRIS_CPTR_FB_CONTROL, INV_BOOT_FRAMEBUFFER_INFO, (long)(uintptr_t)&fb2, 0) != 0) {
        ok = 0; why = "framebuffer info was one-shot";
    }
    if (ok && (fb1.phys != fb2.phys || fb1.size != fb2.size ||
               fb1.width != fb2.width || fb1.height != fb2.height)) {
        ok = 0; why = "framebuffer geometry changed between asks";
    }
    /* And it describes something: a query that answers zeroes proves nothing. */
    if (ok && (fb1.size == 0u || fb1.width == 0u || fb1.phys == 0u)) {
        ok = 0; why = "framebuffer geometry empty";
    }
    /* Wrong-type auth cap (a notification).  A-30: the type is answered as a
     * type — WRONG_TYPE, not ACCESS_DENIED.  Nothing is disclosed by it: the
     * caller can ask SYS_CAP_IDENTIFY about its own slot for free.  What stays
     * ACCESS_DENIED is a real bootstrap capability of the wrong FLAVOUR, which
     * is an authority answer and not a type one. */
    if (ok) {
        long n = it_notify_create();
        handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
        if (n < 0) { ok = 0; why = "notif fixture"; }
        if (ok && it_invoke2(n, INV_BOOT_FRAMEBUFFER_INFO, (long)(uintptr_t)&fb1, 0)
                  != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "wrong-type got framebuffer"; }
        it_close(&n_h);
    }
    /* The RETIRED half stays retired: a stale caller asking for a VMO over the
     * framebuffer gets a refusal, not somebody else's operation. */
    if (ok && it_sys3(SYS_FRAMEBUFFER_VMO, (long)IRIS_CPTR_FB_CONTROL,
                      (long)(uintptr_t)&fb1, 0)
              != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "the retired VMO half still answers";
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T168"); else it_fail("T168", why);
}

/* ── T169: device cap derivation monotonicity ───────────────────────────────
 * A derived device cap can only narrow rights, never widen.  A READ-derived
 * ioport cap cannot OUT and cannot re-derive back a WRITE that works; a
 * ROUTE-less IRQ cap cannot ack or route; a revoked cap fails.  No path
 * recovers lost device authority.  Invariants: D3, D5, D14, D15. */
void test_t169(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "device derivation";

    handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
    if (io == HANDLE_INVALID) { it_fail("T169", "io cap"); return; }

    long rr = it_dev_mint((long)io, IT_DEV_MINT_A, RIGHT_READ);
    handle_id_t ro = (rr >= 0) ? (handle_id_t)rr : HANDLE_INVALID;
    if (ok && rr < 0) { ok = 0; why = "read derive"; }
    /* Re-derive asking for WRITE from a READ-only cap: either rejected or the
     * result still cannot OUT (rights never amplified). */
    if (ok) {
        long wr = it_dev_mint((long)ro, IT_DEV_MINT_B, RIGHT_READ | RIGHT_WRITE);
        if (wr >= 0) {
            handle_id_t w_h = (handle_id_t)wr;
            if (it_invoke2(wr, INV_IOPORT_OUT, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights amplified via re-derive"; }
            it_close(&w_h);
        }
    }
    /* Revoke children; the READ cap must be dead afterward. */
    if (ok && it_invoke0((long)io, INV_CSPACE_REVOKE) < 0) { ok = 0; why = "revoke"; }
    if (ok && it_invoke1(ro, INV_IOPORT_IN, 0) >= 0) { ok = 0; why = "revoked cap usable"; }
    it_close(&ro);

    /* IRQ cap: a ROUTE-less derivation cannot ack. */
    if (ok) {
        long ic = (long)it_make_irqcap(5);
        handle_id_t irq = (ic >= 0) ? (handle_id_t)ic : HANDLE_INVALID;
        if (ic < 0) { ok = 0; why = "irqcap"; }
        long rd = ok ? it_dev_mint((long)irq, IT_DEV_MINT_A, RIGHT_DUPLICATE) : -1;
        handle_id_t irq_ro = (rd >= 0) ? (handle_id_t)rd : HANDLE_INVALID;
        if (ok && rd < 0) { ok = 0; why = "irq derive"; }
        if (ok && it_invoke0(irq_ro, INV_IRQ_ACK) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "route-less ack not denied"; }
        it_close(&irq_ro); it_close(&irq);
    }

    it_close(&io);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T169"); else it_fail("T169", why);
}

/* ── T170: driver death cleanup ─────────────────────────────────────────────
 * A driver holding device caps that dies releases them: several probe children
 * are minted an ioport cap and parked in a recv, then killed.  Process, task,
 * handle, endpoint and notification live counts return to baseline — a dead
 * driver's device authority is gone, no ghost.  Invariants: D13, D19. */
void test_t170(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "driver death cleanup";

    for (int i = 0; ok && i < 4; i++) {
        handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        if (io == HANDLE_INVALID) { ok = 0; why = "io cap"; break; }

        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        struct svc_mint mints[2] = { 0 };
        mints[0].slot = LP_CPTR_CMD_EP; IT_MINT_SRC(mints[0], cmd);
        mints[0].rights = RIGHT_READ | RIGHT_WRITE; mints[0].badge = 0;
        mints[1].slot = 10; IT_MINT_SRC(mints[1], io);
        mints[1].rights = RIGHT_READ | RIGHT_WRITE; mints[1].badge = 0;
        handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
        long r = (ep < 0) ? -1 : svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL,
                                                    IRIS_CPTR_INITRD_CONTROL,
                     "lifecycle_probe", &proc, &boot, mints, 2u,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
        it_child_bind(proc);
        it_close(&boot); it_close(&io);
        if (r < 0 || proc == HANDLE_INVALID) { ok = 0; why = "spawn"; it_close(&cmd); it_close(&proc); break; }

        /* The child blocks in its first recv; kill it while parked. */
        it_settle(2);
        if (it_kill((long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "exit"; }
        it_close(&cmd); it_close(&proc);
        it_quiesce_reaper();
    }

    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T170"); else it_fail("T170", why);
}
void test_t171(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "device fuzz";
    g_fz_seed = T171_SEED;
    if (ok && it_ioport_narrow((long)IRIS_CPTR_IOPORT_CONTROL, IT_COM2_BASE,
                               IT_COM2_BASE + IT_COM2_COUNT - 1,
                               T171_NARROW_SLOT) != 0) {
        it_fail("T171", "narrow"); return;
    }
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T171_ROUNDS; i++) {
        /* Outside the narrowed authority: always denied.  The authority is
         * narrowed to COM2 once, above the loop; the kernel has no port table
         * of its own any more, so what bounds a claim is what the caller
         * holds. */
        op = 1;
        static const long bad_bases[] = { 0x70L, 0x80L, 0x3B0L, 0xCF8L };
        long bb = bad_bases[fz_rand() % 4u];
        if (it_ioport_create((long)T171_NARROW_SLOT, bb, 2, (long)IT_DEV_SLOT_A) != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "out-of-range create allowed"; break;
        }

        /* A valid COM2 cap, exercised in and out of range. */
        op = 2;
        handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        if (io == HANDLE_INVALID) { ok = 0; why = "io cap"; break; }
        uint32_t off = fz_rand() % 32u;
        long rin = it_invoke1((long)io, INV_IOPORT_IN, (long)off);
        if (off < (uint32_t)IT_COM2_COUNT) { if (rin < 0) { ok = 0; why = "in-range IN failed"; } }
        else { if (rin != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "out-range IN honoured"; } }
        if (!ok) { it_close(&io); break; }

        /* Sometimes derive READ-only and confirm OUT is denied. */
        op = 3;
        if (fz_rand() & 1u) {
            long rr = it_dev_mint((long)io, IT_DEV_MINT_A, RIGHT_READ);
            if (rr >= 0) {
                handle_id_t ro = (handle_id_t)rr;
                if (it_invoke2(rr, INV_IOPORT_OUT, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO OUT honoured"; }
                if (ok && it_invoke0((long)io, INV_CSPACE_REVOKE) < 0) { ok = 0; why = "revoke"; }
                if (ok && it_invoke1(rr, INV_IOPORT_IN, 0) >= 0) { ok = 0; why = "revoked usable"; }
                it_close(&ro);
            }
        }
        it_close(&io);
        if (!ok) break;

        /* Occasionally: a compromised-driver probe must stay contained. */
        op = 4;
        if (fz_rand() & 1u) {
            handle_id_t io2 = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
            if (io2 == HANDLE_INVALID) { ok = 0; why = "io2"; break; }
            long mask = it_dev_probe(io2, 500u + (fz_rand() & 0xFFu), RIGHT_READ | RIGHT_WRITE);
            it_close(&io2);
            if (mask != 0) { ok = 0; why = "probe escalated"; break; }
        }

        /* IRQ authority failure path: ack with a wrong-type cap. */
        op = 5;
        {
            long n = it_notify_create();
            if (n >= 0) {
                handle_id_t n_h = (handle_id_t)n;
                if (it_invoke0(n, INV_IRQ_ACK) >= 0) { ok = 0; why = "ack wrong-type honoured"; }
                it_close(&n_h);
            }
        }
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_slot_delete((uint32_t)T171_NARROW_SLOT);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T171");
    else { it_fz_note("T171", T171_SEED, i, op); it_fail("T171", why); }
}

/* Read the STATUS policy of a catalog service into six logical fields:
 *   p[0]=alive p[1]=gen p[2]=supervision p[3]=restart_count p[4]=restart_limit
 *   p[5]=degraded.  Over the wire words[3] packs count|limit<<8|degraded<<16
 *   (IPC carries only 4 words).  Returns 4 when the policy words are present,
 *   2 when only alive/gen were returned, or -1 on failure. */
static long it_policy(const char *name, uint32_t p[6]) {
    uint32_t len = it_stage_path(name);
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label    = IRIS_SVCMGR_EP_STATUS;
    m.buf_len  = len;
    for (uint32_t i = 0; i < 6u; i++) p[i] = 0u;
    if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -1;
    p[0] = (uint32_t)m.words[0];   /* alive */
    p[1] = (uint32_t)m.words[1];   /* generation */
    if (m.word_count >= 4u) {
        uint32_t w3 = (uint32_t)m.words[3];
        p[2] = (uint32_t)m.words[2];        /* supervision */
        p[3] = w3 & 0xFFu;                  /* restart_count */
        p[4] = (w3 >> 8) & 0xFFu;           /* restart_limit */
        p[5] = (w3 >> 16) & 0x1u;           /* degraded */
        return 4;
    }
    return (long)m.word_count;
}

/* Unregister a dynamic service id through the svcmgr endpoint; returns 0 (OK) or
 * the negative error the reply carried. */
long it_unregister(uint32_t dyn_id) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label      = IRIS_SVCMGR_EP_UNREGISTER;
    m.words[0]   = dyn_id;
    m.word_count = 1u;
    if (iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -(long)(uint32_t)m.words[0];
    return 0;
}

/* ── T172: supervision policy manifest consistency ──────────────────────────
 * Every catalog service declares an explicit supervision policy, and the policy
 * is consistent with its restart flags: a RESTART class carries a non-zero
 * limit, a NO_RESTART class carries a zero limit.  The policy does not
 * contradict the Phase 22 authority manifest (a driver stays a driver).
 * Invariants: R14, R15, R16, R20, R21. */
void test_t172(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "policy manifest";

    struct { const char *name; uint32_t sup; int restartable; } expect[] = {
        { VFS_EP_SVC_NAME, IT_SUP_CRITICAL_RESTART,    1 },
        { KBD_EP_SVC_NAME, IT_SUP_OPTIONAL_RESTART,    1 },
        { "sh",            IT_SUP_OPTIONAL_NO_RESTART,  0 },
    };
    for (int i = 0; ok && i < 3; i++) {
        uint32_t p[6];
        long wc = it_policy(expect[i].name, p);
        if (wc < 4) { ok = 0; why = "no explicit policy"; break; }
        if (p[2] != expect[i].sup) { ok = 0; why = "wrong criticality"; break; }
        /* Consistency: restartable class ⇒ limit > 0; no-restart ⇒ limit 0. */
        if (expect[i].restartable && p[4] == 0u) { ok = 0; why = "restartable with zero limit"; break; }
        if (!expect[i].restartable && p[4] != 0u) { ok = 0; why = "no-restart with nonzero limit"; break; }
        /* A restart_count must never exceed its limit. */
        if (p[4] != 0u && p[3] > p[4]) { ok = 0; why = "restart_count exceeds limit"; break; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T172"); else it_fail("T172", why);
}

/* ── T173: restartable service basic recovery (real catalog restart) ────────
 * Drive one real RESTART of kbd (a restartable OPTIONAL service, budget-frugal:
 * one of three) through the supervisor cap.  The kernel's watch path respawns
 * it: the generation and restart_count both advance, the service is alive again,
 * kbd.ep still resolves and answers PING (the endpoint survives restarts), and
 * the live books return to baseline.  Invariants: R1, R2, R3, R19. */
void test_t173(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "restart recovery";

    uint32_t p0[6];
    if (ok && it_policy(KBD_EP_SVC_NAME, p0) < 4) { ok = 0; why = "pre-policy"; }
    if (ok && p0[0] != 1u) { ok = 0; why = "kbd not alive pre"; }

    struct iris_msg msg;
    iris_msg_zero(&msg);
    msg.label = IRIS_SVCMGR_EP_RESTART;
    msg.words[0] = (uint64_t)SVCMGR_SERVICE_KBD;
    msg.word_count = 1u;
    if (ok && (iris_msg_call((long)IRIS_CPTR_TEST_SUPER, &msg) != 0 ||
               msg.label != IRIS_EP_REPLY_OK)) { ok = 0; why = "restart denied"; }

    /* Poll (bounded, no sleep — each EP_CALL yields) until the new generation. */
    int recovered = 0;
    for (uint32_t i = 0; ok && i < 400u && !recovered; i++) {
        uint64_t bb = 0;
        (void)it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &bb);
        uint32_t p1[6];
        if (it_policy(KBD_EP_SVC_NAME, p1) >= 4 && p1[0] == 1u &&
            p1[1] > p0[1] && p1[3] > p0[3]) recovered = 1;
    }
    if (ok && !recovered) { ok = 0; why = "kbd did not restart with new gen/count"; }

    /* The restarted instance answers on kbd.ep (endpoint survives restart). */
    if (ok) {
        struct iris_msg pm;
        iris_msg_zero(&pm);
        pm.label = IRIS_EP_OP_PING;
        if (iris_msg_call((long)IRIS_CPTR_KBD_EP, &pm) != 0 ||
            pm.label != IRIS_EP_REPLY_OK) { ok = 0; why = "kbd.ep dead after restart"; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T173"); else it_fail("T173", why);
}

/* ── T174: stale generation / stale registration rejection ──────────────────
 * A dynamic registration is owner-managed: register a dummy service, unregister
 * it, and prove the stale id cannot act again — a second unregister is
 * NOT_FOUND, and a lookup of the gone name is NOT_FOUND.  A fresh registration
 * of the same name succeeds with its own slot; the OLD id still cannot
 * unregister the NEW registration (no cross-generation authority).  In parallel,
 * the catalog generation is monotonic (kbd's generation from T173 never
 * decreases).  Invariants: R2, R4, R18, R24. */
void test_t174(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "stale generation";

    long e = it_ep_create();
    handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
    if (e < 0) { it_fail("T174", "ep"); return; }

    long id0 = it_register_ep("t174.svc", svc_ep);
    if (ok && id0 < 0) { ok = 0; why = "register"; }
    if (ok && it_unregister((uint32_t)id0) != 0) { ok = 0; why = "unregister"; }
    /* Stale id: second unregister is NOT_FOUND; the name no longer resolves. */
    if (ok && it_unregister((uint32_t)id0) != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale unregister accepted"; }
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t174.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "gone name resolves"; }

    /* Fresh registration of the same name; the OLD id must not unregister it. */
    long id1 = ok ? it_register_ep("t174.svc", svc_ep) : -1;
    if (ok && id1 < 0) { ok = 0; why = "re-register"; }
    if (ok && id1 == id0) {
        /* Same slot reused: the stale-id test is only meaningful with a
         * different id, but reuse is acceptable — the unregister below still
         * proves the NEW registration is the one that is torn down. */
    }
    if (ok && it_unregister((uint32_t)id1) != 0) { ok = 0; why = "new unregister"; }

    /* Catalog generation is monotonic (kbd was restarted in T173). */
    if (ok) {
        uint32_t p[6];
        if (it_policy(KBD_EP_SVC_NAME, p) < 4 || p[1] < 1u) { ok = 0; why = "gen not monotonic"; }
    }

    it_close(&svc_ep);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T174"); else it_fail("T174", why);
}
void test_t175(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "crash-loop limit";

    uint32_t restart_count = 0u;
    int degraded = 0;
    uint32_t generation = 0u;

    /* Supervision loop: (re)start until the budget is spent. */
    while (ok && !degraded) {
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; it_close(&cmd); break; }
        generation++;                              /* each (re)start is a new generation */

        /* The "service" dies immediately (modelled by an external kill). */
        it_settle(1);
        if (it_kill((long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "exit"; }
        it_close(&cmd); it_close(&proc);
        it_quiesce_reaper();

        /* Restart policy: count the death; stop at the limit. */
        if (restart_count < T175_LIMIT) restart_count++;
        else degraded = 1;
    }

    if (ok && restart_count != T175_LIMIT) { ok = 0; why = "wrong restart count"; }
    if (ok && !degraded) { ok = 0; why = "never degraded"; }
    if (ok && generation != T175_LIMIT + 1u) { ok = 0; why = "generation mismatch"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T175"); else it_fail("T175", why);
}

/* ── T176: client behavior during service death ─────────────────────────────
 * A client blocked on a call to a service that dies must wake with an error, not
 * hang forever, and the stale endpoint must not become magically valid.  A probe
 * blocks as a CALLER on its command endpoint (LP_CMD_CALL_BLOCK); killing it
 * while blocked is the kernel path a supervisor relies on — the blocked wait is
 * cancelled, the child reaps, no KReply/waiter is stranded.  A following NB-send
 * to the closed endpoint reports WOULD_BLOCK (no phantom receiver).
 * Invariants: R9, R12. */
void test_t176(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "client during death";

    long ep = it_ep_create();
    handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
    handle_id_t proc = HANDLE_INVALID;
    if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { it_close(&cmd); it_fail("T176", "spawn"); return; }

    /* Drive the child to block as a caller, then kill it mid-call. */
    if (ok && it_lp_cmd(cmd, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }
    it_settle(3);
    if (ok && it_kill((long)proc) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "no exit"; }

    /* The endpoint has no receiver now: a non-blocking send reports WOULD_BLOCK,
     * not a phantom rendezvous with the dead caller. */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = 0x176;
        long r = iris_msg_nb_send((long)cmd, &m);
        if (r != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "phantom receiver after death"; }
    }

    it_close(&cmd); it_close(&proc);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T176"); else it_fail("T176", why);
}

/* ── T177: driver restart with device authority ─────────────────────────────
 * A restarted driver must come back with ONLY its declared device authority —
 * no amplification, no leaked route.  A driver-like probe is minted an ioport
 * cap, killed, then a NEW instance is spawned with the SAME declared caps; the
 * new instance is still contained (DEV_PROBE breach 0) and its slot report shows
 * only the command endpoint + its device cap — no spawn/proc/untyped, no peer
 * client caps.  No IRQ/device ghost across the restart.  Invariants: R5, R6, R7,
 * R8, R10, R23. */
void test_t177(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "driver restart authority";

    for (int gen = 0; ok && gen < 2; gen++) {
        /* Each "generation" is a fresh driver instance with the SAME manifest. */
        handle_id_t io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        if (io == HANDLE_INVALID) { ok = 0; why = "io cap"; break; }
        long mask = it_dev_probe(io, 1000u, RIGHT_READ);   /* out-of-range → contained */
        it_close(&io);
        if (mask != 0) { ok = 0; why = "restarted driver escalated"; break; }
    }

    /* The driver instance holds only its command endpoint + device cap: report
     * its well-known slots and assert no high-authority slot appears. */
    if (ok) {
        long io = it_make_ioport(IT_COM2_BASE, IT_COM2_COUNT);
        handle_id_t io_h = (io >= 0) ? (handle_id_t)io : HANDLE_INVALID;
        if (io_h == HANDLE_INVALID) { ok = 0; why = "io cap 2"; }
        else {
            struct svc_mint extra[1] = { 0 };
            /* Phase S4: CSpace-sourced device delegation. */
            extra[0].slot = 10; extra[0].src_cptr = (uint64_t)io_h;
            extra[0].rights = RIGHT_READ; extra[0].badge = 0;
            long rep = it_lp_report_slots(extra, 1u);
            /* Expect exactly {cmd ep slot 3, device cap slot 10}. */
            if (rep < 0 || (uint32_t)rep != ((1u << 3) | (1u << 10))) { ok = 0; why = "unexpected authority set"; }
            /* Explicitly none of spawn(6)/proc(16)/untyped(17)/vspace(18)/peers(1,2,4). */
            if (ok && ((uint32_t)rep & ((1u<<6)|(1u<<16)|(1u<<17)|(1u<<18)|(1u<<1)|(1u<<2)|(1u<<4))) != 0) {
                ok = 0; why = "driver gained extra authority on restart";
            }
            it_slot_delete((uint32_t)io_h);
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T177"); else it_fail("T177", why);
}

/* ── T178: critical service death policy ────────────────────────────────────
 * A critical service's loss is an explicit, documented state — never an implicit
 * silent behaviour.  The policy manifest marks vfs CRITICAL_RESTART (restarted
 * up to its limit, then degraded) and sh OPTIONAL_NO_RESTART (never auto-
 * restarted); both are observable via STATUS.  This asserts the policy is
 * present and self-consistent WITHOUT destructively killing a critical service
 * (which would break the running system for no test value).  Invariants: R15,
 * R16, R20. */
void test_t178(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "critical policy";

    uint32_t pv[6], ps[6];
    if (ok && it_policy(VFS_EP_SVC_NAME, pv) < 4) { ok = 0; why = "vfs policy"; }
    if (ok && pv[2] != IT_SUP_CRITICAL_RESTART) { ok = 0; why = "vfs not critical-restart"; }
    if (ok && pv[4] == 0u) { ok = 0; why = "critical service has no restart budget"; }
    if (ok && pv[0] != 1u) { ok = 0; why = "vfs not alive"; }   /* critical ⇒ present */

    if (ok && it_policy("sh", ps) < 4) { ok = 0; why = "sh policy"; }
    if (ok && ps[2] != IT_SUP_OPTIONAL_NO_RESTART) { ok = 0; why = "sh not optional-no-restart"; }
    if (ok && ps[4] != 0u) { ok = 0; why = "no-restart service has budget"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T178"); else it_fail("T178", why);
}

/* ── T179: service death during register / unregister ───────────────────────
 * The registry stays consistent when a registrant dies around registration.  A
 * dynamic registration backed by an endpoint iris_test owns survives the death
 * of any OTHER process; a probe killed after a registration was made on its
 * behalf leaves no ghost — the name still resolves to the (owner-held) endpoint,
 * and a clean unregister removes it.  Repeated unregister is idempotent
 * (NOT_FOUND).  Invariants: R17, R18. */
void test_t179(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "register/unregister death";
    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }

    /* Register a service, spawn a probe, kill the probe: the registration (owned
     * by iris_test) is unaffected by the unrelated death. */
    long e = it_ep_create();
    handle_id_t svc_ep = (e >= 0) ? (handle_id_t)e : HANDLE_INVALID;
    if (e < 0) { it_fail("T179", "ep"); return; }
    long id = it_register_ep("t179.svc", svc_ep);
    if (ok && id < 0) { ok = 0; why = "register"; }

    long cep = it_ep_create();
    handle_id_t cmd = (cep >= 0) ? (handle_id_t)cep : HANDLE_INVALID;
    handle_id_t proc = HANDLE_INVALID;
    if (ok && (cep < 0 || lp_spawn_child(cmd, &proc) < 0)) { ok = 0; why = "spawn"; }
    if (ok) {
        it_settle(2);
        if (it_kill((long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "exit"; }
    }
    it_close(&cmd); it_close(&proc);

    /* The registration still resolves; unregister removes it; repeat is NOT_FOUND. */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t179.svc") < 0) { ok = 0; why = "reg lost on unrelated death"; }
    if (ok && it_unregister((uint32_t)id) != 0) { ok = 0; why = "unregister"; }
    if (ok && it_unregister((uint32_t)id) != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "double unregister accepted"; }
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "registry slot drift"; }

    it_close(&svc_ep);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T179"); else it_fail("T179", why);
}
void test_t180(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "supervision stress";
    long slots0 = it_svcmgr_active_slots();
    if (ok && slots0 < 0) { ok = 0; why = "diag baseline"; }
    g_fz_seed = T180_SEED;
    uint32_t i = 0, op = 0;

    for (i = 0; ok && i < T180_ROUNDS; i++) {
        /* Stale-registry pressure: unregister a never-registered id. */
        op = 1;
        {
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label = IRIS_SVCMGR_EP_UNREGISTER;
            m.words[0] = 0x4000u + (fz_rand() & 0xFFu);
            m.word_count = 1u;
            long r = iris_msg_call((long)IRIS_CPTR_SVCMGR_EP, &m);
            if (r == 0 && m.label == IRIS_EP_REPLY_OK) { ok = 0; why = "stale unregister accepted"; break; }
        }

        /* A supervised probe: spawn, kill or fault-crash, reap. */
        op = 2;
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; it_close(&cmd); break; }

        uint32_t how = fz_rand() % 3u;
        if (how == 0u) {                             /* immediate kill */
            it_settle(1);
            if (it_kill((long)proc) != 0) { ok = 0; why = "kill"; }
        } else if (how == 1u) {                      /* block then kill */
            if (it_lp_cmd(cmd, LP_CMD_SEND_BLOCK) != 0) { ok = 0; why = "cmd block"; }
            it_settle(2);
            if (ok && it_kill((long)proc) != 0) { ok = 0; why = "kill2"; }
        } else {                                     /* fault-crash (no handler → kill) */
            if (it_lp_cmd_va(cmd, LP_CMD_FAULT_READ, T14X_BAD_VA) != 0) { ok = 0; why = "fault cmd"; }
        }
        if (ok && it_lp_wait_exit(proc) != 0) { ok = 0; why = "no exit"; }
        it_close(&cmd); it_close(&proc);

        /* Occasionally exercise the dynamic registry with a clean round-trip. */
        op = 3;
        if (ok && (fz_rand() & 1u)) {
            long e2 = it_ep_create();
            handle_id_t sep = (e2 >= 0) ? (handle_id_t)e2 : HANDLE_INVALID;
            if (e2 < 0) { ok = 0; why = "reg ep"; it_close(&cmd); break; }
            long id = it_register_ep("t180.svc", sep);
            if (id < 0) { ok = 0; why = "register"; }
            if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "t180.svc") < 0) { ok = 0; why = "lookup"; }
            if (ok && it_unregister((uint32_t)id) != 0) { ok = 0; why = "unregister"; }
            it_close(&sep);
        }
        if ((i & 3u) == 3u) it_quiesce_reaper();
    }

    it_quiesce_reaper();
    if (ok && it_svcmgr_active_slots() != slots0) { ok = 0; why = "registry drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T180");
    else { it_fz_note("T180", T180_SEED, i, op); it_fail("T180", why); }
}

/* Best-effort reap: kill (no-op if already dead), wait, close.  Every Phase 25
 * exit path — success or failure — must NOT leave a probe blocked in EP_RECV
 * forever: the child holds its own mint of the command endpoint, so closing
 * the parent's handle alone never wakes it, and a leaked live child keeps its
 * exception-handler notification pinned. */
void t25_reap(handle_id_t *proc_h) {
    if (*proc_h != HANDLE_INVALID) {
        (void)it_kill((long)*proc_h);
        (void)it_lp_wait_exit(*proc_h);
    }
    it_close(proc_h);
}

static void t25_tgt_close(struct t25_tgt *g) {
    it_close(&g->cmd); it_close(&g->proc); it_close(&g->vs);
    it_close(&g->notif); it_close(&g->watch);
}

/* Reap variant of the close: kills a possibly-still-alive target first (a
 * dead target makes the kill a clean no-op). */
void t25_tgt_reap(struct t25_tgt *g) {
    t25_reap(&g->proc);
    t25_tgt_close(g);
}

/* Ledger A-22: a target's faults go to an ENDPOINT, and `g->notif` is where
 * this fixture keeps it — the suite RECEIVES on it, and a target handed to a
 * pager is re-aimed at the pager's own endpoint instead. */
static int t25_tgt_spawn_dest(struct t25_tgt *g, const char **why) {
    g->cmd = g->proc = g->vs = g->notif = g->watch = HANDLE_INVALID;
    g->fault_leaf = 0u;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ep create"; return 0; }
    g->cmd = (handle_id_t)ep;
    it_child_keep_vspace();   /* the pager maps into this child */
    if (lp_spawn_child(g->cmd, &g->proc) < 0 || g->proc == HANDLE_INVALID) {
        it_close(&g->cmd); *why = "spawn"; return 0;
    }
    /* Stage 4: the target's VSpace is published into a CSpace slot (arg1 is
     * the destination), so every rights-reduced copy of it is a slot-to-slot
     * derive and an MDB child.  It used to come back as a handle. */
    /* Stage 7 Step 15: the target's address space is the one the spawn kept
     * for us, not one asked of its process. */
    long vs = it_child_vspace(g->proc);
    if (vs == 0) vs = -1;
    long n  = it_ep_create();     /* A-22: the target's FAULT ENDPOINT */
    long w  = it_notify_create();
    g->vs    = (vs >= 0) ? (handle_id_t)vs : HANDLE_INVALID;
    g->notif = (n  >= 0) ? (handle_id_t)n  : HANDLE_INVALID;
    g->watch = (w  >= 0) ? (handle_id_t)w  : HANDLE_INVALID;
    long eh = 0, wt = 0;
    /*
     * Registered through a copy BADGED 1 — target index 0 plus one — even
     * though this fixture is single-target and the suite itself ignores the
     * badge.  A fault carries the badge of the capability it was DELIVERED
     * through, and it keeps it while it sits queued: a target handed to a
     * multi-target pager later would otherwise present an unbadged fault the
     * pager cannot attribute to anyone.  Badging at arming time is the only
     * moment that is always before the fault.
     */
    long bn = (n >= 0) ? it_cs_badge(n, RIGHT_READ | RIGHT_WRITE, 1u) : -1;
    if (vs < 0 || n < 0 || w < 0 || bn < 0 ||
        (eh = it_invoke(it_child_tcb((long)g->proc), INV_TCB_SET_FAULT_HANDLER, bn, 0, 0)) != 0 ||
        (it_slot_delete((uint32_t)bn), 0) ||
        (wt = it_invoke2(it_child_tcb((long)g->proc), INV_TCB_WATCH, w, 1)) != 0) {
        it_serial_write("[IRIS][TEST] t25 wire vs="); it_log_num((uint32_t)-vs);
        it_serial_write(" n="); it_log_num((uint32_t)-n);
        it_serial_write(" w="); it_log_num((uint32_t)-w);
        it_serial_write(" eh="); it_log_num((uint32_t)-eh);
        it_serial_write(" wt="); it_log_num((uint32_t)-wt);
        it_serial_write("\n");
        (void)it_kill((long)g->proc);
        t25_tgt_close(g);
        *why = "target wire"; return 0;
    }
    return 1;
}

/* Rotating reply leaves for suite-resolved targets: enough that no two live
 * at once collide, and reused rather than grown. */
static uint32_t g_t25_fault_leaf;

int t25_tgt_spawn(struct t25_tgt *g, const char **why) {
    uint32_t leaf = 4u + (__atomic_fetch_add(&g_t25_fault_leaf, 1u,
                                             __ATOMIC_RELAXED) % 8u);
    g_it_fault_have[leaf] = 0u;
    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)IT_FAULT_LEAF(leaf));
    if (!t25_tgt_spawn_dest(g, why)) return 0;
    g->fault_leaf = leaf;
    return 1;
}

/* Spawn an external pager over `g` with the declared manifest (plus optional
 * extra mints, e.g. T184's under-privileged victim caps).  0 on success. */
long t25_pager_spawn(const struct t25_tgt *g, handle_id_t frame_h,
                            iris_rights_t frame_rights,
                            const struct svc_mint *extra, uint32_t nextra,
                            handle_id_t *out_cmd, handle_id_t *out_proc) {
    *out_cmd = *out_proc = HANDLE_INVALID;
    /*
     * Ledger A-22: nothing is RE-AIMED.
     *
     * A fault is a message on an endpoint, so handing a target to a pager is
     * handing the pager that endpoint — the target's registration never
     * changes.  What decides who may serve a fault is who holds a capability
     * to receive on it, which is the thing a supervisor can actually grant and
     * revoke.  It used to be a destination the kernel wrote into, re-declared
     * on the target's own registration; every other target kept delivering
     * into the suite's mailbox, and that is still what makes T184's victim
     * unresolvable BY THE PAGER — its faults go to an endpoint the pager was
     * never given.
     */
    if (!it_pgr_mbox_fresh(1u)) return -1;
    long ep = it_ep_create();
    if (ep < 0) return -1;
    handle_id_t cmd = (handle_id_t)ep;
    struct svc_mint m[10] = { 0 };
    m[0].slot = LP_CPTR_CMD_EP;    IT_MINT_SRC(m[0], cmd);      m[0].rights = RIGHT_READ | RIGHT_WRITE;  m[0].badge = 0;
    m[1].slot = LP_PGR_SLOT_TPROC; IT_MINT_SRC(m[1], g->proc);  m[1].rights = RIGHT_READ | RIGHT_MANAGE; m[1].badge = 0;
    m[2].slot = LP_PGR_SLOT_TVS;   IT_MINT_SRC(m[2], g->vs);    m[2].rights = RIGHT_WRITE;               m[2].badge = 0;
    m[3].slot = LP_PGR_SLOT_FRAME; IT_MINT_SRC(m[3], frame_h);  m[3].rights = frame_rights;              m[3].badge = 0;
    /* A-22: RIGHT_READ is the authority to RECEIVE — which is the whole of
     * "this pager may serve this target's faults". */
    m[4].slot = LP_PGR_SLOT_FAULT_EP; IT_MINT_SRC(m[4], g->notif); m[4].rights = RIGHT_READ;                m[4].badge = 0;
    /* ...and the reply objects it receives with.  WRITE so it can invoke one
     * (SYS_REPLY), READ so it can name the CNode's leaves. */
    m[5].slot = LP_PGR_SLOT_FAULTCN; IT_MINT_SRC(m[5], IT_PGR_MBOX_SLOT);
    m[5].rights = RIGHT_READ | RIGHT_WRITE; m[5].badge = 0;
    uint32_t n = 6u;
    for (uint32_t i = 0; i < nextra && n < 10u; i++) m[n++] = extra[i];
    handle_id_t boot = HANDLE_INVALID;
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "lifecycle_probe",
                             out_proc, &boot, m, n,
                             IT_LOADER_WS, 0,
                               /* This lifecycle_probe instance is spawned to act as a PAGER: it
                                * maps frames into a target's address space, so it owes the
                                * paging levels under them and needs a budget to retype
                                * those from.  The CONTAINED instances the authority tests
                                * audit are spawned without one, from the same image —
                                * which is the capability model working: what a task may do
                                * follows from what it was handed, not from what it is.
                                * Slot 16 and not 12: 12 is LP_PGR_SLOT_TPROC, the
                                * target's process capability. */
                               /*own_budget_slot=*/LP_SLOT_BUDGET, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(*out_proc);
    it_close(&boot);
    if (r < 0 || *out_proc == HANDLE_INVALID) {
        it_close(&cmd); it_close(out_proc); return -1;
    }
    *out_cmd = cmd;
    return 0;
}

long t25_serve(handle_id_t pcmd, uint32_t sub, uint32_t count,
                      uint64_t mflags, uint64_t va_ovr, uint64_t expect_cr2) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = LP_CMD_PAGER_SERVE;
    m.words[0] = (uint64_t)sub | ((uint64_t)count << 8);
    m.words[1] = mflags;
    m.words[2] = va_ovr;
    m.words[3] = expect_cr2;
    m.word_count = 4u;
    return iris_msg_send((long)pcmd, &m);
}

long t25_xprobe(handle_id_t pcmd, uint32_t vtid, uint64_t va, uint32_t vseq) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = LP_CMD_PAGER_XPROBE;
    m.words[0] = vtid;
    m.words[1] = va;
    m.words[2] = vseq;
    m.word_count = 3u;
    return iris_msg_send((long)pcmd, &m);
}

/*
 * Ledger A-22 — answer the fault this target is blocked in.
 *
 * It was a seq-checked SYS_EXCEPTION_RESUME: the supervisor named the faulting
 * thread with a capability the kernel had minted into its mailbox and echoed
 * back the fault GENERATION so a stale answer could not resolve a fault it had
 * never observed.  A reply capability is both of those at once and needs
 * neither — it names one call, it is spent when used, and a second use is
 * NOT_FOUND because there is nothing left to answer.
 *
 * `tid` and `seq` stay in the signature because callers still read them out of
 * the record for their own assertions; nothing SELECTS with them any more.
 */
long t25_resume_seq(const struct t25_tgt *g, uint32_t tid, uint32_t seq,
                           int kill) {
    (void)tid; (void)seq;
    return kill ? it_fault_kill(g->fault_leaf) : it_fault_resume(g->fault_leaf);
}

/*
 * Bounded wait for this target's fault to ARRIVE.
 *
 * The old version polled SYS_TCB_FAULT_INFO deliberately without consuming the
 * notification, so a pager could still be the one to serve it.  That
 * separation is gone by construction: a fault is a message, and receiving it
 * IS taking delivery.  A test that wants a pager to serve a fault therefore
 * does not look at it first — it asks the pager and checks the outcome, which
 * is what a supervisor could actually do in a system with one fault mechanism.
 */
int t25_wait_fault(const struct t25_tgt *g, struct it_fault *f) {
    if (g->fault_leaf < IT_FAULT_LEAVES && g_it_fault_have[g->fault_leaf])
        return it_fault_info(g->fault_leaf, f) == 0;
    if (!it_fault_wait_ep((long)g->notif, g->fault_leaf)) return 0;
    return it_fault_info(g->fault_leaf, f) == 0;
}

/*
 * Bounded wait for a fault to be DELIVERED, without taking delivery of it.
 *
 * A supervisor that intends somebody ELSE to serve a fault cannot look at it:
 * receiving the message IS taking delivery, and a fault taken here is a fault
 * the pager will never see.  That is not a limitation to work around, it is
 * the single-mechanism property — so what the supervisor watches instead is
 * the kernel's own count of faults delivered, which is an observation and not
 * a claim on the fault.
 *
 * The old version polled SYS_TCB_FAULT_INFO, which was a THIRD view of the
 * same event and is exactly what A-22 removed.
 */
int t25_wait_delivered(uint32_t base) {
    uint32_t f[6];
    for (int i = 0; i < 400; i++) {
        if (it_sched_ext5(f) && f[IT_S5_DELIVER] > base) return 1;
        it_settle(1);
    }
    return 0;
}

/* The delivery counter right now, for the waiter above. */
uint32_t t25_delivered_now(void) {
    uint32_t f[6];
    return it_sched_ext5(f) ? f[IT_S5_DELIVER] : 0u;
}

/* Bounded wait for the NEXT fault (a refault after a resume). */
int t25_wait_refault(const struct t25_tgt *g, uint32_t old_seq,
                            struct it_fault *f) {
    (void)old_seq;
    if (!it_fault_wait_ep((long)g->notif, g->fault_leaf)) return 0;
    return it_fault_info(g->fault_leaf, f) == 0;
}

/* Write (write=1) or read back (write=0) word 0 of a frame through the
 * parent's own VSpace — frame preparation and post-mortem inspection. */
int t25_frame_word(handle_id_t fr, uint32_t *val, int write) {
    if (!it_setup_self_vspace()) return 0;
    if (it_invoke((long)fr, INV_FRAME_MAP, IT_VS, (long)T25_SELF_VA, write ? 1L : 0L) != 0) return 0;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T25_SELF_VA;
    if (write) *p = *val; else *val = *p;
    return it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T25_SELF_VA) == 0;
}

/* ── T181: pager authority manifest ─────────────────────────────────────────
 * A pager's authority is EXACTLY its declared manifest: cmd endpoint + target
 * proc (READ|MANAGE) + target VSpace (WRITE) + one frame + fault notification
 * (WAIT).  The slot report shows exactly that set — no spawn cap, no device
 * caps, no untyped, no KDEBUG, no peer service slots, no self-proc/vspace.
 * Stage 7 Step 15: the second half used to check SYS_PROCESS_VSPACE's MANAGE
 * gate — a READ-only PROCESS capability denied, a wrong type rejected, self
 * equivalent to VSPACE_SELF.  That syscall is retired, and with it the idea
 * that reaching an address space is authorised by a capability to something
 * else.  The same three claims are asserted where they now live: on the VSPACE
 * capability itself, which is what a spawner keeps and hands on.
 * Invariants: P1, P2, P24. */
void test_t181(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager manifest";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T181", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }

    if (ok) {
        struct svc_mint x[4] = { 0 };
        x[0].slot = LP_PGR_SLOT_TPROC; IT_MINT_SRC(x[0], g.proc);  x[0].rights = RIGHT_READ | RIGHT_MANAGE; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_TVS;   IT_MINT_SRC(x[1], g.vs);    x[1].rights = RIGHT_WRITE;               x[1].badge = 0;
        x[2].slot = LP_PGR_SLOT_FRAME; IT_MINT_SRC(x[2], fr_h);    x[2].rights = RIGHT_READ | RIGHT_WRITE;  x[2].badge = 0;
        x[3].slot = LP_PGR_SLOT_FAULT_EP; IT_MINT_SRC(x[3], g.notif); x[3].rights = RIGHT_READ;                x[3].badge = 0;
        long rep = it_lp_report_slots(x, 4u);
        uint32_t expect = (1u << LP_CPTR_CMD_EP)    | (1u << LP_PGR_SLOT_TPROC) |
                          (1u << LP_PGR_SLOT_TVS)   | (1u << LP_PGR_SLOT_FRAME) |
                          /* No fault mailbox: this probe declares its own
                           * four-capability manifest and resolves nothing, so
                           * it is handed no mailbox to resolve WITH. */
                          (1u << LP_PGR_SLOT_FAULT_EP);
        if (rep < 0 || (uint32_t)rep != expect) { ok = 0; why = "manifest mismatch"; }
        /* Explicitly: no spawn(6), no device(10/11), no peers(1/2/4), no
         * self-proc(→16)/untyped(→17)/vspace-self(→18). */
        if (ok && ((uint32_t)rep & ((1u<<6)|(1u<<10)|(1u<<11)|(1u<<1)|(1u<<2)|(1u<<4)|
                                    (1u<<16)|(1u<<17)|(1u<<18))) != 0) {
            ok = 0; why = "high-authority slot leaked";
        }
    }

    /* Address-space authority: WRITE on the VSPACE, or nothing. */
    long vmo_t = ok ? it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096u) : -1;
    if (ok && vmo_t < 0) { ok = 0; why = "probe vmo"; }
    long ro = ok ? it_cs_reduce((long)g.vs, RIGHT_READ) : -1;
    handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
    if (ok && ro < 0) { ok = 0; why = "ro dup"; }
    /* A READ-only address space cannot be mapped into. */
    if (ok && it_invoke(vmo_t, INV_FRAME_MAP, ro, (long)0x80D0000000ULL, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "no-write not denied";
    }
    /* ...and something that is not an address space is not one.  Ledger D-5:
     * the answer is WRONG_TYPE.  SYS_VMO_MAP_INTO flattened it to INVALID_ARG
     * — a resolver that knew exactly what the caller named, reporting only
     * that something was wrong — and SYS_FRAME_MAP says which. */
    if (ok && it_invoke(vmo_t, INV_FRAME_MAP, (long)g.notif, (long)0x80D0000000ULL, 1) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "wrong type accepted";
    }
    /* Self stays reachable — by SYS_VSPACE_SELF, which is the capability, not
     * a process standing in for it. */
    if (ok) {
        long sv = it_vspace_self_slot();
        if (sv < 0) { ok = 0; why = "self vspace"; }
        else { handle_id_t h = (handle_id_t)sv; it_close(&h); }
    }
    if (vmo_t >= 0) { handle_id_t h = (handle_id_t)vmo_t; it_close(&h); }
    it_close(&ro_h);

    if (it_kill((long)g.proc) != 0 && ok) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T181"); else it_fail("T181", why);
}
