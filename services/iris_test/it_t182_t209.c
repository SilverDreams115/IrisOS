/*
 * it_t182_t209.c — tests T182 through T209.
 *
 * The suite's numbering is chronological, not thematic: T182 was written
 * stages before T209, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T182 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


/* ── T182: external pager receives an invalid-VA fault ──────────────────────
 * The full external delivery path: the target touches an unmapped VA; the
 * PAGER (a separate process) wakes on its WAIT-only notification cap, reads
 * honest fault info through its READ|MANAGE target cap (validating cr2
 * itself), decides, and resolves with a seq-checked kill.  Delivery is
 * exactly-once (no residual signal, no residual record) and hands the pager
 * no capability it was not minted.  Invariants: P2, P3, P9, P12, P21. */
void test_t182(void) {
    uint32_t f0[6], f1[6];
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_sched_ext5(f0);
    const char *why = "external delivery";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T182", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "pager spawn";
    }

    /* Pager: one fault, validate cr2 == T25_VA_A, seq-checked kill. */
    if (ok && t25_serve(pcmd, 3u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "serve cmd"; }
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target not killed"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report"; }

    /* Exactly-once: the signal was consumed by the pager and never re-fires;
     * the record did not outlive the resolution. */
    if (ok) {
        uint64_t bits = 0;
        if (it_wait_timeout( (long)g.notif, (long)(uintptr_t)&bits,
                    100000000LL) == 0 && (bits & 1ull)) { ok = 0; why = "double delivery"; }
    }
    if (ok && it_fault_info(g.fault_leaf, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }

    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivered != 1"; }
    if (ok && f1[IT_S5_KILL]    != f0[IT_S5_KILL] + 1u)    { ok = 0; why = "kill count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 1u)   { ok = 0; why = "cleanup count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T182"); else it_fail("T182", why);
}

/* ── T183: pager maps a frame into the target VSpace ────────────────────────
 * The resolution path that DEFINES a pager: map-into-target + resume, both by
 * explicit capability.  (A) read fault: the pager installs its frame
 * read-only at the faulting VA and seq-resumes; the target continues and
 * READS the pattern the supervisor placed in the frame (its exit code proves
 * the data flowed).  (B) write fault: a second target's store retires into a
 * writable mapping and is visible to the supervisor through the frame
 * afterwards.  Target death sweeps the pager-installed mapping; the frame
 * stays intact and reusable.  Invariants: P5, P7, P9, P11, P18, P19, P20. */
void test_t183(void) {
    uint32_t f0[6], f1[6], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_sched_ext5(f0);
    const char *why = "map into target";

    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { it_fail("T183", "frame retype"); return; }
    word = T25_PATTERN;
    if (!t25_frame_word(fr_h, &word, 1)) { it_close(&fr_h); it_fail("T183", "frame fill"); return; }

    /* (A) read fault, read-only frame cap — the pager cannot and need not map
     * writable; the target reads the supervisor's pattern. */
    struct t25_tgt g;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g, &why)) { it_close(&fr_h); it_fail("T183", why); return; }
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "pager spawn A";
    }
    if (ok && t25_serve(pcmd, 1u, 1u, 0 /*RO map*/, 0, T25_VA_A) != 0) { ok = 0; why = "serve A"; }
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault A"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "target did not continue"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report A"; }
    if (ok && it_fault_info(g.fault_leaf, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived A";
    }
    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&g);

    /* Target death swept the mapping: the frame must be clean and intact. */
    it_quiesce_reaper();
    if (ok && (!t25_frame_word(fr_h, &word, 0) || word != T25_PATTERN)) {
        ok = 0; why = "frame not reusable after sweep";
    }

    /* (B) write fault, writable mapping — the store lands in the frame. */
    if (ok) {
        word = 0;
        if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame zero"; }
    }
    struct t25_tgt g2;
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g2, &why)) { it_close(&fr_h); it_fail("T183", why); return; }
    if (ok) {
        if (t25_pager_spawn(&g2, fr_h, RIGHT_READ | RIGHT_WRITE, 0, 0u,
                            &p2cmd, &p2proc) != 0) { ok = 0; why = "pager spawn B"; }
        if (ok && t25_serve(p2cmd, 1u, 1u, 1 /*W map*/, 0, T25_VA_B) != 0) { ok = 0; why = "serve B"; }
        if (ok && it_lp_cmd_va(g2.cmd, LP_CMD_FAULT_WRITE, T25_VA_B) != 0) { ok = 0; why = "fault B"; }
        if (ok && it_lp_wait_exit(g2.proc) != LP_EXIT_MARKER) { ok = 0; why = "store did not retire"; }
        if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report B"; }
        t25_reap(&p2proc); it_close(&p2cmd);
    }
    t25_tgt_reap(&g2);
    it_quiesce_reaper();
    if (ok && (!t25_frame_word(fr_h, &word, 0) || word != T25_WMARK)) {
        ok = 0; why = "write not visible in frame";
    }

    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 2u)  { ok = 0; why = "resume count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T183"); else it_fail("T183", why);
}

/* ── T184: unauthorized pager cannot resolve a foreign fault ────────────────
 * A pager fully authorized for target B holds only under-privileged caps for
 * victim A (proc READ-only, VSpace READ-only).  While A's fault is pending
 * the pager attempts the whole battery — resume, kill, map, unmap, resolve
 * A's task through B's cap, read A's fault through B's cap, forge device
 * authority — and every attempt is denied with no fallback, no partial
 * mapping and no record disturbance.  READ on A's cap grants exactly
 * information, never resolution.  Invariants: P1, P3, P4, P5, P22. */
void test_t184(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "unauthorized pager";

    struct t25_tgt va, gb;                    /* victim A, authorized target B */
    if (!t25_tgt_spawn(&va, &why)) { it_fail("T184", why); return; }
    if (!t25_tgt_spawn(&gb, &why)) {
        (void)it_kill((long)va.proc);
        t25_tgt_reap(&va); it_fail("T184", why); return;
    }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }

    /* Under-privileged victim caps (DUPLICATE only so they can be minted). */
    long apro = ok ? it_cs_reduce((long)va.proc, RIGHT_READ | RIGHT_DUPLICATE) : -1;
    long avso = ok ? it_cs_reduce((long)va.vs,   RIGHT_READ | RIGHT_DUPLICATE) : -1;
    handle_id_t apro_h = (apro >= 0) ? (handle_id_t)apro : HANDLE_INVALID;
    handle_id_t avso_h = (avso >= 0) ? (handle_id_t)avso : HANDLE_INVALID;
    if (ok && (apro < 0 || avso < 0)) { ok = 0; why = "victim dups"; }

    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok) {
        struct svc_mint x[2] = { 0 };
        x[0].slot = LP_PGR_SLOT_XPROC; IT_MINT_SRC(x[0], apro_h); x[0].rights = RIGHT_READ; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_XVS;   IT_MINT_SRC(x[1], avso_h); x[1].rights = RIGHT_READ; x[1].badge = 0;
        if (t25_pager_spawn(&gb, fr_h, RIGHT_READ | RIGHT_WRITE, x, 2u,
                            &pcmd, &pproc) != 0) { ok = 0; why = "pager spawn"; }
    }

    /* Victim faults; the pager runs the unauthorized battery. */
    struct it_fault fa;
    if (ok && it_lp_cmd_va(va.cmd, LP_CMD_FAULT_READ, T25_VA_C) != 0) { ok = 0; why = "victim fault"; }
    if (ok && !t25_wait_fault(&va, &fa)) { ok = 0; why = "victim fault pending"; }
    if (ok && t25_xprobe(pcmd, fa.task_id, T25_VA_C, fa.seq) != 0) { ok = 0; why = "xprobe cmd"; }
    if (ok) {
        long breach = it_lp_wait_exit(pproc);
        if (breach != 0) { ok = 0; why = "unauthorized op leaked"; }
    }

    /* The victim's fault is untouched: same generation, still suspended. */
    struct it_fault fa2;
    if (ok && (it_fault_info(va.fault_leaf, &fa2) != 0 || fa2.seq != fa.seq ||
               fa2.task_id != fa.task_id || fa2.cr2 != fa.cr2)) { ok = 0; why = "record disturbed"; }
    if (ok && it_invoke0(it_child_tcb((long)va.proc), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "victim not suspended"; }

    /*
     * Ledger A-22: the rights split is SHARPER, not gone.
     *
     * It used to be READ-on-the-thread reads the fault record and
     * WRITE-on-the-thread resolves it — two rights on one capability, so
     * anyone who could read a fault held an object that also names everything
     * else the thread can be made to do.  Reading a fault is now RECEIVING a
     * message, which needs RIGHT_READ on the ENDPOINT, and resolving it is
     * spending a reply capability that does not exist until the fault does.
     *
     * A rights-reduced THREAD capability now buys neither.  It cannot read the
     * fault (there is no syscall that reads one), it cannot re-aim where the
     * thread's faults go (that is a WRITE), and it cannot resume anything.
     */
    long atcb = ok ? it_cs_reduce(it_child_tcb(va.proc), RIGHT_READ) : -1;
    if (ok && atcb < 0) { ok = 0; why = "victim tcb dup"; }
    if (ok) {
        uint8_t fb[FAULT_MSG_LEN];
        if (it_sys2(SYS_TCB_FAULT_INFO, atcb, (long)(uintptr_t)fb)
            != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "read cap info denied";
        }
    }
    if (ok) {
        /* A thread capability is not reply authority, whatever its rights. */
        struct IrisMsg rm;
        it_iris_msg_zero(&rm);
        if (it_invoke1(atcb, INV_REPLY_SEND, (long)(uintptr_t)&rm)
            != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "read tcb resumed"; }
    }
    if (ok && it_invoke(atcb, INV_TCB_SET_FAULT_HANDLER, (long)va.notif, 0, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read cap registered"; }

    /* Proper authority resolves. */
    if (ok && t25_resume_seq(&va, fa.task_id, fa.seq, 1) != 0) { ok = 0; why = "proper resolve"; }
    if (ok && it_lp_wait_exit(va.proc) != 0) { ok = 0; why = "victim exit"; }

    if (it_kill((long)gb.proc) != 0 && ok) { ok = 0; why = "kill B"; }
    if (ok && it_lp_wait_exit(gb.proc) != 0) { ok = 0; why = "B exit"; }
    it_close(&apro_h); it_close(&avso_h);
    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&va); t25_tgt_reap(&gb);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T184"); else it_fail("T184", why);
}

/* ── T185: a spent answer cannot answer the next fault (A-22) ───────────────
 *
 * This test used to be about fault GENERATIONS: a counter the kernel kept per
 * thread, echoed back by the handler, so a stale resolution could be told from
 * a current one.  The counter existed because "may resume this thread" was
 * RIGHT_WRITE on a TCB capability — permanent, copyable, and just as valid for
 * the next fault as for the one it was handed for.  A number had to be bolted
 * on to make it one-shot.
 *
 * A reply capability is one-shot by construction, so the claim is the same and
 * the mechanism is gone.  A fresh target's first fault is generation 1 and
 * binds reply R1; resuming without fixing anything re-faults the SAME
 * instruction as generation 2 and binds a DIFFERENT reply, R2.  A COPY of R1,
 * taken while it was still live, then answers nothing at all — not F1, which
 * it already answered, and not F2, which it was never bound to.  That is the
 * property the generation number was approximating, and holding a copy is the
 * strongest form of the attack: the handler kept the old authority on purpose.
 *
 * The generation is still in the record, and still increments, because a
 * handler correlating logs wants it — it just does not GATE anything.
 * Invariants: P9, P10, P12, P13.  */
void test_t185(void) {
    uint32_t f0[6], f1[6], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_sched_ext5(f0);
    const char *why = "stale generation";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T185", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    long tvs_c = (long)g.vs;   /* dual resolver: the VSpace HANDLE works */
    if (ok && fr < 0)    { ok = 0; why = "frame retype"; }
    if (ok) { word = 0; if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame zero"; } }

    struct it_fault fx1, fx2;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T25_VA_D) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(&g, &fx1)) { ok = 0; why = "F1 pending"; }
    if (ok && fx1.seq != 1u) { ok = 0; why = "first gen not 1"; }
    if (ok && (fx1.vector != 14u || fx1.cr2 != T25_VA_D ||
               fx1.error != (PF_ERR_W | PF_ERR_U))) { ok = 0; why = "F1 info"; }

    /* Keep a COPY of F1's reply authority before spending it — the attack this
     * test exists to refuse. */
    long r1dup = ok ? it_cs_reduce(IT_FAULT_CPTR(g.fault_leaf),
                                   RIGHT_READ | RIGHT_WRITE) : -1;
    if (ok && r1dup < 0) { ok = 0; why = "reply dup"; }

    /* Clean refault: resume without resolving → generation 2, same site. */
    if (ok && t25_resume_seq(&g, fx1.task_id, fx1.seq, 0) != 0) { ok = 0; why = "resume F1"; }
    if (ok && !t25_wait_refault(&g, fx1.seq, &fx2)) { ok = 0; why = "no refault"; }
    if (ok && (fx2.seq != fx1.seq + 1u || fx2.rip != fx1.rip ||
               fx2.cr2 != fx1.cr2 || fx2.task_id != fx1.task_id)) { ok = 0; why = "F2 identity"; }

    /* The kept copy of R1 answers nothing: F1 is already answered and F2 is
     * bound to a different object entirely. */
    if (ok && r1dup >= 0) {
        struct IrisMsg rm;
        it_iris_msg_zero(&rm);
        if (it_invoke1(r1dup, INV_REPLY_SEND, (long)(uintptr_t)&rm)
            != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale resume accepted"; }
    }
    /* ...and destroying it resolves nothing either — a fault is ended by the
     * object its OWN call is bound to, not by any reply that ever existed. */
    if (ok && r1dup >= 0) {
        it_slot_delete((uint32_t)r1dup);
        r1dup = -1;
        it_quiesce_reaper();
        if (it_invoke0(it_child_tcb((long)g.proc), INV_TCB_EXIT_CODE)
            != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "stale kill accepted"; }
    }
    struct it_fault fx3;
    if (ok && (it_fault_info(g.fault_leaf, &fx3) != 0 || fx3.seq != fx2.seq)) {
        ok = 0; why = "record disturbed by stale ops";
    }

    /* Resolve the CURRENT generation for real: map writable + seq-resume. */
    if (ok && it_invoke((long)fr_h, INV_FRAME_MAP, tvs_c, (long)T25_VA_D, 1) != 0) {
        ok = 0; why = "map at fault";
    }
    if (ok && t25_resume_seq(&g, fx2.task_id, fx2.seq, 0) != 0) { ok = 0; why = "resume F2"; }
    if (ok && it_lp_wait_exit(g.proc) != LP_EXIT_MARKER) { ok = 0; why = "target completion"; }

    /* Late-but-correct is still late: the record did not outlive resolution. */
    if (ok && t25_resume_seq(&g, fx2.task_id, fx2.seq, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume accepted"; }
    if (ok && it_fault_info(g.fault_leaf, &fx3) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }
    it_quiesce_reaper();
    if (ok && (!t25_frame_word(fr_h, &word, 0) || word != T25_WMARK)) {
        ok = 0; why = "store did not land";
    }

    if (r1dup >= 0) it_slot_delete((uint32_t)r1dup);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_CLEAN]   != f0[IT_S5_CLEAN] + 2u)   { ok = 0; why = "cleanup count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T185"); else it_fail("T185", why);
}

/* ── T186: pager death while the target's fault is pending ──────────────────
 * The pager dies holding responsibility (blocked, never resolving).  The
 * contract (same as Phase 20 handler-death, now under supervision): the target
 * is NOT a zombie — it stays suspended with its record and generation intact,
 * observable by any READ holder, resolvable by any proper authority.  The
 * supervisor restarts the pager with the SAME declared manifest; the new
 * generation consumes the still-pending delivery signal and completes the
 * map+resume.  Nothing leaks per generation.  Invariants: P9, P14(neg), P15,
 * P16, P21. */
void test_t186(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager death pending";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T186", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }
    if (ok) { word = T25_PATTERN; if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame fill"; } }

    /* Gen 1: spawned in charge, never commanded — dies blocked. */
    handle_id_t p1cmd = HANDLE_INVALID, p1proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &p1cmd, &p1proc) != 0) {
        ok = 0; why = "pager1 spawn";
    }
    uint32_t d0 = t25_delivered_now();
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    /* A-22: OBSERVE the delivery, do not take it — the fault is for the pager
     * to serve, and receiving it here would be taking it away from them. */
    if (ok && !t25_wait_delivered(d0)) { ok = 0; why = "fault pending"; }
    if (ok && it_kill((long)p1proc) != 0) { ok = 0; why = "kill pager1"; }
    if (ok && it_lp_wait_exit(p1proc) != 0) { ok = 0; why = "pager1 exit"; }
    it_quiesce_reaper();

    /* No zombie: suspended-alive, record and generation intact. */
    if (ok && it_invoke0(it_child_tcb((long)g.proc), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "target not suspended"; }
    /* A-22: that the fault SURVIVED its handler's death is proved by the next
     * pager serving it, below — which is a stronger claim than reading a
     * record back, and the only one available now that taking delivery is the
     * same act as reading it. */
    /* The dead pager's endpoint has no phantom receiver. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.label = 0x186;
        if (it_invoke1((long)p1cmd, INV_EP_NB_SEND, (long)&m)
            != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "phantom pager receiver"; }
    }
    t25_reap(&p1proc); it_close(&p1cmd);

    /* Gen 2: same declared manifest, nothing more — and it finishes the job
     * (the delivery signal was never lost). */
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &p2cmd, &p2proc) != 0) {
        ok = 0; why = "pager2 spawn";
    }
    if (ok && t25_serve(p2cmd, 1u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "serve"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "target completion"; }
    if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager2 report"; }

    t25_reap(&p2proc); it_close(&p2cmd);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T186"); else it_fail("T186", why);
}

/* ── T187: target death during pager resolution ─────────────────────────────
 * The supervisor kills the target between fault delivery and the pager's
 * completion.  Every late step that names the DEAD THREAD fails clean:
 * seq-checked and legacy resume are NOT_FOUND, the record reports WOULD_BLOCK.
 * The never-installed frame stays clean and reusable; VSpace/mapping books
 * return to baseline once the stale caps are dropped.
 *
 * Stage 7-proc changed one of these, deliberately.  Mapping into the target's
 * address space used to be BAD_HANDLE, "the VSpace was invalidated with the
 * process".  An address space is not invalidated by a thread dying any more —
 * it is invalidated when the last CAPABILITY to it goes, which is its close
 * hook — so a holder that kept a capability across the death keeps a usable
 * address space with nothing running in it.  That is seL4's shape (a page
 * directory outlives its threads and can still be mapped into), and it is what
 * this test now asserts: the late map SUCCEEDS, and the books still come back
 * to baseline once the capability is dropped, which is the property that
 * mattered.  Invariants: P12, P14, P18, P19, P23. */
void test_t187(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "target death mid-resolution";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T187", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    long tvs_c = (long)g.vs;   /* dual resolver: the VSpace HANDLE works */
    if (ok && fr < 0)    { ok = 0; why = "frame retype"; }

    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "fault pending"; }

    /* Mid-resolution kill. */
    if (ok && it_kill((long)g.proc) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }
    it_quiesce_reaper();

    /* Late completion fails clean at every step. */
    /* Stage 7-proc: the address space outlives its threads while a capability
     * to it lives, so this succeeds — and is undone below so the baseline
     * still has to hold. */
    if (ok && it_invoke((long)fr_h, INV_FRAME_MAP, tvs_c, (long)T25_VA_A, 0) != 0) {
        ok = 0; why = "late map into a live address space refused";
    }
    if (ok && it_invoke2((long)fr_h, INV_FRAME_UNMAP, tvs_c, (long)T25_VA_A) != 0) {
        ok = 0; why = "late unmap refused";
    }
    if (ok && t25_resume_seq(&g, f.task_id, f.seq, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late seq-resume accepted"; }
    if (ok && it_fault_resume(g.fault_leaf)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "late resume accepted"; }
    if (ok && it_fault_info(g.fault_leaf, &f) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived death";
    }

    /* The frame was never installed: still clean, still usable. */
    if (ok) {
        word = T25_PATTERN;
        if (!t25_frame_word(fr_h, &word, 1) ||
            !t25_frame_word(fr_h, &word, 0) || word != T25_PATTERN) {
            ok = 0; why = "frame unusable after target death";
        }
    }

    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T187"); else it_fail("T187", why);
}

/* ── T188: pager rights and PTE policy ──────────────────────────────────────
 * PTE rights are the MEET of every capability involved, never more.  A
 * read-only frame cap cannot install a writable PTE; a read-only VSpace
 * authority cannot install anything; W^X, kernel-range, out-of-window and
 * unaligned VAs are INVALID_ARG; a denied map leaves NO partial PTE; an
 * occupied VA is BUSY.  The decisive check is architectural: a page mapped
 * READ-ONLY into the target through a fully-writable frame cap still refuses
 * the target's store with err P|W|U — the PTE carries the MAPPING's rights,
 * not the cap's ceiling.  Late unmap after target death is BAD_HANDLE.
 * Invariants: P5, P6, P7, P8, P10, P19. */
void test_t188(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "rights/PTE policy";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T188", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    long fro = (fr >= 0) ? it_cs_reduce(fr, RIGHT_READ) : -1;
    handle_id_t fro_h = (fro >= 0) ? (handle_id_t)fro : HANDLE_INVALID;
    long tvs_c  = (long)g.vs;  /* dual resolver: the VSpace HANDLE works */
    long tvs_ro = it_cs_reduce((long)g.vs, RIGHT_READ);
    handle_id_t tvs_ro_h = (tvs_ro >= 0) ? (handle_id_t)tvs_ro : HANDLE_INVALID;
    if (ok && (fr < 0 || fro < 0)) { ok = 0; why = "frame caps"; }
    if (ok && tvs_ro < 0)          { ok = 0; why = "tvs ro dup"; }

    /* Rights monotonicity and namespace limits — all denied, no fallback. */
    if (ok && it_invoke(fro, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 1)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO frame mapped W"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_ro, (long)T25_VA_B, 0)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "RO vspace installed"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 3)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "W^X accepted"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, (long)0xFFFF800000000000ULL, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA accepted"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, 0x1000L, 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "low VA accepted"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, (long)(T25_VA_B | 0x123u), 0)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "unaligned accepted"; }
    /* No partial PTE from any denial. */
    if (ok && it_invoke2(fr, INV_FRAME_UNMAP, tvs_c, (long)T25_VA_B)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "partial PTE"; }

    /* Occupied-VA and W^X-compliant exec map contracts. */
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 0) != 0) {
        ok = 0; why = "RO map";
    }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 0)
              != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied VA remapped"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, tvs_c, (long)T25_VA_C, 2) != 0) {
        ok = 0; why = "r-x map denied";
    }
    if (ok && it_invoke2(fr, INV_FRAME_UNMAP, tvs_c, (long)T25_VA_C) != 0) {
        ok = 0; why = "r-x unmap";
    }

    /* Architectural proof: the RO PTE (installed via the RW frame cap) still
     * refuses the target's store — err = P|W|U at exactly that VA. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T25_VA_B) != 0) { ok = 0; why = "write cmd"; }
    if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "no wp fault"; }
    if (ok && (f.vector != 14u || f.cr2 != T25_VA_B ||
               f.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U))) { ok = 0; why = "wp err bits"; }
    if (ok && t25_resume_seq(&g, f.task_id, f.seq, 1) != 0) { ok = 0; why = "seq kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }
    it_quiesce_reaper();

    /* The target died with the mapping installed.  Stage 7-proc: the address
     * space is still valid — we hold a capability to it — so the sweep that
     * matters is the FRAME's: its mapped_count came back, which the reuse
     * check below proves.  A late unmap therefore finds no such mapping rather
     * than a dead address space. */
    /* The mapping is still installed — the address space is alive and nothing
     * swept it, because nothing died that owned it.  Unmapping it is the
     * holder's job and it succeeds; what the frame's reuse check below proves
     * is that mapped_count came back either way. */
    if (ok && it_invoke2(fr, INV_FRAME_UNMAP, tvs_c, (long)T25_VA_B) != 0) {
        ok = 0; why = "late unmap";
    }
    /* Frame reusable, mapped_count back at zero. */
    if (ok) {
        uint32_t word = T25_PATTERN;
        if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame not clean"; }
    }

    it_close(&fro_h); it_close(&tvs_ro_h);
    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T188"); else it_fail("T188", why);
}
void test_t189(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "restart least authority";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T189", why); return; }
    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { ok = 0; why = "frame retype"; }
    if (ok) { word = T25_PATTERN; if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame fill"; } }

    uint32_t d0 = t25_delivered_now();
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
    /* A-22: observed, not taken — every generation below is meant to be able
     * to serve it. */
    if (ok && !t25_wait_delivered(d0)) { ok = 0; why = "fault pending"; }

    /* Supervision loop: each generation crashes before resolving; the budget
     * (T189_LIMIT) bounds the loop and flips the service to degraded. */
    uint32_t restart_count = 0u, generation = 0u;
    int degraded = 0;
    while (ok && !degraded) {
        handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
        if (t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
            ok = 0; why = "gen spawn"; break;
        }
        generation++;
        it_settle(1);
        if (it_kill((long)pproc) != 0) { ok = 0; why = "gen kill"; }
        if (ok && it_lp_wait_exit(pproc) != 0) { ok = 0; why = "gen exit"; }
        t25_reap(&pproc); it_close(&pcmd);
        it_quiesce_reaper();
        if (restart_count < T189_LIMIT) restart_count++;
        else degraded = 1;
    }
    if (ok && restart_count != T189_LIMIT) { ok = 0; why = "wrong restart count"; }
    if (ok && generation != T189_LIMIT + 1u) { ok = 0; why = "generation mismatch"; }

    /* Degraded or not, the fault never became a zombie: still suspended, and
     * still resolvable — which the surviving generation below proves by
     * serving it.  A-22: a supervisor cannot read the record of a fault it
     * means somebody else to answer, so what it checks is the thread. */
    if (ok && it_invoke0(it_child_tcb((long)g.proc), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "fault lost across pager generations";
    }

    /* A fresh instance of the same declaration carries EXACTLY the manifest —
     * restart amplified nothing. */
    if (ok) {
        struct svc_mint x[4] = { 0 };
        x[0].slot = LP_PGR_SLOT_TPROC; IT_MINT_SRC(x[0], g.proc);  x[0].rights = RIGHT_READ | RIGHT_MANAGE; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_TVS;   IT_MINT_SRC(x[1], g.vs);    x[1].rights = RIGHT_WRITE;               x[1].badge = 0;
        x[2].slot = LP_PGR_SLOT_FRAME; IT_MINT_SRC(x[2], fr_h);    x[2].rights = RIGHT_READ;                x[2].badge = 0;
        x[3].slot = LP_PGR_SLOT_FAULT_EP; IT_MINT_SRC(x[3], g.notif); x[3].rights = RIGHT_READ;                x[3].badge = 0;
        long rep = it_lp_report_slots(x, 4u);
        uint32_t expect = (1u << LP_CPTR_CMD_EP)    | (1u << LP_PGR_SLOT_TPROC) |
                          (1u << LP_PGR_SLOT_TVS)   | (1u << LP_PGR_SLOT_FRAME) |
                          /* No fault mailbox: this probe declares its own
                           * four-capability manifest and resolves nothing, so
                           * it is handed no mailbox to resolve WITH. */
                          (1u << LP_PGR_SLOT_FAULT_EP);
        if (rep < 0 || (uint32_t)rep != expect) { ok = 0; why = "post-restart manifest"; }
    }

    /* The serving generation finishes the original resolution. */
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, fr_h, RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "server spawn";
    }
    if (ok && t25_serve(pcmd, 1u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "serve"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "target completion"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "server report"; }
    t25_reap(&pproc); it_close(&pcmd);

    t25_tgt_reap(&g);
    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T189"); else it_fail("T189", why);
}
static uint32_t t190_rnd(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
void test_t190(void) {
    uint32_t rng = T190_SEED, word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager stress";
    uint32_t round = 0u, op = 0u;

    long fr = it_frame_create_slot(IT_UT, 4096);
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (fr < 0) { it_fail("T190", "frame retype"); return; }

    /* The frame is held for the whole test, so the PER-ROUND balance is
     * against a baseline that already accounts for it; the final check below
     * (after fr_h is closed) uses the pre-suite `b`. */
    it_quiesce_reaper();
    struct it_snap br = it_snap_take();
    if (ok && !br.ok) { it_close(&fr_h); it_fail("T190", "round baseline"); return; }

    for (round = 0; ok && round < T190_ROUNDS; round++) {
        op = t190_rnd(&rng) % 6u;
        word = T25_PATTERN;
        if (!t25_frame_word(fr_h, &word, 1)) { ok = 0; why = "frame fill"; break; }

        struct t25_tgt g1, g2;
        if (!t25_tgt_spawn(&g1, &why)) { ok = 0; break; }
        if (!t25_tgt_spawn(&g2, &why)) {
            (void)it_kill((long)g1.proc);
            t25_tgt_reap(&g1); ok = 0; break;
        }

        /* Two concurrent pending faults, every round. */
        struct it_fault f1, f2;
        uint32_t d0 = t25_delivered_now();
        if (it_lp_cmd_va(g1.cmd, LP_CMD_FAULT_READ,  T25_VA_A) != 0 ||
            it_lp_cmd_va(g2.cmd, LP_CMD_FAULT_WRITE, T25_VA_B) != 0) { ok = 0; why = "fault cmds"; }
        if (ok && !t25_wait_delivered(d0 + 1u)) { ok = 0; why = "faults pending"; }
        /*
         * A-22: g2 is always the SUPERVISOR's to answer, so its fault is taken
         * here.  g1's is taken only in the rounds where the supervisor answers
         * it too — in ops 0 and 1 an external pager does, and a fault this
         * thread received is a fault that pager can never see.
         */
        if (ok && !t25_wait_fault(&g2, &f2)) { ok = 0; why = "faults pending"; }
        if (ok && op >= 2u && !t25_wait_fault(&g1, &f1)) { ok = 0; why = "faults pending"; }
        else if (ok && op < 2u) { f1.task_id = 0; f1.seq = 0; }

        switch (ok ? (int)op : -1) {
        case 0: {
            /* External pager resolves g1 by map+resume; supervisor seq-kills g2. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g1, fr_h, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op0 pager"; break; }
            if (t25_serve(pc, 1u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "op0 serve"; }
            if (ok && it_lp_wait_exit(g1.proc) !=
                      (long)(LP_EXIT_MARKER ^ (T25_PATTERN & 0xFFu))) { ok = 0; why = "op0 g1"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op0 pager report"; }
            if (ok && t25_resume_seq(&g2, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "op0 g2 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op0 g2 exit"; }
            t25_reap(&pp); it_close(&pc);
            break;
        }
        case 1: {
            /* External pager seq-kills g1; stale-generation replay on g2 must
             * fail before the proper kill lands. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g1, fr_h, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op1 pager"; break; }
            if (t25_serve(pc, 3u, 1u, 0, 0, T25_VA_A) != 0) { ok = 0; why = "op1 serve"; }
            if (ok && it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op1 g1"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op1 pager report"; }
            /* A-22: there is no "bogus generation" to try any more — an
             * answer is a capability, so the only wrong one is one you do not
             * hold, which T185 and T184 assert directly. */
            if (ok && t25_resume_seq(&g2, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "op1 g2 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op1 g2 exit"; }
            t25_reap(&pp); it_close(&pc);
            break;
        }
        case 2: {
            /* Pager dies before serving; the supervisor takes over: seq-kill
             * g1, map+seq-resume g2 (its store retires into the frame). */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g1, fr_h, RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op2 pager"; break; }
            if (it_kill((long)pp) != 0 ||
                it_lp_wait_exit(pp) != 0) { ok = 0; why = "op2 pager death"; }
            t25_reap(&pp); it_close(&pc);
            long tvs_c = (long)g2.vs;
            if (ok && t25_resume_seq(&g1, f1.task_id, f1.seq, 1) != 0) { ok = 0; why = "op2 g1 kill"; }
            if (ok && it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op2 g1 exit"; }
            if (ok && it_invoke((long)fr_h, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 1) != 0) { ok = 0; why = "op2 map"; }
            if (ok && t25_resume_seq(&g2, f2.task_id, f2.seq, 0) != 0) { ok = 0; why = "op2 resume"; }
            if (ok && it_lp_wait_exit(g2.proc) != LP_EXIT_MARKER) { ok = 0; why = "op2 g2 exit"; }
            break;
        }
        case 3: {
            /* Target death mid-fault + refault path on the survivor: resume
             * g2 without map → new generation at the same site → old
             * generation refused → proper seq-kill of the NEW generation. */
            if (it_kill((long)g1.proc) != 0 ||
                it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op3 g1 death"; break; }
            if (t25_resume_seq(&g1, f1.task_id, f1.seq, 0)
                != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op3 late resume"; }
            struct it_fault f2b;
            if (ok && t25_resume_seq(&g2, f2.task_id, f2.seq, 0) != 0) { ok = 0; why = "op3 refault resume"; }
            if (ok && !t25_wait_refault(&g2, f2.seq, &f2b)) { ok = 0; why = "op3 no refault"; }
            if (ok && f2b.seq != f2.seq + 1u) { ok = 0; why = "op3 gen"; }
            /* The first answer was spent on the first fault; the second fault
             * carries its own, and that is what ends it. */
            if (ok && t25_resume_seq(&g2, f2b.task_id, f2b.seq, 1) != 0) { ok = 0; why = "op3 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op3 g2 exit"; }
            break;
        }
        case 4: {
            /* Unauthorized caps under load: READ-only proc cap cannot
             * resolve; READ-only vspace mint cannot install.  Then proper
             * seq-kills. */
            long rp = it_cs_reduce((long)g1.proc, RIGHT_READ);
            handle_id_t rp_h = (rp >= 0) ? (handle_id_t)rp : HANDLE_INVALID;
            long rvs = it_cs_reduce((long)g1.vs, RIGHT_READ);
            handle_id_t rvs_h = (rvs >= 0) ? (handle_id_t)rvs : HANDLE_INVALID;
            if (rp < 0 || rvs < 0) { ok = 0; why = "op4 caps"; }
            /* A-22: a READ-only copy of the very reply that would resume g1
             * answers nothing — the rights on the ANSWER are what gate it. */
            if (ok) {
                struct IrisMsg rm;
                it_iris_msg_zero(&rm);
                long rr = it_cs_reduce(IT_FAULT_CPTR(g1.fault_leaf), RIGHT_READ);
                if (rr < 0) { ok = 0; why = "op4 caps"; }
                else if (it_invoke1(rr, INV_REPLY_SEND, (long)(uintptr_t)&rm)
                         != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro resume"; }
            }
            if (ok && it_invoke((long)fr_h, INV_FRAME_MAP, rvs, (long)T25_VA_A, 0)
                      != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro map"; }
            if (ok && it_invoke2((long)fr_h, INV_FRAME_UNMAP, rvs, (long)T25_VA_A)
                      != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro unmap"; }
            it_close(&rp_h); it_close(&rvs_h);
            if (ok && t25_resume_seq(&g1, f1.task_id, f1.seq, 1) != 0) { ok = 0; why = "op4 g1 kill"; }
            if (ok && t25_resume_seq(&g2, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "op4 g2 kill"; }
            if (ok && (it_lp_wait_exit(g1.proc) != 0 ||
                       it_lp_wait_exit(g2.proc) != 0)) { ok = 0; why = "op4 exits"; }
            break;
        }
        case 5: {
            /* Occupied VA + RO-PTE enforcement under load: install the frame
             * read-only at g2's fault VA; a duplicate install is BUSY; the
             * resumed store now write-protection-faults (P|W|U) as a NEW
             * generation, then dies by it. */
            long tvs_c = (long)g2.vs;
            if (it_invoke((long)fr_h, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 0) != 0) { ok = 0; why = "op5 map"; }
            if (ok && it_invoke((long)fr_h, INV_FRAME_MAP, tvs_c, (long)T25_VA_B, 0)
                      != (long)IRIS_ERR_BUSY) { ok = 0; why = "op5 busy"; }
            struct it_fault f2b;
            if (ok && t25_resume_seq(&g2, f2.task_id, f2.seq, 0) != 0) { ok = 0; why = "op5 resume"; }
            if (ok && !t25_wait_refault(&g2, f2.seq, &f2b)) { ok = 0; why = "op5 no wp fault"; }
            if (ok && f2b.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U)) { ok = 0; why = "op5 err bits"; }
            if (ok && t25_resume_seq(&g2, f2b.task_id, f2b.seq, 1) != 0) { ok = 0; why = "op5 g2 kill"; }
            if (ok && it_lp_wait_exit(g2.proc) != 0) { ok = 0; why = "op5 g2 exit"; }
            if (ok && t25_resume_seq(&g1, f1.task_id, f1.seq, 1) != 0) { ok = 0; why = "op5 g1 kill"; }
            if (ok && it_lp_wait_exit(g1.proc) != 0) { ok = 0; why = "op5 g1 exit"; }
            break;
        }
        default:
            break;
        }

        /* Round postconditions: no pending fault, no zombie, live books at
         * the pre-suite baseline. */
        if (ok && it_fault_info(g1.fault_leaf, &f1) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "g1 record"; }
        if (ok && it_fault_info(g2.fault_leaf, &f2) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "g2 record"; }
        t25_tgt_reap(&g1); t25_tgt_reap(&g2);
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&br, &r, &why)) ok = 0;
        }
    }

    it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T190");
    else { it_fz_note("T190", T190_SEED, round, op); it_fail("T190", why); }
}

/* Live FRAME count (SYS_SCHED_INFO ext3, offset 116 — the Phase 18 per-type
 * authority word).  Ledger D-5: this was it_vmo_live, reading the KVmo gauge
 * at offset 132.  The question it answers is unchanged — did the memory this
 * test made come back — and it is asked of the object that now holds it.
 * Returns -1 on failure. */
long it_frame_live(void) {
    uint8_t buf[136];
    long r = it_invoke2((long)IRIS_CPTR_DEBUG_CONTROL, INV_BOOT_SCHED_INFO, (long)(uintptr_t)buf, 136);
    if (r != 0) return -1;
    return (long)((uint32_t)buf[116] | ((uint32_t)buf[117] << 8) |
                  ((uint32_t)buf[118] << 16) | ((uint32_t)buf[119] << 24));
}

static long t26_grant_create(void) {
    uint32_t span = IT_OBJ_SLOT_SPAN - IT_OBJ_POOL_FIRST - T26_GRANT_PAGES;
    uint32_t base = IT_OBJ_POOL_FIRST +
                    (__atomic_fetch_add(&g_it_obj_slot_next, T26_GRANT_PAGES,
                                        __ATOMIC_RELAXED) % span);
    for (uint32_t i = 0; i < T26_GRANT_PAGES; i++) {
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)(base + i));
        if (it_invoke((long)IRIS_CPTR_TEST_UNTYPED, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)), (long)((uint64_t)IT_OBJ_CNODE_SLOT |
                           ((uint64_t)(base + i) << 32)), 4096) != 0) {
            while (i-- > 0)
                (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)(base + i));
            return -1;
        }
    }
    return (long)IT_OBJ_CPTR(base);
}

/* A grant is closed page by page — the run is the object. */
void t26_grant_close(handle_id_t *g) {
    if (!g || *g == HANDLE_INVALID) return;
    for (uint32_t i = 0; i < T26_GRANT_PAGES; i++)
        it_slot_delete((uint32_t)T26_PAGE(*g, i));
    *g = HANDLE_INVALID;
}

handle_id_t t26_grant(void) {
    long v = t26_grant_create();
    return (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
}

/* Read/write word 0 of one granted page, through the suite's OWN address space
 * (IT_VS).  Lets the supervisor prep and inspect what a pager will hand over. */
int t26_page_word(handle_id_t page, uint32_t *val, int write) {
    if (!it_setup_self_vspace()) return -1;
    long r = it_invoke((long)page, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, write ? 1L : 0L);
    if (r != 0) return (int)r;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T26_SELF_VA;
    if (write) *p = *val; else *val = *p;
    return (int)it_invoke2((long)page, INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA);
}

/* ── T191: granted-page authority ───────────────────────────────────────────
 * Ledger D-5.  Its subject was a KVmo — size contract, offset gating, a live
 * count of memory objects.  What survives is everything that was about
 * AUTHORITY, restated on the capability that now carries it: a granted page is
 * a frame, its size is stable and READ-gated, mapping it is READ-gated with
 * RIGHT_WRITE additionally for a writable PTE, a wrong type in either slot is
 * WRONG_TYPE, a reduced-rights derivation cannot regain what it dropped, and a
 * released capability fails clean.
 *
 * What did NOT survive is the size CONTRACT — "the region is as big as you
 * asked for" — because a granted page is one page by construction and there is
 * no second number to disagree with.  The Stage 4 rule: a property that only
 * existed because of the mechanism dies with it; one that outlives it moves.
 * Invariants: M1, M2, M9, M10, M11, M12, M23. */
void test_t191(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long flive0 = it_frame_live();
    int ok = b.ok && flive0 >= 0;
    const char *why = "granted-page authority";

    handle_id_t g = t26_grant();
    if (g == HANDLE_INVALID) { it_fail("T191", "grant"); return; }
    /* A grant costs exactly one frame per page — no hidden region object. */
    if (ok && it_frame_live() != flive0 + (long)T26_GRANT_PAGES) {
        ok = 0; why = "grant is not its pages"; }

    /* Every page of the run is a real, separate, one-page capability. */
    for (uint32_t i = 0; ok && i < T26_GRANT_PAGES; i++)
        if (it_invoke0((long)T26_PAGE(g, i), INV_FRAME_SIZE) != 4096L) {
            ok = 0; why = "page size"; }
    if (ok && it_invoke0((long)g, INV_FRAME_SIZE) != 4096L) { ok = 0; why = "size unstable"; }

    /* A READ-only derivation cannot map writable (rights monotonicity). */
    long ro = it_cs_reduce((long)g, RIGHT_READ | RIGHT_DUPLICATE);
    handle_id_t ro_h = (ro >= 0) ? (handle_id_t)ro : HANDLE_INVALID;
    if (ok && ro < 0) { ok = 0; why = "ro dup"; }
    if (ok && it_setup_self_vspace()) {
        if (it_invoke(ro, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 1L)
            != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro mapped writable"; }
        /* READ-only can still map read-only. */
        if (ok && it_invoke(ro, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L)
            != 0) { ok = 0; why = "ro map denied"; }
        if (ok && it_invoke2(ro, INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA) != 0) {
            ok = 0; why = "ro unmap"; }
    } else if (ok) { ok = 0; why = "self vspace"; }

    /* Wrong-type in the frame slot (a notification) and the VSpace slot. */
    long n = it_notify_create();
    handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
    if (ok && n < 0) { ok = 0; why = "notif"; }
    if (ok && it_invoke(n, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "frame wrong-type"; }
    if (ok && it_invoke((long)g, INV_FRAME_MAP, n, (long)T26_SELF_VA, 0L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "vspace wrong-type"; }

    /* Stale (deleted) page cap fails clean. */
    it_close(&ro_h);
    if (ok && it_invoke(ro, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L)
              >= 0) { ok = 0; why = "stale cap mapped"; }
    if (ok && it_invoke0(ro, INV_FRAME_SIZE) >= 0) { ok = 0; why = "stale size"; }

    it_close(&n_h);
    t26_grant_close(&g);
    it_quiesce_reaper();
    if (ok && it_frame_live() != flive0) { ok = 0; why = "frame live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T191"); else it_fail("T191", why);
}

/* ── T192: map VA and flag validation ───────────────────────────────────────
 * Every VA and flag is validated before a page is touched: a kernel VA is
 * INVALID_ARG; an unaligned VA is INVALID_ARG; W^X is INVALID_ARG; a VA that
 * already holds a mapping is BUSY.  None of them installs a PTE, which the
 * valid map afterwards proves.
 *
 * Ledger D-5: three of the original seven cases were OFFSETS — unaligned,
 * == size, > size — and they were the kernel range-checking a caller's index
 * into a region it owned.  There is no index: the page is the capability, and
 * an offset that named a page you were not granted is a slot you do not hold,
 * which T191 already covers as "stale cap".  Invariants: M9, M21. */
void test_t192(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "map validation";

    handle_id_t g = t26_grant();
    if (g == HANDLE_INVALID) { it_fail("T192", "grant"); return; }

    /* Kernel VA. */
    if (ok && it_invoke((long)g, INV_FRAME_MAP, IT_VS, (long)0xFFFF800000000000ULL, 0L)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA"; }
    /* Unaligned VA. */
    if (ok && it_invoke((long)g, INV_FRAME_MAP, IT_VS, (long)(T26_SELF_VA | 0x800ULL), 0L)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "unaligned VA"; }
    /* Bad flags (W^X). */
    if (ok && it_invoke((long)g, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 3L)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "W^X"; }

    /* None of the above installed a PTE — a valid map now succeeds. */
    if (ok && it_invoke((long)T26_PAGE(g, 2), INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L) != 0) { ok = 0; why = "valid map"; }
    /* Occupied VA — a DIFFERENT page of the grant cannot take it. */
    if (ok && it_invoke((long)T26_PAGE(g, 1), INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L)
              != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied VA"; }
    if (ok && it_invoke2((long)T26_PAGE(g, 2), INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA) != 0) { ok = 0; why = "unmap"; }

    t26_grant_close(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T192"); else it_fail("T192", why);
}

/* ── T193: VMO-backed pager resolves a target fault ──────────────────────────
 * The defining Phase 26 path.  The supervisor fills VMO page 2 (offset 0x2000)
 * with a pattern; a VMO-backed pager (slot 14 = the VMO) resolves the target's
 * read fault by mapping THAT page read-only at the fault VA and seq-resuming;
 * the target continues and reads the pattern (its exit code proves the byte
 * flowed VMO→target).  Then a writable run: the target's store lands in the
 * VMO page, visible to the supervisor afterwards.  Target death sweeps the
 * VMO-backed mapping; the VMO stays live and reusable.
 * Invariants: M3, M9, M13(pos), M14, M15, M17, M19, M20. */
void test_t193(void) {
    uint32_t f0[6], f1[6], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0 && it_sched_ext5(f0);
    const char *why = "vmo-backed pager";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T193", "vmo create"); return; }
    word = T26_PAT0;
    if (ok && t26_page_word(T26_AT(vmo, 0x2000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    /* (A) read fault, read-only VMO cap → target reads the supervisor pattern. */
    struct t25_tgt g;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T193", why); return; }
    if (ok && t25_pager_spawn(&g, T26_AT(vmo, 0x2000ULL), RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) {
        ok = 0; why = "pager spawn A"; }
    /* subaction 4 = VMO map; va_ovr carries the VMO offset (page 2). */
    if (ok && t25_serve(pcmd, 1u, 1u, 0 /*RO*/, 0, T26_TVA_A) != 0) { ok = 0; why = "serve A"; }
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "fault A"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T26_PAT0 & 0xFFu))) { ok = 0; why = "target did not read pattern"; }
    if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report A"; }
    t25_reap(&pproc); it_close(&pcmd);
    t25_tgt_reap(&g);
    it_quiesce_reaper();

    /* VMO survived target death and is still readable/intact. */
    if (ok && (t26_page_word(T26_AT(vmo, 0x2000ULL), &word, 0) != 0 || word != T26_PAT0)) {
        ok = 0; why = "vmo not reusable after sweep"; }

    /* (B) write fault, writable VMO cap → target store lands in the VMO page. */
    if (ok) { word = 0; if (t26_page_word(T26_AT(vmo, 0x3000ULL), &word, 1) != 0) { ok = 0; why = "vmo zero"; } }
    struct t25_tgt g2;
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&g2, &why)) { t26_grant_close(&vmo); it_fail("T193", why); return; }
    if (ok) {
        if (t25_pager_spawn(&g2, T26_AT(vmo, 0x3000ULL), RIGHT_READ | RIGHT_WRITE, 0, 0u, &p2cmd, &p2proc) != 0) {
            ok = 0; why = "pager spawn B"; }
        if (ok && t25_serve(p2cmd, 1u, 1u, 1 /*W*/, 0, T26_TVA_B) != 0) { ok = 0; why = "serve B"; }
        if (ok && it_lp_cmd_va(g2.cmd, LP_CMD_FAULT_WRITE, T26_TVA_B) != 0) { ok = 0; why = "fault B"; }
        if (ok && it_lp_wait_exit(g2.proc) != LP_EXIT_MARKER) { ok = 0; why = "store did not retire"; }
        if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report B"; }
        t25_reap(&p2proc); it_close(&p2cmd);
    }
    t25_tgt_reap(&g2);
    it_quiesce_reaper();
    if (ok && (t26_page_word(T26_AT(vmo, 0x3000ULL), &word, 0) != 0 || word != T26_WMARK)) {
        ok = 0; why = "write not visible in vmo"; }

    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5 final"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 2u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 2u)  { ok = 0; why = "resume count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T193"); else it_fail("T193", why);
}

/* ── T194: unauthorized VMO pager denial ────────────────────────────────────
 * A VMO-backed pager fully authorized for target B, holding a WRITABLE VMO but
 * only a READ-only VMO derivation for the writable attempt, cannot: map into a
 * VSpace it lacks WRITE on; install a writable PTE from a read-only VMO cap;
 * touch a foreign target.  Every denial is ACCESS_DENIED with no mapping, no
 * VMO ref leak, no fault-state corruption.
 * Invariants: M2, M3, M9, M10, M13, M26. */
void test_t194(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0 && it_setup_self_vspace();
    const char *why = "unauthorized vmo pager";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T194", "vmo create"); return; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T194", why); return; }

    /* A READ-only VMO cap cannot install a writable PTE into g's VSpace. */
    long vro = it_cs_reduce((long)vmo, RIGHT_READ | RIGHT_DUPLICATE);
    handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
    if (ok && vro < 0) { ok = 0; why = "vro dup"; }
    if (ok && it_invoke((long)T26_AT(vro, 0), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(1u))
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro vmo writable into target"; }

    /* A READ-only VSpace derivation cannot install ANY PTE. */
    long vsro = it_cs_reduce((long)g.vs, RIGHT_READ | RIGHT_DUPLICATE);
    handle_id_t vsro_h = (vsro >= 0) ? (handle_id_t)vsro : HANDLE_INVALID;
    if (ok && vsro < 0) { ok = 0; why = "vsro dup"; }
    if (ok && it_invoke((long)T26_AT(vmo, 0), INV_FRAME_MAP, vsro, (long)T26_TVA_A, (long)(0))
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro vspace installed"; }

    /* Correct authority DOES install (proves the denials were the rights, not
     * some unrelated failure), then unmap through the target VSpace cap. */
    if (ok && it_invoke((long)T26_AT(vmo, 0), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(1u))
              != 0) { ok = 0; why = "authorized map denied"; }
    /* The authorized mapping is swept when the target dies below. */

    it_close(&vro_h); it_close(&vsro_h);
    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T194"); else it_fail("T194", why);
}

/* ── T195: VMO shared mappings across two targets ────────────────────────────
 * One VMO backs a page in target A and (independently) a page in target B.  A
 * writable share lets A's store become visible to the supervisor and to a
 * subsequent B mapping of the same VMO page — the VMO is genuinely shared.  A
 * dies; B's mapping is untouched and still works; B dies; everything returns
 * to baseline with the VMO still live until the supervisor closes it.
 * Invariants: M27, M28, M17, M19, M20, M22. */
void test_t195(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo shared mappings";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T195", "vmo create"); return; }
    word = 0;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo zero"; }

    /* Target A writes the shared VMO page (offset 0x1000). */
    struct t25_tgt ga;
    handle_id_t pacmd = HANDLE_INVALID, paproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&ga, &why)) { t26_grant_close(&vmo); it_fail("T195", why); return; }
    if (ok) {
        if (t25_pager_spawn(&ga, T26_AT(vmo, 0x1000ULL), RIGHT_READ | RIGHT_WRITE, 0, 0u, &pacmd, &paproc) != 0) {
            ok = 0; why = "pager A"; }
        if (ok && t25_serve(pacmd, 1u, 1u, 1 /*W*/, 0, T26_TVA_A) != 0) { ok = 0; why = "serve A"; }
        if (ok && it_lp_cmd_va(ga.cmd, LP_CMD_FAULT_WRITE, T26_TVA_A) != 0) { ok = 0; why = "fault A"; }
        if (ok && it_lp_wait_exit(ga.proc) != LP_EXIT_MARKER) { ok = 0; why = "A store"; }
        if (ok && it_lp_wait_exit(paproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager A report"; }
        t25_reap(&paproc); it_close(&pacmd);
    }
    t25_tgt_reap(&ga);           /* A dies — its mapping swept */
    it_quiesce_reaper();

    /* Supervisor sees A's write in the shared VMO page. */
    if (ok && (t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 0) != 0 || word != T26_WMARK)) {
        ok = 0; why = "A write not shared"; }

    /* Target B reads the SAME VMO page and observes A's write (RO map). */
    struct t25_tgt gb;
    handle_id_t pbcmd = HANDLE_INVALID, pbproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&gb, &why)) { t26_grant_close(&vmo); it_fail("T195", why); return; }
    if (ok) {
        if (t25_pager_spawn(&gb, T26_AT(vmo, 0x1000ULL), RIGHT_READ, 0, 0u, &pbcmd, &pbproc) != 0) {
            ok = 0; why = "pager B"; }
        if (ok && t25_serve(pbcmd, 1u, 1u, 0 /*RO*/, 0, T26_TVA_B) != 0) { ok = 0; why = "serve B"; }
        if (ok && it_lp_cmd_va(gb.cmd, LP_CMD_FAULT_READ, T26_TVA_B) != 0) { ok = 0; why = "fault B"; }
        if (ok && it_lp_wait_exit(gb.proc) !=
                  (long)(LP_EXIT_MARKER ^ (T26_WMARK & 0xFFu))) { ok = 0; why = "B did not read A's write"; }
        if (ok && it_lp_wait_exit(pbproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager B report"; }
        t25_reap(&pbproc); it_close(&pbcmd);
    }
    t25_tgt_reap(&gb);           /* B dies — its (independent) mapping swept */

    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T195"); else it_fail("T195", why);
}

/* ── T196: a mapping keeps its frame alive ──────────────────────────────────
 * The lifetime contract, and Ledger D-5 made it sharper rather than weaker.
 * It used to say a KVmo with a live mapping is not destroyed while the mapping
 * exists, because the KFrame behind the map retained the VMO — one object
 * holding another.  Now the mapped thing IS the frame: installing a PTE
 * retains it, so releasing the LAST capability to a mapped page destroys
 * nothing, and the page dies when the mapping does.
 *
 * Which is checked here by closing the whole grant while one of its pages is
 * mapped into a target: three pages go, the mapped one stays, and it goes when
 * the address space holding it does.  A released capability fails clean; no
 * double free, no stale PTE, no live drift.
 * Invariants: M18, M22, M23, M17, M19. */
void test_t196(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long flive0 = it_frame_live();
    int ok = b.ok && flive0 >= 0;
    const char *why = "mapping keeps its frame";

    handle_id_t g = t26_grant();
    if (g == HANDLE_INVALID) { it_fail("T196", "grant"); return; }
    word = T26_PAT1;
    if (ok && t26_page_word(T26_AT(g, 0), &word, 1) != 0) { ok = 0; why = "fill"; }
    if (ok && it_frame_live() != flive0 + (long)T26_GRANT_PAGES) {
        ok = 0; why = "grant not live"; }

    /* A pager installs page 0 in a target, and the target reads it. */
    struct t25_tgt t;
    handle_id_t pcmd = HANDLE_INVALID, pproc = HANDLE_INVALID;
    if (ok && !t25_tgt_spawn(&t, &why)) { t26_grant_close(&g); it_fail("T196", why); return; }
    if (ok) {
        if (t25_pager_spawn(&t, T26_AT(g, 0), RIGHT_READ, 0, 0u, &pcmd, &pproc) != 0) { ok = 0; why = "pager"; }
        if (ok && t25_serve(pcmd, 1u, 1u, 0, 0, T26_TVA_A) != 0) { ok = 0; why = "serve"; }
        if (ok && it_lp_cmd_va(t.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "fault"; }
        if (ok && it_lp_wait_exit(t.proc) !=
                  (long)(LP_EXIT_MARKER ^ (T26_PAT1 & 0xFFu))) { ok = 0; why = "target read"; }
        if (ok && it_lp_wait_exit(pproc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager report"; }
        t25_reap(&pproc); it_close(&pcmd);
    }
    /*
     * The target exited, but Stage 7-proc means its ADDRESS SPACE did not: an
     * address space ends when its last capability does, and this test still
     * holds one.  So the mapping is still installed — which is the whole point
     * of what follows.
     */
    it_quiesce_reaper();

    /* Release EVERY capability to the grant while page 0 is still mapped.
     *
     * Measured as a DELTA across the release, not against the count at the top
     * of the test: the target and the pager have their own images and stacks
     * in frames, and the target's are still alive because this test is still
     * holding its address space.  An absolute number here would be measuring
     * them too, and would have been wrong in a way that looked like the
     * property failing. */
    handle_id_t vcopy = T26_AT(g, 0);
    long n0 = it_frame_live();
    t26_grant_close(&g);
    it_quiesce_reaper();
    long n1 = it_frame_live();
    if (ok && (n0 < 0 || n1 < 0)) { ok = 0; why = "gauge"; }
    /* Three unreferenced pages went; the mapped one stayed. */
    if (ok && n1 != n0 - (long)(T26_GRANT_PAGES - 1u)) {
        it_fz_note("T196", (uint32_t)(n0 - n1), T26_GRANT_PAGES - 1u, 0u);
        ok = 0; why = "the mapping did not hold it";
    }
    /* The released capability fails clean even though the object is alive. */
    if (ok && it_invoke0((long)vcopy, INV_FRAME_SIZE) >= 0) { ok = 0; why = "stale size"; }

    /* Let go of the address space: the mapping goes, and so does the page. */
    it_close(&t.vs);
    it_quiesce_reaper();
    if (ok && it_frame_live() > n1 - 1) { ok = 0; why = "page outlived its mapping"; }

    t25_tgt_reap(&t);
    it_quiesce_reaper();
    if (ok && it_frame_live() != flive0) { ok = 0; why = "frame live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T196"); else it_fail("T196", why);
}

/* ── T197: VMO-backed pager death/restart ────────────────────────────────────
 * A VMO-backed pager supervised under a restart limit dies with the target's
 * fault pending; the fault survives (suspended-alive), the VMO stays live, no
 * ghost refs.  A restarted instance carries EXACTLY the declared manifest (its
 * page source is the VMO, no untyped/global frame authority) and completes the
 * resolution from the VMO.  The Fase24↔26 junction.
 * Invariants: M24, M25, plus P15/P16/P17 (supervision) under a VMO source. */
void test_t197(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo pager death/restart";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T197", "vmo create"); return; }
    word = T26_PAT0;
    if (ok && t26_page_word(T26_AT(vmo, 0x2000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T197", why); return; }
    uint32_t d0 = t25_delivered_now();
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "fault cmd"; }
    if (ok && !t25_wait_delivered(d0)) { ok = 0; why = "fault pending"; }

    /* Gen 1: spawned in charge, killed before serving. */
    handle_id_t p1cmd = HANDLE_INVALID, p1proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, T26_AT(vmo, 0x2000ULL), RIGHT_READ, 0, 0u, &p1cmd, &p1proc) != 0) { ok = 0; why = "pager1"; }
    if (ok && it_kill((long)p1proc) != 0) { ok = 0; why = "kill pager1"; }
    if (ok && it_lp_wait_exit(p1proc) != 0) { ok = 0; why = "pager1 exit"; }
    t25_reap(&p1proc); it_close(&p1cmd);
    it_quiesce_reaper();

    /* Fault survives; VMO stays live.  A-22: survival is proved by the next
     * generation serving it, at the end of this test. */
    if (ok && it_invoke0(it_child_tcb((long)g.proc), INV_TCB_EXIT_CODE)
              != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "fault lost"; }
    /* The VMO survived the pager's death — verified functionally (the target is
     * still SUSPENDED here, so an absolute vmo_live count would also see its
     * live segment/stack VMOs; the leak guard is the final vlive0 check after
     * everything is reaped). */
    if (ok && it_invoke0((long)T26_AT(vmo, 0x2000ULL), INV_FRAME_SIZE) != 4096L) {
        ok = 0; why = "grant lost with pager"; }

    /* Post-restart manifest is exactly the declaration (slot-14 = VMO source). */
    if (ok) {
        struct svc_mint x[4] = { 0 };
        x[0].slot = LP_PGR_SLOT_TPROC; IT_MINT_SRC(x[0], g.proc);  x[0].rights = RIGHT_READ | RIGHT_MANAGE; x[0].badge = 0;
        x[1].slot = LP_PGR_SLOT_TVS;   IT_MINT_SRC(x[1], g.vs);    x[1].rights = RIGHT_WRITE;               x[1].badge = 0;
        x[2].slot = LP_PGR_SLOT_FRAME; IT_MINT_SRC(x[2], T26_AT(vmo, 0x2000ULL));
                                                          x[2].rights = RIGHT_READ;                x[2].badge = 0;
        x[3].slot = LP_PGR_SLOT_FAULT_EP; IT_MINT_SRC(x[3], g.notif); x[3].rights = RIGHT_READ;                x[3].badge = 0;
        long rep = it_lp_report_slots(x, 4u);
        uint32_t expect = (1u << LP_CPTR_CMD_EP) | (1u << LP_PGR_SLOT_TPROC) |
                          (1u << LP_PGR_SLOT_TVS) | (1u << LP_PGR_SLOT_FRAME) |
                          /* No fault mailbox: this probe declares its own
                           * four-capability manifest and resolves nothing, so
                           * it is handed no mailbox to resolve WITH. */
                          (1u << LP_PGR_SLOT_FAULT_EP);
        if (rep < 0 || (uint32_t)rep != expect) { ok = 0; why = "post-restart manifest"; }
        if (ok && ((uint32_t)rep & ((1u<<6)|(1u<<10)|(1u<<11)|(1u<<16)|(1u<<17)|(1u<<18))) != 0) {
            ok = 0; why = "extra authority after restart"; }
    }

    /* Gen 2 (the server) completes the resolution from the VMO. */
    handle_id_t p2cmd = HANDLE_INVALID, p2proc = HANDLE_INVALID;
    if (ok && t25_pager_spawn(&g, T26_AT(vmo, 0x2000ULL), RIGHT_READ, 0, 0u, &p2cmd, &p2proc) != 0) { ok = 0; why = "pager2"; }
    if (ok && t25_serve(p2cmd, 1u, 1u, 0, 0, T26_TVA_A) != 0) { ok = 0; why = "serve"; }
    if (ok && it_lp_wait_exit(g.proc) !=
              (long)(LP_EXIT_MARKER ^ (T26_PAT0 & 0xFFu))) { ok = 0; why = "target completion"; }
    if (ok && it_lp_wait_exit(p2proc) != LP_EXIT_PGR_OK) { ok = 0; why = "pager2 report"; }
    t25_reap(&p2proc); it_close(&p2cmd);

    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T197"); else it_fail("T197", why);
}

/* ── T198: partial-failure atomicity ────────────────────────────────────────
 * A batch of denied SYS_FRAME_MAP calls — kernel VA, occupied VA, insufficient
 * rights, a released capability — leaves the address space and every book
 * exactly as before: no partial PTE, no mapped_count drift, no frame ref leak.
 * A valid map before and after the batch proves the space was never corrupted.
 *
 * Ledger D-5: the "bad offset" and "offset == size" cases went with the object.
 * They asserted that the kernel range-checked an index into a region it owned;
 * a caller now names a page by capability, so the same mistake IS the released
 * capability case below, which this batch still makes.
 * Invariants: M21, M22, M19, M20, M23. */
void test_t198(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long flive0 = it_frame_live();
    int ok = b.ok && flive0 >= 0 && it_setup_self_vspace();
    const char *why = "partial failure";

    handle_id_t g = t26_grant();
    if (g == HANDLE_INVALID) { it_fail("T198", "grant"); return; }
    long vro = it_cs_reduce((long)T26_PAGE(g, 1), RIGHT_READ | RIGHT_DUPLICATE);
    handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
    long vstale = it_cs_reduce((long)T26_PAGE(g, 1), RIGHT_READ | RIGHT_DUPLICATE);
    handle_id_t vstale_h = (vstale >= 0) ? (handle_id_t)vstale : HANDLE_INVALID;
    if (ok && (vro < 0 || vstale < 0)) { ok = 0; why = "dups"; }
    it_close(&vstale_h);   /* now stale */

    /* Anchor map at page 0 so "occupied VA" has a real occupant. */
    if (ok && it_invoke((long)g, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L) != 0) {
        ok = 0; why = "anchor map"; }

    /* Failure battery — every one must be rejected with no side effect. */
    struct { long fr_c; long vs_c; uint64_t va; long fl; long want; const char *tag; } bad[] = {
        { (long)g,            IT_VS, 0xFFFF800000000000ULL,    0L, (long)IRIS_ERR_INVALID_ARG,  "kernel va" },
        { (long)T26_PAGE(g,2),IT_VS, T26_SELF_VA | 0x800ULL,   0L, (long)IRIS_ERR_INVALID_ARG,  "unaligned va" },
        { (long)T26_PAGE(g,2),IT_VS, T26_SELF_VA,              3L, (long)IRIS_ERR_INVALID_ARG,  "W^X" },
        { (long)T26_PAGE(g,1),IT_VS, T26_SELF_VA,              0L, (long)IRIS_ERR_BUSY,         "occupied" },
        { vro,                IT_VS, T26_SELF_VA + 0x10000ULL, 1L, (long)IRIS_ERR_ACCESS_DENIED,"ro writable" },
        /* Stage 4: a "stale cap" is a DELETED SLOT, and an empty slot is
         * NOT_FOUND — the CSpace form of the BAD_HANDLE this asserted while
         * the cap was a handle.  The property is the same and is the one that
         * survives: a capability that was released fails clean and mutates
         * nothing. */
        { vstale,             IT_VS, T26_SELF_VA + 0x10000ULL, 0L, (long)IRIS_ERR_NOT_FOUND,    "stale page" },
    };
    for (uint32_t i = 0; ok && i < 6u; i++) {
        long r = it_invoke(bad[i].fr_c, INV_FRAME_MAP, bad[i].vs_c, (long)bad[i].va, bad[i].fl);
        if (r != bad[i].want) { ok = 0; why = bad[i].tag; }
    }

    /* The space is intact: unmap the anchor, remap elsewhere, unmap. */
    if (ok && it_invoke2((long)g, INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA) != 0) {
        ok = 0; why = "anchor unmap"; }
    if (ok && it_invoke((long)T26_PAGE(g, 2), INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 1L) != 0) { ok = 0; why = "post-batch map"; }
    if (ok && it_invoke2((long)T26_PAGE(g, 2), INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA) != 0) { ok = 0; why = "post unmap"; }

    it_close(&vro_h);
    t26_grant_close(&g);
    it_quiesce_reaper();
    if (ok && it_frame_live() != flive0) { ok = 0; why = "frame live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T198"); else it_fail("T198", why);
}

/* ── T199: VMO rights/PTE policy stress ──────────────────────────────────────
 * PTE rights are the meet of the VMO cap and the requested flags, enforced at
 * the hardware level: a page mapped READ-ONLY into a target from a WRITABLE
 * VMO cap still write-protection-faults the target's store (err P|W|U) — the
 * PTE carries the MAPPING's rights, not the cap's ceiling.  W^X and remap of an
 * occupied VA are rejected.  No silent write, no escalation via remap.
 * Invariants: M9, M10, M8, M21, plus the Phase 20 write-fault observable. */
void test_t199(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo rights/PTE";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T199", "vmo create"); return; }

    /* Map RO into a target through a fully-WRITABLE VMO cap; the target's store
     * must still fault write-protection (the PTE is RO, cap ceiling irrelevant). */
    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T199", why); return; }
    if (ok && it_invoke((long)T26_AT(vmo, 0), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(0)) != 0) {
        ok = 0; why = "ro map"; }
    /* Occupied VA remap rejected. */
    if (ok && it_invoke((long)T26_AT(vmo, 0x1000ULL), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(0))
              != (long)IRIS_ERR_BUSY) { ok = 0; why = "occupied remap"; }

    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T26_TVA_A) != 0) { ok = 0; why = "write cmd"; }
    if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "no wp fault"; }
    if (ok && (f.vector != 14u || f.cr2 != T26_TVA_A ||
               f.error != (PF_ERR_P | PF_ERR_W | PF_ERR_U))) { ok = 0; why = "wp err bits"; }
    if (ok && t25_resume_seq(&g, f.task_id, f.seq, 1) != 0) { ok = 0; why = "seq kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }

    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T199"); else it_fail("T199", why);
}
static uint32_t t200_rnd(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
void test_t200(void) {
    uint32_t rng = T200_SEED, word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "vmo stress";
    uint32_t round = 0u, op = 0u;

    for (round = 0; ok && round < T200_ROUNDS; round++) {
        op = t200_rnd(&rng) % 5u;
        uint64_t ofs = ((uint64_t)(t200_rnd(&rng) % 4u)) << 12;   /* one of 4 pages */

        handle_id_t vmo = t26_grant();
        if (vmo == HANDLE_INVALID) { ok = 0; why = "vmo create"; break; }
        word = T26_PAT0 ^ round;
        if (t26_page_word(T26_AT(vmo, ofs), &word, 1) != 0) { ok = 0; why = "vmo fill"; t26_grant_close(&vmo); break; }

        struct t25_tgt g;
        if (!t25_tgt_spawn(&g, &why)) { ok = 0; t26_grant_close(&vmo); break; }
        struct it_fault f;

        switch (op) {
        case 0: {
            /* VMO-backed read resolve. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (t25_pager_spawn(&g, T26_AT(vmo, ofs), RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op0 pager"; break; }
            if (t25_serve(pc, 1u, 1u, 0, 0, 0) != 0) { ok = 0; why = "op0 serve"; }
            if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op0 fault"; }
            if (ok && it_lp_wait_exit(g.proc) !=
                      (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op0 target"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op0 pager report"; }
            t25_reap(&pp); it_close(&pc);
            break;
        }
        case 1: {
            /* VMO-backed writable resolve; store lands in the VMO. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            word = 0; if (t26_page_word(T26_AT(vmo, ofs), &word, 1) != 0) { ok = 0; why = "op1 zero"; break; }
            if (t25_pager_spawn(&g, T26_AT(vmo, ofs), RIGHT_READ | RIGHT_WRITE, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op1 pager"; break; }
            if (t25_serve(pc, 1u, 1u, 1u, 0, T26_TVA_A) != 0) { ok = 0; why = "op1 serve"; }
            if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T26_TVA_A) != 0) { ok = 0; why = "op1 fault"; }
            if (ok && it_lp_wait_exit(g.proc) != LP_EXIT_MARKER) { ok = 0; why = "op1 store"; }
            if (ok && it_lp_wait_exit(pp) != LP_EXIT_PGR_OK) { ok = 0; why = "op1 pager report"; }
            t25_reap(&pp); it_close(&pc);
            if (ok && (t26_page_word(T26_AT(vmo, ofs), &word, 0) != 0 || word != T26_WMARK)) { ok = 0; why = "op1 not stored"; }
            break;
        }
        case 2: {
            /* Pager dies before serving; supervisor takes over from the VMO. */
            handle_id_t pc = HANDLE_INVALID, pp = HANDLE_INVALID;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op2 fault"; break; }
            if (!t25_wait_fault(&g, &f)) { ok = 0; why = "op2 pending"; break; }
            if (t25_pager_spawn(&g, T26_AT(vmo, ofs), RIGHT_READ, 0, 0u, &pc, &pp) != 0) { ok = 0; why = "op2 pager"; break; }
            if (it_kill((long)pp) != 0 || it_lp_wait_exit(pp) != 0) { ok = 0; why = "op2 pager death"; }
            t25_reap(&pp); it_close(&pc);
            /* Supervisor resolves from the VMO via its own VSpace handle. */
            if (ok && it_invoke((long)T26_AT(vmo, ofs), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(0)) != 0) { ok = 0; why = "op2 map"; }
            if (ok && t25_resume_seq(&g, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op2 resume"; }
            if (ok && it_lp_wait_exit(g.proc) !=
                      (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op2 target"; }
            break;
        }
        case 3: {
            /* Target death mid-fault; late map is BAD_HANDLE. */
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op3 fault"; break; }
            if (!t25_wait_fault(&g, &f)) { ok = 0; why = "op3 pending"; break; }
            if (it_kill((long)g.proc) != 0 || it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "op3 kill"; }
            it_quiesce_reaper();
            if (ok && it_invoke((long)T26_AT(vmo, ofs), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(0))
                      != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "op3 late map"; }
            if (ok && t25_resume_seq(&g, f.task_id, f.seq, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op3 late resume"; }
            break;
        }
        case 4: {
            /* Failure injection under load, THEN a clean resolve.  The denied
             * maps install nothing, so T26_TVA_A stays unmapped and the target
             * still faults; only then does the supervisor map + resume. */
            long vro = it_cs_reduce((long)T26_AT(vmo, ofs), RIGHT_READ | RIGHT_DUPLICATE);
            handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
            if (vro < 0) { ok = 0; why = "op4 dup"; break; }
            /* RO cap cannot install a writable PTE; a page past the end of
             * the grant is a slot nobody holds — neither leaves anything at
             * T26_TVA_A.  Ledger D-5: "beyond size" used to be the kernel
             * range-checking an offset; it is an empty CSpace slot now, which
             * is a stronger statement and a cheaper check. */
            if (it_invoke(vro, INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, 1L)
                != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op4 ro writable"; }
            if (ok && it_invoke((long)T26_PAGE(vmo, T26_GRANT_PAGES), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, 0L)
                != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "op4 past the grant"; }
            it_close(&vro_h);
            /* Drive the real fault, then map + resume from the VMO. */
            if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T26_TVA_A) != 0) { ok = 0; why = "op4 fault"; }
            if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "op4 pending"; }
            if (ok && it_invoke((long)T26_AT(vmo, ofs), INV_FRAME_MAP, (long)g.vs, (long)T26_TVA_A, (long)(0)) != 0) { ok = 0; why = "op4 map"; }
            if (ok && t25_resume_seq(&g, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op4 resume"; }
            if (ok && it_lp_wait_exit(g.proc) !=
                      (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op4 target"; }
            break;
        }
        default: break;
        }

        /* A-22: a fault this round did not answer is one the supervisor still
         * holds the reply for.  Dropping it is the answer — and after that
         * nothing is outstanding, which is what "residual" meant. */
        (void)it_fault_kill(g.fault_leaf);
        if (ok && it_fault_info(g.fault_leaf, &f) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "residual fault"; }
        t25_tgt_reap(&g);
        t26_grant_close(&vmo);
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&b, &r, &why)) ok = 0;
            else if (it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live final"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T200");
    else { it_fz_note("T200", T200_SEED, round, op); it_fail("T200", why); }
}

/* Spawn the pager SERVICE granting targets[0..nt) and vmos[0..nv).  vmo_w_mask
 * bit j = grant RIGHT_WRITE on vmo j.  Optionally register "pager.ep".  0 on
 * success. */
int t27_pager_spawn(struct t27_pager *p,
                           struct t25_tgt *targets, uint32_t nt,
                           handle_id_t *vmos, uint32_t nv, uint32_t vmo_w_mask,
                           int do_register, const char **why) {
    p->ctrl_ep = p->proc = HANDLE_INVALID; p->reg_id = -1; p->generation = 0;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ctrl ep"; return 0; }
    handle_id_t ctrl = (handle_id_t)ep;

    /*
     * Ledger A-22: point every granted target's faults at the ONE shared
     * ENDPOINT (targets[0].notif), each through a copy BADGED with its own
     * index, BEFORE the pager starts — so no fault can land on the old wiring.
     *
     * The badge is what replaces both the per-target signal bit and the
     * per-target mailbox leaf: it rides on the message, so the pager learns
     * whose fault it is from the fault itself rather than from where the
     * kernel happened to put a capability.  It is `i + 1` because 0 is what an
     * unbadged capability carries and a server must be able to tell those
     * apart.
     */
    if (!it_pgr_mbox_fresh(nt)) { it_close(&ctrl); *why = "fault replies"; return 0; }
    for (uint32_t i = 0; i < nt; i++) {
        long bep = it_cs_badge((long)targets[0].notif,
                               RIGHT_READ | RIGHT_WRITE, i + 1u);
        int wired = (bep >= 0 &&
                     it_invoke(it_child_tcb((long)targets[i].proc), INV_TCB_SET_FAULT_HANDLER, bep, 0, 0) == 0);
        /* The badge is COPIED into the registration, so the capability that
         * carried it has done its job and is dropped — leaving it in the
         * rotating pool would keep the endpoint alive past the test's own
         * baseline check. */
        if (bep >= 0) it_slot_delete((uint32_t)bep);
        if (!wired) { it_close(&ctrl); *why = "shared fault ep wire"; return 0; }
    }

    struct svc_mint m[48] = { 0 };
    uint32_t n = 0;
    m[n].slot = PGR_SLOT_CTRL_EP; IT_MINT_SRC(m[n], ctrl); m[n].rights = RIGHT_READ; m[n].badge = 0; n++;
    if (nt > 0) {
        m[n].slot = PGR_SLOT_FAULT_EP; IT_MINT_SRC(m[n], targets[0].notif); m[n].rights = RIGHT_READ; m[n].badge = 0; n++;
        /* A-22: the reply objects the pager receives with — the authority to
         * resume, one per target it may serve. */
        m[n].slot = PGR_SLOT_FAULT_CN; IT_MINT_SRC(m[n], IT_PGR_MBOX_SLOT); m[n].rights = RIGHT_READ | RIGHT_WRITE; m[n].badge = 0; n++;
    }
    for (uint32_t i = 0; i < nt; i++) {
        /* Stage 7 Step 8: the pager is NOT given the target's process
         * capability.  It resolved faults with it — read the record, name the
         * thread — and both of those are the thread's now: the mailbox hands
         * it the faulting TCB and SYS_TCB_FAULT_INFO reads off that.  What is
         * left that a pager does is MAP, which is the VSpace below.  Holding
         * authority nothing uses is what this manifest exists to catch. */
        m[n].slot = PGR_TSLOT_VS(i);    IT_MINT_SRC(m[n], targets[i].vs);    m[n].rights = RIGHT_WRITE;               m[n].badge = 0; n++;
    }
    /* One capability per page.  What the pager may install is what it holds,
     * and whether it may install a page writable is RIGHT_WRITE on that page —
     * the grant's whole policy, stated in mints. */
    for (uint32_t j = 0; j < nv; j++) {
        iris_rights_t vr = RIGHT_READ | ((vmo_w_mask & (1u << j)) ? RIGHT_WRITE : 0u);
        for (uint32_t pg = 0; pg < PGR_GRANT_PAGES; pg++) {
            m[n].slot = PGR_PSLOT(j, pg);
            IT_MINT_SRC(m[n], T26_PAGE(vmos[j], pg));
            m[n].rights = vr; m[n].badge = 0; n++;
        }
    }

    /* Phase S1: the pager serves EP_CALLs on its ctrl endpoint — retype a
     * fresh reply object and mint it at PGR_SLOT_REPLY (13); drop our handle
     * right after so pager death still wakes blocked callers. */
    handle_id_t pgr_reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            pgr_reply_h = (handle_id_t)rr;
            m[n].slot = 13u; IT_MINT_SRC(m[n], pgr_reply_h);
            m[n].rights = RIGHT_READ | RIGHT_WRITE; m[n].badge = 0; n++;
        }
    }

    /* Phase 28: the pager is its own supervised binary (initrd "pager"); it
     * enters its serve loop immediately on start — no mode-entry message. */
    handle_id_t boot = HANDLE_INVALID;
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "pager",
                             &p->proc, &boot, m, n,
                             IT_LOADER_WS, 0,
                               /* The pager MAPS — into address spaces that are
                                * not even its own — so it owes paging levels
                                * and needs a budget to retype them from. */
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(p->proc);
    it_close(&pgr_reply_h);
    it_close(&boot);
    if (r < 0 || p->proc == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&p->proc); *why = "pager spawn"; return 0;
    }
    p->ctrl_ep = ctrl;
    p->generation = 1;
    if (do_register) p->reg_id = it_register_ep("pager.svc", ctrl);
    return 1;
}

/* EP_CALL the pager; returns its result word (0 = OK, negative error/marker,
 * or a positive REPORT bitmask), or a negative transport error. */
long t27_pager_call(handle_id_t ctrl_ep, uint32_t op, uint32_t tidx,
                           uint32_t vidx, uint32_t flags,
                           uint64_t offset, uint64_t expect) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.words[0] = PGR_PACK(op, tidx, vidx, flags);
    m.words[1] = offset;
    m.words[2] = expect;
    m.word_count = 3u;
    long r = it_invoke1((long)ctrl_ep, INV_EP_CALL, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}

void t27_pager_reap(struct t27_pager *p) {
    if (p->reg_id >= 0) { (void)it_unregister((uint32_t)p->reg_id); p->reg_id = -1; }
    if (p->proc != HANDLE_INVALID) {
        (void)it_kill((long)p->proc);
        (void)it_lp_wait_exit(p->proc);
    }
    it_close(&p->proc);
    it_close(&p->ctrl_ep);
}

/* Drive one VMO-backed read-fault resolution end to end: trigger the target's
 * read fault, call the pager to map vmo[vidx]@offset and resume; return 1 if
 * the target then ran to completion reading `pat`. */
int t27_resolve_read(struct t27_pager *p, struct t25_tgt *g,
                            uint32_t tidx, uint32_t vidx, uint64_t offset,
                            uint64_t va, uint32_t pat, const char **why) {
    if (it_lp_cmd_va(g->cmd, LP_CMD_FAULT_READ, va) != 0) { *why = "fault trigger"; return 0; }
    long res = t27_pager_call(p->ctrl_ep, PGR_OP_MAP_RESUME, tidx, vidx, 0u, offset, va);
    if (res != 0) { *why = "pager resolve"; return 0; }
    if (it_lp_wait_exit(g->proc) != (long)(LP_EXIT_MARKER ^ (pat & 0xFFu))) {
        *why = "target completion"; return 0;
    }
    return 1;
}

/* ── T201: pager service manifest and startup ───────────────────────────────
 * The pager comes up as a real supervised service with EXACTLY its declared
 * manifest — control endpoint + one target grant (proc/vspace/notif) + one VMO
 * grant — and nothing else: no spawn cap, no device caps, no untyped, no
 * KDEBUG, no core client eps, no caps for undeclared targets/VMOs.  It
 * registers "pager.ep" in svcmgr; a lookup returns the current endpoint and a
 * PING through it answers.  Invariants: G1–G8, G28. */
void test_t201(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager manifest";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T201", why); return; }
    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { t25_tgt_reap(&g); it_fail("T201", "vmo create"); return; }

    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    uint32_t pg0 = it_ipc_buffer_gauge();
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 1 /*register*/, &why)) { ok = 0; }

    /* PING via the direct control cap. */
    if (ok && t27_pager_call(p.ctrl_ep, PGR_OP_PING, 0, 0, 0, 0, 0) != 0) { ok = 0; why = "ping"; }

    /* D-4: and it registered an IPC buffer of its own on the way up.  The
     * pager is spawned per-test rather than at boot, so it is not in T313's
     * standing count and this is where its migration is observable.  A
     * registration that quietly failed would leave the pager on the kernel's
     * 256-byte staging with every test still green — which is exactly how the
     * first service's failure went unnoticed. */
    {
        uint32_t pg1 = it_ipc_buffer_gauge();
        if (ok && pg0 != 0xFFFFFFFFu && pg1 != 0xFFFFFFFFu && pg1 <= pg0) {
            ok = 0; why = "pager did not register an IPC buffer";
            it_fz_note("T201", pg0, pg1, 0u);
        }
    }

    /* Manifest oracle: EXACTLY {ctrl 3, shared fault notif 5, vmo0 16,
     * target proc/vs presence (bits 20/21)}. */
    if (ok) {
        long mask = t27_pager_call(p.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        uint32_t expect = (1u << PGR_SLOT_CTRL_EP) | (1u << PGR_SLOT_FAULT_EP) |
                          (1u << PGR_SLOT_FAULT_CN) /* Stage 7 Step 7: the fault
                              * mailbox.  Real authority — the CNode a fault
                              * delivers the faulting thread into — so the
                              * oracle counts it rather than being blind to it,
                              * which is the same reason slot 15 is here. */ |
                          (1u << 13) /* Phase S1: explicit reply object */ |
                          (1u << 15) /* Stage 4: the pager's own VSpace, now a cap */ |
                          (1u << IRIS_CPTR_OWN_UNTYPED) /* Stage 6-pure Step 2: the
                              * budget its own address space was built from.  The
                              * pager MAPS, and the kernel no longer creates paging
                              * levels, so it must be able to retype one.  A real
                              * authority, which is why it belongs in this oracle. */ |
                          (1u << IRIS_CPTR_OWN_VSPACE) |
                          (1u << IRIS_CPTR_OWN_TCB) /* D-6: its own address
                              * space and its own thread, DELEGATED by its
                              * spawner instead of fabricated with
                              * SYS_VSPACE_SELF / SYS_TCB_SELF, which publish
                              * capabilities with no MDB parent that no revoke
                              * can reach.  Not new authority — a thread could
                              * always name both — but they are now in the
                              * oracle, because a capability that exists in a
                              * slot is authority whoever reads this must
                              * account for. */ |
                          PGR_REPORT_GRANT | (1u << 21);
        /* Stage 7 Step 8: bit 20 (any target PROCESS capability) is GONE.  A
         * pager maps and answers faults; both name the address space and the
         * thread, and neither names the process. */
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "manifest mismatch"; }
        /* Explicitly none of: core client eps (1/2/4 — slot 3 is the control
         * endpoint), spawn(bit24), untyped(bit26), vspace-self(bit27), second
         * vmo (17). */
        if (ok && ((uint32_t)mask & ((1u<<1)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7)|
                                     (1u<<24)|(1u<<26)|(1u<<27)|(1u<<17))) != 0) {
            ok = 0; why = "extra authority";
        }
    }

    /* Registry presence: "pager.ep" resolves and a PING through the looked-up
     * cap answers (the registry serves the CURRENT endpoint). */
    if (ok && p.reg_id < 0) { ok = 0; why = "not registered"; }
    if (ok) {
        struct IrisMsg lm;
        it_slot_delete((uint32_t)IT_LOOKUP_TMP);
        if (it_lookup_name_slot("pager.svc", (uint32_t)IT_LOOKUP_TMP, &lm) != 0 ||
            lm.label != IRIS_EP_REPLY_OK ||
            lm.attached_handle != (uint32_t)IT_LOOKUP_TMP) { ok = 0; why = "lookup"; }
        else {
            if (t27_pager_call((handle_id_t)IT_LOOKUP_TMP, PGR_OP_PING, 0, 0, 0, 0, 0) != 0) {
                ok = 0; why = "lookup ping";
            }
            it_slot_delete((uint32_t)IT_LOOKUP_TMP);
        }
    }

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T201"); else it_fail("T201", why);
}

/* ── T202: pager service resolves one target fault ──────────────────────────
 * The full service path: supervisor fills a VMO page, spawns the pager with a
 * target grant + VMO grant, triggers the target's read fault, and calls the
 * pager to resolve it.  The pager waits on the target's fault notification,
 * reads the record through its proc cap, maps the VMO page at the fault VA via
 * its VSpace cap, and seq-resumes.  The target reads the supervisor's pattern
 * and completes.  Exactly-once delivery; no implicit caps; books at baseline.
 * Invariants: G9–G13, G23–G27. */
void test_t202(void) {
    uint32_t f0[6], f1[6], word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0 && it_sched_ext5(f0);
    const char *why = "pager resolve";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T202", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T202", why); return; }
    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    if (ok && !t27_resolve_read(&p, &g, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    /* Fault record cleared; no residual delivery. */
    if (ok && it_fault_info(g.fault_leaf, &(struct it_fault){0}) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "record survived";
    }

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_sched_ext5(f1)) { ok = 0; why = "ext5"; }
    if (ok && f1[IT_S5_DELIVER] != f0[IT_S5_DELIVER] + 1u) { ok = 0; why = "delivery count"; }
    if (ok && f1[IT_S5_RESUME]  != f0[IT_S5_RESUME] + 1u)  { ok = 0; why = "resume count"; }
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T202"); else it_fail("T202", why);
}

/* ── T203: unauthorized target denial ───────────────────────────────────────
 * A pager granted target A only cannot touch target B: neither the pager (its
 * manifest has no B cap) nor a hand-forged attempt through A's caps reaches B.
 * We prove it at the cap layer the pager relies on: A's proc/vspace caps do
 * NOT resolve B's fault or install in B's VSpace.  B's fault stays pending and
 * is resolved only by B's own authority.  Invariants: G5, G6, G10, G20, G30. */
void test_t203(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "unauthorized target";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T203", "vmo create"); return; }

    struct t25_tgt ga, gb;      /* A = pager's grant; B = unrelated */
    if (ok && !t25_tgt_spawn(&ga, &why)) { t26_grant_close(&vmo); it_fail("T203", why); return; }
    if (ok && !t25_tgt_spawn(&gb, &why)) { t25_tgt_reap(&ga); t26_grant_close(&vmo); it_fail("T203", why); return; }

    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &ga, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* B faults. */
    struct it_fault fb;
    if (ok && it_lp_cmd_va(gb.cmd, LP_CMD_FAULT_READ, T27_VA_B) != 0) { ok = 0; why = "B fault"; }
    if (ok && !t25_wait_fault(&gb, &fb)) { ok = 0; why = "B pending"; }

    /* A's caps (which the pager holds) grant nothing over B — proven directly:
     * A's proc cap is a different object, so resolving B's task through it is
     * NOT_FOUND; mapping the VMO into A's VSpace does not touch B. */
    if (ok && t25_resume_seq(&ga, fb.task_id, fb.seq, 0) != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "A cap resolved B"; }
    if (ok && it_invoke((long)T26_AT(vmo, 0x1000ULL), INV_FRAME_MAP, (long)ga.vs, (long)T27_VA_B, 0)
        == 0) {
        /* This installs into A's VSpace at T27_VA_B — legal for A, but it must
         * NOT affect B.  Verify B still faults (unchanged) below; unmap via A's
         * death at reap. */
    }
    /* B's fault is intact: same generation, still suspended. */
    struct it_fault fb2;
    if (ok && (it_fault_info(gb.fault_leaf, &fb2) != 0 || fb2.seq != fb.seq)) { ok = 0; why = "B fault disturbed"; }
    if (ok && it_invoke0(it_child_tcb((long)gb.proc), INV_TCB_EXIT_CODE) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "B not suspended"; }

    /* B is resolved only by B's own authority. */
    if (ok && t25_resume_seq(&gb, fb2.task_id, fb2.seq, 1) != 0) { ok = 0; why = "B proper kill"; }
    if (ok && it_lp_wait_exit(gb.proc) != 0) { ok = 0; why = "B exit"; }

    t27_pager_reap(&p);
    t25_tgt_reap(&ga); t25_tgt_reap(&gb);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T203"); else it_fail("T203", why);
}

/* ── T204: unauthorized VMO denial ──────────────────────────────────────────
 * A pager with target authority but a READ-only VMO grant cannot resolve a
 * WRITE fault (its map is refused ACCESS_DENIED, so the target keeps
 * write-faulting); a bad offset / kernel VA is refused; a valid RO resolution
 * afterwards still works.  No partial PTE, no VMO ref leak, fault state intact.
 * Invariants: G7, G11, G12, G24, G25. */
void test_t204(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "unauthorized vmo";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T204", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x2000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T204", why); return; }
    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    /* RO VMO grant (vmo_w_mask = 0). */
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* WRITE fault + RO VMO grant → the pager's writable map is ACCESS_DENIED,
     * so PGR_OP_MAP_RESUME (flags W) reports the error and the target is left
     * write-faulting.  We drive it directly to observe the denial cleanly. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_WRITE, T27_VA_A) != 0) { ok = 0; why = "write fault"; }
    if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "pending"; }
    /* The pager holds RO vmo → a writable map into the target must be denied.
     * Emulate the pager's exact call via its VSpace cap (the pager would get
     * the same ACCESS_DENIED). */
    long vro = it_cs_reduce((long)T26_AT(vmo, 0x2000ULL), RIGHT_READ | RIGHT_DUPLICATE);
    handle_id_t vro_h = (vro >= 0) ? (handle_id_t)vro : HANDLE_INVALID;
    if (ok && vro < 0) { ok = 0; why = "ro dup"; }
    if (ok && it_invoke(vro, INV_FRAME_MAP, (long)g.vs, (long)T27_VA_A, 1L)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "ro writable not denied"; }
    /* A page past the end of the grant, and a kernel VA, are both refused with
     * no PTE installed.  Ledger D-5: "bad offset" is an empty CSpace slot now,
     * because the page a caller names IS a capability. */
    if (ok && it_invoke((long)T26_PAGE(vmo, T26_GRANT_PAGES), INV_FRAME_MAP, (long)g.vs, (long)T27_VA_A, 0L)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "past the grant"; }
    if (ok && it_invoke((long)vmo, INV_FRAME_MAP, (long)g.vs, (long)0xFFFF800000000000ULL, 0L)
              != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "kernel VA"; }
    it_close(&vro_h);

    /* A valid RO resolution afterwards works (space uncorrupted): map RO at the
     * fault VA and seq-resume — the store will re-fault WP, so kill instead. */
    if (ok && it_invoke((long)T26_AT(vmo, 0x2000ULL), INV_FRAME_MAP, (long)g.vs, (long)T27_VA_A, (long)(0u)) != 0) {
        ok = 0; why = "valid ro map"; }
    if (ok && t25_resume_seq(&g, f.task_id, f.seq, 1) != 0) { ok = 0; why = "kill"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "exit"; }

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T204"); else it_fail("T204", why);
}

/* ── T205: pager restart preserves authority ────────────────────────────────
 * A pager service resolves a fault, is killed, and restarted by the supervisor
 * with the SAME manifest.  The new instance: gets a new generation; is
 * re-registered so "pager.ep" serves the CURRENT endpoint (the stale endpoint
 * is gone); reports EXACTLY the declared manifest (no accumulated authority);
 * resolves a fresh fault.  Invariants: G14, G16, G28, plus G2–G4 across
 * restart. */
void test_t205(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager restart";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T205", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    /* Two fresh targets: gen1 resolves g1 (which then exits), gen2 resolves a
     * NEW target g2 (a resolved target runs to completion, so each generation
     * needs its own live target). */
    struct t25_tgt g1, g2;
    if (ok && !t25_tgt_spawn(&g1, &why)) { t26_grant_close(&vmo); it_fail("T205", why); return; }
    if (ok && !t25_tgt_spawn(&g2, &why)) { t25_tgt_reap(&g1); t26_grant_close(&vmo); it_fail("T205", why); return; }
    handle_id_t vmos1[1] = { vmo };

    /* Gen 1: register + resolve g1 once. */
    struct t27_pager p1;
    if (ok && !t27_pager_spawn(&p1, &g1, 1u, vmos1, 1u, 0u, 1, &why)) { ok = 0; }
    if (ok && !t27_resolve_read(&p1, &g1, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;
    long gen1_reg = ok ? p1.reg_id : -1;

    /* Kill gen 1; the stale endpoint no longer serves. */
    if (ok && it_kill((long)p1.proc) != 0) { ok = 0; why = "kill gen1"; }
    if (ok && it_lp_wait_exit(p1.proc) != 0) { ok = 0; why = "gen1 exit"; }
    if (gen1_reg >= 0) { (void)it_unregister((uint32_t)gen1_reg); p1.reg_id = -1; }
    it_close(&p1.proc); it_close(&p1.ctrl_ep);
    it_quiesce_reaper();
    /* The old "pager.svc" name no longer resolves (unregistered on death). */
    if (ok && it_lookup_rights((long)IRIS_CPTR_SVCMGR_EP, "pager.svc")
              != -(long)(uint32_t)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale endpoint served"; }

    /* Gen 2: restart with the same manifest (over g2), re-register. */
    struct t27_pager p2;
    if (ok && !t27_pager_spawn(&p2, &g2, 1u, vmos1, 1u, 0u, 1, &why)) { ok = 0; }
    if (ok && p2.reg_id < 0) { ok = 0; why = "gen2 not registered"; }

    /* Manifest is exactly the declaration — restart amplified nothing. */
    if (ok) {
        long mask = t27_pager_call(p2.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        uint32_t expect = (1u << PGR_SLOT_CTRL_EP) | (1u << PGR_SLOT_FAULT_EP) |
                          (1u << PGR_SLOT_FAULT_CN) /* Stage 7 Step 7: the fault
                              * mailbox.  Real authority — the CNode a fault
                              * delivers the faulting thread into — so the
                              * oracle counts it rather than being blind to it,
                              * which is the same reason slot 15 is here. */ |
                          (1u << 13) /* Phase S1: explicit reply object */ |
                          (1u << 15) /* Stage 4: the pager's own VSpace, now a cap */ |
                          (1u << IRIS_CPTR_OWN_UNTYPED) /* Stage 6-pure Step 2: the
                              * budget its own address space was built from.  The
                              * pager MAPS, and the kernel no longer creates paging
                              * levels, so it must be able to retype one.  A real
                              * authority, which is why it belongs in this oracle. */ |
                          (1u << IRIS_CPTR_OWN_VSPACE) |
                          (1u << IRIS_CPTR_OWN_TCB) /* D-6: its own address
                              * space and its own thread, DELEGATED by its
                              * spawner instead of fabricated with
                              * SYS_VSPACE_SELF / SYS_TCB_SELF, which publish
                              * capabilities with no MDB parent that no revoke
                              * can reach.  Not new authority — a thread could
                              * always name both — but they are now in the
                              * oracle, because a capability that exists in a
                              * slot is authority whoever reads this must
                              * account for. */ |
                          PGR_REPORT_GRANT | (1u << 21);
        /* Stage 7 Step 8: bit 20 (any target PROCESS capability) is GONE.  A
         * pager maps and answers faults; both name the address space and the
         * thread, and neither names the process. */
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "restart manifest"; }
        if (ok && ((uint32_t)mask & ((1u<<6)|(1u<<24)|(1u<<26)|(1u<<27))) != 0) {
            ok = 0; why = "restart gained authority"; }
    }
    /* Gen 2 resolves a fresh fault on g2. */
    if (ok && !t27_resolve_read(&p2, &g2, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    t27_pager_reap(&p2);
    t25_tgt_reap(&g1); t25_tgt_reap(&g2);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T205"); else it_fail("T205", why);
}
void test_t206(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager crash-loop";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T206", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T206", why); return; }
    handle_id_t vmos[1] = { vmo };

    /* Target faults; it will stay pending across the whole crash-loop. */
    struct it_fault f;
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "fault"; }
    if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "pending"; }

    /* Supervision loop: each generation dies immediately (modelled by kill
     * right after start); stop at the limit, mark degraded. */
    uint32_t restart_count = 0u, generation = 0u;
    int degraded = 0;
    while (ok && !degraded) {
        struct t27_pager pg;
        if (!t27_pager_spawn(&pg, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
        generation++;
        it_settle(1);
        if (it_kill((long)pg.proc) != 0) { ok = 0; why = "kill"; }
        if (ok && it_lp_wait_exit(pg.proc) != 0) { ok = 0; why = "gen exit"; }
        it_close(&pg.proc); it_close(&pg.ctrl_ep);
        it_quiesce_reaper();
        if (restart_count < T206_LIMIT) restart_count++;
        else degraded = 1;
    }
    if (ok && restart_count != T206_LIMIT) { ok = 0; why = "wrong restart count"; }
    if (ok && !degraded) { ok = 0; why = "never degraded"; }
    if (ok && generation != T206_LIMIT + 1u) { ok = 0; why = "generation mismatch"; }

    /* The fault survived every crash; supervisor resolves with its authority. */
    struct it_fault f2;
    if (ok && (it_fault_info(g.fault_leaf, &f2) != 0 || f2.seq != f.seq)) { ok = 0; why = "fault lost"; }
    if (ok && t25_resume_seq(&g, f2.task_id, f2.seq, 1) != 0) { ok = 0; why = "supervisor resolve"; }
    if (ok && it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "target exit"; }

    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T206"); else it_fail("T206", why);
}

/* ── T207: multiple targets under one pager ─────────────────────────────────
 * One pager holds explicit grants for target A (grant 0) and target B (grant
 * 1).  Both fault; the pager resolves each from the shared VMO into the RIGHT
 * VSpace by index.  A dies; B still resolves.  Grants never mix: the fault info
 * and the mappings are per-target.  Invariants: G20, G17, G25, G26. */
void test_t207(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "multi-target";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T207", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt ga, gb;
    if (ok && !t25_tgt_spawn(&ga, &why)) { t26_grant_close(&vmo); it_fail("T207", why); return; }
    if (ok && !t25_tgt_spawn(&gb, &why)) { t25_tgt_reap(&ga); t26_grant_close(&vmo); it_fail("T207", why); return; }
    struct t25_tgt tg[2] = { ga, gb };

    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, tg, 2u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* Resolve A via grant 0, B via grant 1 — both read the same VMO page. */
    if (ok && !t27_resolve_read(&p, &tg[0], 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;
    /* A has exited; B still resolves through its own grant. */
    if (ok && !t27_resolve_read(&p, &tg[1], 1u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    t27_pager_reap(&p);
    t25_tgt_reap(&tg[0]); t25_tgt_reap(&tg[1]);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T207"); else it_fail("T207", why);
}

/* ── T208: multiple VMOs under one pager ────────────────────────────────────
 * One pager holds two VMO grants (VMO0 read-only, VMO1 writable).  The target
 * faults twice; the pager backs region A from VMO0 and region B from VMO1.  The
 * backings never mix: A reads VMO0's pattern, B's store lands in VMO1 (and not
 * VMO0).  PTE rights are per-VMO grant.  Invariants: G21, G12, G24, G26. */
void test_t208(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "multi-vmo";

    handle_id_t vmo0 = t26_grant();
    handle_id_t vmo1 = t26_grant();
    if (vmo0 == HANDLE_INVALID || vmo1 == HANDLE_INVALID) {
        t26_grant_close(&vmo0); t26_grant_close(&vmo1); it_fail("T208", "vmo create"); return;
    }
    word = T27_PAT;  if (ok && t26_page_word(T26_AT(vmo0, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo0 fill"; }
    word = 0;        if (ok && t26_page_word(T26_AT(vmo1, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo1 zero"; }

    /* One pager, two targets, two VMO grants: target A reads from VMO0 (grant
     * 0, RO), target B writes into VMO1 (grant 1, writable). */
    struct t25_tgt ga, gb;
    if (ok && !t25_tgt_spawn(&ga, &why)) { t26_grant_close(&vmo0); t26_grant_close(&vmo1); it_fail("T208", why); return; }
    if (ok && !t25_tgt_spawn(&gb, &why)) { t25_tgt_reap(&ga); t26_grant_close(&vmo0); t26_grant_close(&vmo1); it_fail("T208", why); return; }
    struct t25_tgt tg[2] = { ga, gb };
    struct t27_pager p;
    handle_id_t vmos[2] = { vmo0, vmo1 };
    if (ok && !t27_pager_spawn(&p, tg, 2u, vmos, 2u, (1u << 1), 0, &why)) { ok = 0; }

    /* A: read VMO0 grant into target A → reads the pattern. */
    if (ok && !t27_resolve_read(&p, &tg[0], 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    /* B: write VMO1 grant into target B → store lands in VMO1. */
    if (ok && it_lp_cmd_va(tg[1].cmd, LP_CMD_FAULT_WRITE, T27_VA_B) != 0) { ok = 0; why = "B fault"; }
    if (ok && t27_pager_call(p.ctrl_ep, PGR_OP_MAP_RESUME, 1u /*target B*/, 1u /*vmo1*/, 1u /*W*/, 0x1000ULL, T27_VA_B) != 0) {
        ok = 0; why = "B resolve"; }
    if (ok && it_lp_wait_exit(tg[1].proc) != LP_EXIT_MARKER) { ok = 0; why = "B store"; }

    it_quiesce_reaper();
    /* The store landed in VMO1, and VMO0 is untouched (backings did not mix). */
    if (ok && (t26_page_word(T26_AT(vmo1, 0x1000ULL), &word, 0) != 0 || word != T27_WMARK)) { ok = 0; why = "vmo1 not stored"; }
    if (ok && (t26_page_word(T26_AT(vmo0, 0x1000ULL), &word, 0) != 0 || word != T27_PAT)) { ok = 0; why = "vmo0 disturbed"; }

    t27_pager_reap(&p);
    t25_tgt_reap(&tg[0]); t25_tgt_reap(&tg[1]);
    t26_grant_close(&vmo0); t26_grant_close(&vmo1);
    it_quiesce_reaper();
    if (ok && it_frame_live() != vlive0) { ok = 0; why = "vmo live drift"; }
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T208"); else it_fail("T208", why);
}

/* ── T209: pager service death with pending faults ──────────────────────────
 * The pager dies with the target's fault pending (never resolved).  The target
 * is not a zombie: it stays suspended-alive with its record and generation
 * intact, resolvable by another authority (a restarted pager or the
 * supervisor).  The dead pager's control endpoint has no phantom receiver; no
 * endpoint/notification/KReply leak.  Invariants: G16, G18, G23, G27. */
void test_t209(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager death pending";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T209", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T209", why); return; }
    handle_id_t vmos[1] = { vmo };

    /* Target faults; pager spawned in charge but killed before it serves. */
    uint32_t d0 = t25_delivered_now();
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "fault"; }
    if (ok && !t25_wait_delivered(d0)) { ok = 0; why = "pending"; }

    struct t27_pager p1;
    if (ok && !t27_pager_spawn(&p1, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }
    handle_id_t p1ctrl = ok ? p1.ctrl_ep : HANDLE_INVALID;
    if (ok && it_kill((long)p1.proc) != 0) { ok = 0; why = "kill pager"; }
    if (ok && it_lp_wait_exit(p1.proc) != 0) { ok = 0; why = "pager exit"; }
    it_close(&p1.proc);
    it_quiesce_reaper();

    /* Target not a zombie: suspended-alive, record + generation intact. */
    if (ok && it_invoke0(it_child_tcb((long)g.proc), INV_TCB_EXIT_CODE) != (long)IRIS_ERR_WOULD_BLOCK) {
        ok = 0; why = "target not suspended"; }
    /* A-22: the fault outliving its handler is proved by the restart serving
     * it, below. */
    /* Dead pager's control endpoint has no phantom receiver. */
    if (ok) {
        struct IrisMsg m;
        it_iris_msg_zero(&m);
        m.words[0] = PGR_PACK(PGR_OP_PING, 0, 0, 0);
        m.word_count = 1u;
        if (it_invoke1((long)p1ctrl, INV_EP_NB_SEND, (long)&m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "phantom receiver"; }
    }
    it_close(&p1.ctrl_ep);

    /* A restarted pager (same manifest) completes the resolution. */
    struct t27_pager p2;
    if (ok && !t27_pager_spawn(&p2, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }
    if (ok) {
        long res = t27_pager_call(p2.ctrl_ep, PGR_OP_MAP_RESUME, 0u, 0u, 0u, 0x1000ULL, T27_VA_A);
        if (res != 0) { ok = 0; why = "restart resolve"; }
        if (ok && it_lp_wait_exit(g.proc) != (long)(LP_EXIT_MARKER ^ (T27_PAT & 0xFFu))) {
            ok = 0; why = "target completion"; }
    }

    t27_pager_reap(&p2);
    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T209"); else it_fail("T209", why);
}
uint32_t t210_rnd(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
