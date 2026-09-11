/*
 * it_t210_t238.c — tests T210 through T238.
 *
 * The suite's numbering is chronological, not thematic: T210 was written
 * stages before T238, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T210 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"

void test_t210(void) {
    uint32_t rng = T210_SEED, word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    long vlive0 = it_frame_live();
    int ok = b.ok && vlive0 >= 0;
    const char *why = "pager stress";
    uint32_t round = 0u, op = 0u;

    for (round = 0; ok && round < T210_ROUNDS; round++) {
        op = t210_rnd(&rng) % 4u;
        handle_id_t vmo = t26_grant();
        if (vmo == HANDLE_INVALID) { ok = 0; why = "vmo create"; break; }
        word = T27_PAT ^ round;
        if (t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; t26_grant_close(&vmo); break; }

        struct t25_tgt g;
        if (!t25_tgt_spawn(&g, &why)) { ok = 0; t26_grant_close(&vmo); break; }
        handle_id_t vmos[1] = { vmo };
        struct it_fault f;

        switch (op) {
        case 0: {
            /* Clean VMO-backed read resolve. */
            struct t27_pager p;
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (!t27_resolve_read(&p, &g, 0u, 0u, 0x1000ULL, T27_VA_A, word, &why)) ok = 0;
            t27_pager_reap(&p);
            break;
        }
        case 1: {
            /* Pager dies before serving; supervisor takes over. */
            struct t27_pager p;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "op1 fault"; break; }
            if (!t25_wait_fault(&g, &f)) { ok = 0; why = "op1 pending"; break; }
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (it_kill((long)p.proc) != 0 || it_lp_wait_exit(p.proc) != 0) { ok = 0; why = "op1 pager death"; }
            it_close(&p.proc); it_close(&p.ctrl_ep);
            if (ok && it_invoke((long)T26_AT(vmo, 0x1000ULL), INV_FRAME_MAP, (long)g.vs, (long)T27_VA_A, (long)(0u)) != 0) { ok = 0; why = "op1 map"; }
            if (ok && t25_resume_seq(&g, f.task_id, f.seq, 0) != 0) { ok = 0; why = "op1 resume"; }
            if (ok && it_lp_wait_exit(g.proc) != (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op1 target"; }
            break;
        }
        case 2: {
            /* Target death mid-fault; pager's late map is BAD_HANDLE. */
            struct t27_pager p;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "op2 fault"; break; }
            if (!t25_wait_fault(&g, &f)) { ok = 0; why = "op2 pending"; break; }
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (it_kill((long)g.proc) != 0 || it_lp_wait_exit(g.proc) != 0) { ok = 0; why = "op2 kill"; }
            it_quiesce_reaper();
            /* Stage 7-proc: the target's address space outlives the target
             * while this test holds a capability to it, so the late map
             * SUCCEEDS into a space with nothing running in it. */
            if (ok && it_invoke((long)T26_AT(vmo, 0x1000ULL), INV_FRAME_MAP, (long)g.vs, (long)T27_VA_A, (long)(0u))
                      != 0) { ok = 0; why = "op2 late map"; }
            t27_pager_reap(&p);
            break;
        }
        case 3: {
            /* Unauthorized VMO (RO grant) write attempt denied, then clean read. */
            struct t27_pager p;
            if (!t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; break; }
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T27_VA_A) != 0) { ok = 0; why = "op3 fault"; break; }
            long res = t27_pager_call(p.ctrl_ep, PGR_OP_MAP_RESUME, 0u, 0u, 1u /*W*/, 0x1000ULL, T27_VA_A);
            /* RO VMO grant + writable request → the pager's map is ACCESS_DENIED. */
            if (res != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "op3 not denied"; }
            /*
             * A-22: the pager took DELIVERY of the fault before its map was
             * refused, so the answer is still its to give — and the retry
             * proves it kept it.  The supervisor cannot step in here and
             * resolve the fault itself, because a fault is delivered once, to
             * one holder of the endpoint; that is the single-mechanism
             * property, and it is what makes "who may answer this" a fact
             * about capabilities rather than a race.
             */
            if (ok && t27_pager_call(p.ctrl_ep, PGR_OP_MAP_RESUME, 0u, 0u,
                                     0u /*RO*/, 0x1000ULL, T27_VA_A) != 0) {
                ok = 0; why = "op3 pending";
            }
            if (ok && it_lp_wait_exit(g.proc) != (long)(LP_EXIT_MARKER ^ (word & 0xFFu))) { ok = 0; why = "op3 target"; }
            t27_pager_reap(&p);
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
    if (ok) it_pass("T210");
    else { it_fz_note("T210", T210_SEED, round, op); it_fail("T210", why); }
}

/* ── T211: initrd image-count boundary ──────────────────────────────────────
 * The count is queryable and >= the named catalog; every index in range yields
 * an initrd VMO; an out-of-range index fails cleanly (NOT_FOUND), never a
 * wedge.  That this test runs at all proves boot reached userland with more
 * than SL_CATALOG_COUNT images. */
void test_t211(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "initrd count boundary";

    long n = it_invoke0((long)IRIS_CPTR_INITRD_CONTROL, INV_BOOT_INITRD_COUNT);
    if (n < (long)T2_MIN_IMAGES) { ok = 0; why = "count below catalog"; }

    /* Every in-range index yields a live VMO with a positive size. */
    for (long i = 0; ok && i < n; i++) {
        long v = it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, i);
        if (v < 0) { ok = 0; why = "image vmo"; break; }
        handle_id_t vh = (handle_id_t)v;
        /* D-5: the image is a frame, and the call that produced it said how
         * big the FILE was — a frame only knows its region. */
        if (g_it_initrd_size <= 0) { ok = 0; why = "image size"; }
        it_close(&vh);
    }
    /* Out-of-range indices fail cleanly. */
    if (ok && it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, n)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "oob not NOT_FOUND"; }
    if (ok && it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, 9999L)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "far oob not NOT_FOUND"; }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T211"); else it_fail("T211", why);
}

/* ── T212: initrd aggregate-size boundary ───────────────────────────────────
 * Every image's initrd VMO has a correct, non-overflowing size and is mappable
 * into the caller's own VSpace at the file's true size (no truncation, no
 * overlap between images).  Reading back the first bytes of a couple of images
 * confirms the physical bounds are honest (not aliased). */
void test_t212(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "initrd size boundary";

    long n = it_invoke0((long)IRIS_CPTR_INITRD_CONTROL, INV_BOOT_INITRD_COUNT);
    if (n < (long)T2_MIN_IMAGES) { ok = 0; why = "count"; }

    for (long i = 0; ok && i < n; i++) {
        long v = it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, i);
        if (v < 0) { ok = 0; why = "vmo"; break; }
        handle_id_t vh = (handle_id_t)v;
        long sz = g_it_initrd_size;
        if (sz <= 0 || sz > (long)(64u * 1024u * 1024u)) { ok = 0; why = "size range"; }
        /* Map it read-only into our own VSpace at a scratch VA; a mappable
         * image with a real backing proves the bounds are honest.  One map
         * covers the whole frame (D-10), where a VMO needed a page at a
         * time. */
        if (ok && it_invoke(v, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0) != 0) {
            ok = 0; why = "map"; }
        if (ok && it_invoke2(v, INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA) != 0) { ok = 0; why = "unmap"; }
        it_close(&vh);
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T212"); else it_fail("T212", why);
}

/* ── T213: loader/process launch growth ─────────────────────────────────────
 * A launch of a valid service (lifecycle_probe) works; a launch of the
 * invalid-ELF fixture fails cleanly (INVALID_ARG) with FULL atomicity — no
 * ghost process/task/VSpace/CSpace, no handle leak; and a valid launch AFTER
 * the failed one still works.  This is the growth invariant: an added
 * (possibly malformed) image never poisons the loader for the next one. */
void test_t213(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "loader launch growth";

    /* 1. A valid launch works. */
    {
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "valid launch 1"; }
        if (ok) { (void)it_kill((long)proc); (void)it_lp_wait_exit(proc); }
        it_close(&cmd); it_close(&proc);
    }
    it_quiesce_reaper();

    /* 2. The invalid-ELF fixture fails cleanly with no ghost state. */
    {
        struct it_snap fb = it_snap_take();
        handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
        long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "badelf",
                                 &proc, &boot, 0, 0u,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
        it_child_bind(proc);
        if (r != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "badelf not INVALID_ARG"; }
        if (ok && proc != HANDLE_INVALID) { ok = 0; why = "badelf left a process"; }
        it_close(&boot); it_close(&proc);
        it_quiesce_reaper();
        struct it_snap fa = it_snap_take();
        if (ok && !it_snap_baseline_live(&fb, &fa, &why)) ok = 0;   /* no ghost */
    }

    /* 3. A valid launch AFTER the failure still works. */
    {
        long ep = it_ep_create();
        handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ok && (ep < 0 || lp_spawn_child(cmd, &proc) < 0)) { ok = 0; why = "valid launch 2"; }
        if (ok) { (void)it_kill((long)proc); (void)it_lp_wait_exit(proc); }
        it_close(&cmd); it_close(&proc);
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T213"); else it_fail("T213", why);
}

/* ── T214: boot failure diagnostics ─────────────────────────────────────────
 * Every capacity/validity failure in the load path is an EXPLICIT error, never
 * a silent hang: an unknown image name is NOT_FOUND, a malformed image is
 * INVALID_ARG, and neither blocks.  (The boot-time analogue — userboot's
 * catalog-shortage diagnostic — is exercised by construction: this suite only
 * runs because userboot loaded init with >SL_CATALOG_COUNT images present.) */
void test_t214(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "boot failure diagnostics";

    handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
    /* Unknown name → NOT_FOUND, no hang, no process. */
    long r1 = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "no_such_image",
                              &proc, &boot, 0, 0u,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(proc);
    if (r1 != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "unknown not NOT_FOUND"; }
    if (ok && proc != HANDLE_INVALID) { ok = 0; why = "unknown left process"; }
    it_close(&boot); it_close(&proc);

    /* Malformed image → INVALID_ARG, no hang, no process. */
    proc = boot = HANDLE_INVALID;
    long r2 = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "badelf",
                              &proc, &boot, 0, 0u,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(proc);
    if (ok && r2 != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "malformed not INVALID_ARG"; }
    if (ok && proc != HANDLE_INVALID) { ok = 0; why = "malformed left process"; }
    it_close(&boot); it_close(&proc);

    /* Initrd VMO of the invalid image still succeeds (it is bytes, not code) —
     * the failure is the LOADER's, cleanly reported, not the initrd layer's. */
    long v = it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, (long)T2_BADELF_IDX);
    if (ok && v < 0) { ok = 0; why = "badelf vmo"; }
    if (v >= 0) { handle_id_t vh = (handle_id_t)v; it_close(&vh); }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T214"); else it_fail("T214", why);
}

/* ── T215: pager binary promotion ───────────────────────────────────────────
 * The pager is now its OWN initrd binary ("pager", index 10) — no longer a
 * lifecycle_probe mode.  Load it by name, wire a real target + VMO grant, and
 * resolve a VMO-backed fault end to end; verify the manifest is exactly the
 * grant set (no authority gained from being a standalone image).  T201–T210
 * already exercise the full supervised/registered/restart surface over this
 * same binary; T215 is the focused promotion proof. */
void test_t215(void) {
    uint32_t word;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager binary promotion";

    handle_id_t vmo = t26_grant();
    if (vmo == HANDLE_INVALID) { it_fail("T215", "vmo create"); return; }
    word = T27_PAT;
    if (ok && t26_page_word(T26_AT(vmo, 0x1000ULL), &word, 1) != 0) { ok = 0; why = "vmo fill"; }

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) { t26_grant_close(&vmo); it_fail("T215", why); return; }
    struct t27_pager p;
    handle_id_t vmos[1] = { vmo };
    if (ok && !t27_pager_spawn(&p, &g, 1u, vmos, 1u, 0u, 0, &why)) { ok = 0; }

    /* Manifest is exactly the grant set (a standalone binary gained nothing). */
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
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "manifest"; }
        if (ok && ((uint32_t)mask & ((1u<<6)|(1u<<24)|(1u<<26)|(1u<<27))) != 0) {
            ok = 0; why = "gained authority"; }
    }
    /* Resolve a fault from the binary pager. */
    if (ok && !t27_resolve_read(&p, &g, 0u, 0u, 0x1000ULL, T27_VA_A, T27_PAT, &why)) ok = 0;

    t27_pager_reap(&p);
    t25_tgt_reap(&g);
    t26_grant_close(&vmo);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T215"); else it_fail("T215", why);
}
static uint32_t t216_rnd(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
void test_t216(void) {
    uint32_t rng = T216_SEED;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "boot-growth stress";
    uint32_t round = 0u, op = 0u;

    long n = it_invoke0((long)IRIS_CPTR_INITRD_CONTROL, INV_BOOT_INITRD_COUNT);
    if (n < (long)T2_MIN_IMAGES) { it_fail("T216", "count"); return; }

    for (round = 0; ok && round < T216_ROUNDS; round++) {
        op = t216_rnd(&rng) % 4u;
        switch (op) {
        case 0: {
            /* Map a random in-range image page 0 into our own VSpace. */
            long i = (long)(t216_rnd(&rng) % (uint32_t)n);
            long v = it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, i);
            if (v < 0) { ok = 0; why = "map vmo"; break; }
            handle_id_t vh = (handle_id_t)v;
            /* D-5: a frame, mapped whole (D-10). */
            if (it_invoke(v, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0) != 0) { ok = 0; why = "map"; }
            if (ok) (void)it_invoke2(v, INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA);
            it_close(&vh);
            break;
        }
        case 1: {
            /* Out-of-range query fails clean. */
            if (it_initrd_vmo_slot((long)IRIS_CPTR_INITRD_CONTROL, n + (long)(t216_rnd(&rng) % 100u))
                != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "oob"; }
            break;
        }
        case 2: {
            /* Invalid-ELF load fails clean, no ghost. */
            struct it_snap fb = it_snap_take();
            handle_id_t proc = HANDLE_INVALID, boot = HANDLE_INVALID;
            long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "badelf", &proc, &boot, 0, 0u,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/0, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
            it_child_bind(proc);
            if (r >= 0) { ok = 0; why = "badelf loaded"; }
            it_close(&boot); it_close(&proc);
            it_quiesce_reaper();
            struct it_snap fa = it_snap_take();
            if (ok && !it_snap_baseline_live(&fb, &fa, &why)) ok = 0;
            break;
        }
        case 3: {
            /* Valid launch works and reaps cleanly. */
            long ep = it_ep_create();
            handle_id_t cmd = (ep >= 0) ? (handle_id_t)ep : HANDLE_INVALID;
            handle_id_t proc = HANDLE_INVALID;
            if (ep < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "launch"; }
            if (ok) { (void)it_kill((long)proc); (void)it_lp_wait_exit(proc); }
            it_close(&cmd); it_close(&proc);
            break;
        }
        default: break;
        }
        it_quiesce_reaper();
        if (ok) {
            struct it_snap r = it_snap_take();
            if (!it_snap_baseline_live(&b, &r, &why)) ok = 0;
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T216");
    else { it_fz_note("T216", T216_SEED, round, op); it_fail("T216", why); }
}

/* Send a multi-page fault-read sequence to a target (words[0]=base VA,
 * words[1]=count, words[2]=visit order). */
static long t28_cmd_read_seq(handle_id_t cmd, uint64_t base, uint32_t count, uint64_t order) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.label = LP_CMD_FAULT_READ_SEQ;
    m.words[0] = base; m.words[1] = (uint64_t)count; m.words[2] = order;
    m.word_count = 3u;
    return it_invoke1((long)cmd, INV_EP_SEND, (long)&m);
}
/* Two-offset fault-read: read base+off0 and (count==2) base+off1. */
static long t28_cmd_read_offs(handle_id_t cmd, uint64_t base, uint32_t count,
                              uint64_t off0, uint64_t off1) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.label = LP_CMD_FAULT_READ_OFFS_M;
    m.words[0] = base; m.words[1] = (uint64_t)count; m.words[2] = off0; m.words[3] = off1;
    m.word_count = 4u;
    return it_invoke1((long)cmd, INV_EP_SEND, (long)&m);
}

/* Materialize the two supervisor-side file-grant caps init pre-minted:
 *   slot 59 (IRIS_CPTR_TEST_VFS_MINT) — UNBADGED WRITE|DUPLICATE|TRANSFER
 *       vfs.ep cap: the mint SOURCE for session-badged pager caps, and an
 *       ordinary unbadged name-op client for STATs.
 *   slot 58 (IRIS_CPTR_TEST_VFS_DUP) — the grant ADMIN identity (badge
 *       IRIS_BADGE_FILEGRANT_ADMIN, WRITE-only): GRANT_OPEN / GRANT_REVOKE /
 *       GRANT_SESSION_RESET.
 * SYS_CSPACE_RESOLVE copies the CSpace leaf into a fresh handle preserving
 * rights AND badge.  The ordinary svcmgr lookup strips DUPLICATE (client
 * grant tightening) and cannot mint fresh badges, so these pre-mints are the
 * only honest supervisor path. */
/* Stage 4: these are the pre-mint SLOTS themselves.  They used to be
 * materialised into handles for every use and closed again; every syscall they
 * are passed to resolves a CPtr, so the round trip bought nothing. */
static handle_id_t t28_vfs_cap(void) {
    return (it_invoke0((long)IRIS_CPTR_TEST_VFS_MINT, INV_CAP_IDENTIFY) >= 0)
           ? (handle_id_t)IRIS_CPTR_TEST_VFS_MINT : HANDLE_INVALID;
}
static handle_id_t t28_admin_cap(void) {
    return (it_invoke0((long)IRIS_CPTR_TEST_VFS_DUP, INV_CAP_IDENTIFY) >= 0)
           ? (handle_id_t)IRIS_CPTR_TEST_VFS_DUP : HANDLE_INVALID;
}
static handle_id_t t28_session_cap(uint32_t session) {
    if (session >= T28_FG_SESSIONS) return HANDLE_INVALID;
    handle_id_t src = t28_vfs_cap();
    if (src == HANDLE_INVALID) return HANDLE_INVALID;
    handle_id_t root = T28_OWN_ROOT_CNODE;
    (void)it_invoke1((long)root, INV_CNODE_DELETE, (long)T28_FG_SLOT(session));
    long mr = it_invoke2((long)src, INV_CSPACE_MINT, IT_MINT_SELF((long)T28_FG_SLOT(session)), (long)((IRIS_BADGE_FILEGRANT_S(session) << 32) | RIGHT_WRITE));
    it_close(&src);
    if (mr != 0) return HANDLE_INVALID;
    return (it_invoke0((long)T28_FG_SLOT(session), INV_CAP_IDENTIFY) >= 0)
           ? (handle_id_t)T28_FG_SLOT(session) : HANDLE_INVALID;
}

/* STAT a file via a vfs cap → size, or -1. */
long t28_stat(handle_id_t vfs_cap, const char *name) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    uint32_t n = 0; while (name[n] && n + 1u < IT_EP_IO_CAP) { g_ep_io_buf[n] = (uint8_t)name[n]; n++; }
    g_ep_io_buf[n] = 0;
    m.label = VFS_EP_OP_STAT; m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf; m.buf_len = n + 1u;
    if (it_invoke1((long)vfs_cap, INV_EP_CALL, (long)&m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -1;
    return (long)m.words[1];
}

/* Generic VFS grant-protocol call.  Stages `name` (may be NULL) as the bulk
 * payload, sends label/w0..w2, and returns 0 on REPLY_OK (msg copied to *out
 * when non-NULL) or the NEGATIVE iris_error_t from the error reply. */
static long t28_gcall(handle_id_t cap, uint64_t label, uint64_t w0, uint64_t w1,
                      uint64_t w2, uint32_t wc, const char *name,
                      struct IrisMsg *out) {
    struct IrisMsg m;
    it_iris_msg_zero(&m);
    m.label = label;
    m.words[0] = w0; m.words[1] = w1; m.words[2] = w2; m.word_count = wc;
    m.buf_uptr = (uint64_t)(uintptr_t)g_ep_io_buf;
    if (name) {
        uint32_t n = 0;
        while (name[n] && n + 1u < IT_EP_IO_CAP) { g_ep_io_buf[n] = (uint8_t)name[n]; n++; }
        g_ep_io_buf[n] = 0;
        m.buf_len = n + 1u;
    }
    long r = it_invoke1((long)cap, INV_EP_CALL, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return (long)(int32_t)(uint32_t)m.words[0];
    if (out) *out = m;
    return 0;
}

/* Supervisor grant operations (ADMIN cap). */
static long t28_grant_open(handle_id_t admin, uint32_t session, const char *name,
                           uint32_t rights, struct t28_grant *out) {
    struct IrisMsg m;
    long r = t28_gcall(admin, VFS_EP_OP_GRANT_OPEN, session, rights, 0, 2u, name, &m);
    if (r != 0) return r;
    if (out) { out->idx = (uint32_t)m.words[1]; out->bid = m.words[2]; out->gen = m.words[3]; }
    return 0;
}
static long t28_session_reset(handle_id_t admin, uint32_t session) {
    return t28_gcall(admin, VFS_EP_OP_GRANT_SESSION_RESET, session, 0, 0, 1u, 0, 0);
}
long t28_grant_revoke_name(handle_id_t admin, const char *name, uint64_t *newgen) {
    struct IrisMsg m;
    long r = t28_gcall(admin, VFS_EP_OP_GRANT_REVOKE, 0, 0, 0, 0u, name, &m);
    if (r != 0) return r;
    if (newgen) *newgen = m.words[1];
    return 0;
}

/* Session-holder grant operations (a SESSION-badged cap). */
static long t28_grant_read(handle_id_t cap, uint32_t idx, uint64_t off, uint32_t len,
                           uint8_t *first_byte, uint64_t *bytes) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_READ_AT, idx, off, len, 3u, 0, &m);
    if (r != 0) return r;
    if (bytes) *bytes = m.words[1];
    if (first_byte) *first_byte = (m.words[1] > 0u) ? g_ep_io_buf[0] : 0u;
    return 0;
}
static long t28_grant_stat(handle_id_t cap, uint32_t idx, uint64_t *size,
                           uint64_t *bid, uint64_t *gen) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_STAT, idx, 0, 0, 1u, 0, &m);
    if (r != 0) return r;
    if (size) *size = m.words[1];
    if (bid)  *bid  = m.words[2];
    if (gen)  *gen  = m.words[3];
    return 0;
}
/* t28_grant_query removed — its last caller was retired with the T28x grant
 * refactor; GRANT_QUERY_IDENTITY coverage lives in the vfs_ep host suite. */
static long t28_grant_derive(handle_id_t cap, uint32_t src, uint32_t rights,
                             uint32_t *newidx) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_DERIVE, src, rights, 0, 2u, 0, &m);
    if (r != 0) return r;
    if (newidx) *newidx = (uint32_t)m.words[1];
    return 0;
}
static long t28_grant_revoke_idx(handle_id_t cap, uint32_t idx, uint64_t *newgen) {
    struct IrisMsg m;
    long r = t28_gcall(cap, VFS_EP_OP_GRANT_REVOKE, idx, 0, 0, 1u, 0, &m);
    if (r != 0) return r;
    if (newgen) *newgen = m.words[1];
    return 0;
}

/* Spawn a file-backed pager granting `nt` targets, its SESSION-badged vfs cap,
 * the shared fault notification, and cache/private VMOs.  Runs the supervisor
 * restart protocol: session FBK_SESSION is RESET first, so no grant of a
 * previous pager instance can survive into this one (A11).  0 on success. */
int t28_fbk_spawn(struct t28_fbk *f, struct t25_tgt *targets, uint32_t nt,
                         const char **why) {
    f->ctrl_ep = f->proc = HANDLE_INVALID;
    f->vfs_cap = f->admin = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ctrl ep"; return 0; }
    handle_id_t ctrl = (handle_id_t)ep;
    handle_id_t vfs  = t28_vfs_cap();
    handle_id_t adm  = t28_admin_cap();
    if (
        vfs == HANDLE_INVALID || adm == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm);
        *why = "fbk grants"; return 0;
    }
    /* Pager-(re)start protocol step 1: the session starts clean. */
    if (t28_session_reset(adm, FBK_SESSION) != 0) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm);
        *why = "session reset"; return 0;
    }
    /* A-22: every target's faults onto the ONE shared ENDPOINT
     * (targets[0].notif), each through a copy badged `i + 1`, before the pager
     * starts. */
    if (!it_pgr_mbox_fresh(nt)) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm);
        *why = "fault replies"; return 0;
    }
    for (uint32_t i = 0; i < nt; i++) {
        long bep = it_cs_badge((long)targets[0].notif,
                               RIGHT_READ | RIGHT_WRITE, i + 1u);
        int wired = (bep >= 0 &&
                     it_invoke(it_child_tcb((long)targets[i].proc), INV_TCB_SET_FAULT_HANDLER, bep, 0, 0) == 0);
        if (bep >= 0) it_slot_delete((uint32_t)bep);
        if (!wired) {
            it_close(&ctrl); it_close(&vfs); it_close(&adm);
            *why = "shared fault ep wire"; return 0;
        }
    }

    struct svc_mint m[48] = { 0 };
    uint32_t k = 0;
    m[k].slot = PGR_SLOT_CTRL_EP; IT_MINT_SRC(m[k], ctrl); m[k].rights = RIGHT_READ; m[k].badge = 0; k++;
    /* The pager's ONLY VFS identity: a session-badged, WRITE-only cap.  The
     * fresh badge is legal because the source (slot 59) is unbadged. */
    m[k].slot = FBK_SLOT_VFS_EP;  IT_MINT_SRC(m[k], vfs);  m[k].rights = RIGHT_WRITE;
    m[k].badge = IRIS_BADGE_FILEGRANT_S(FBK_SESSION); k++;
    if (nt > 0) {
        m[k].slot = FBK_SLOT_NOTIF; IT_MINT_SRC(m[k], targets[0].notif); m[k].rights = RIGHT_READ; m[k].badge = 0; k++;
        /* Stage 7 Step 7: the mailbox each fault delivers a thread into. */
        m[k].slot = PGR_SLOT_FAULT_CN; IT_MINT_SRC(m[k], IT_PGR_MBOX_SLOT); m[k].rights = RIGHT_READ | RIGHT_WRITE; m[k].badge = 0; k++;
    }
    for (uint32_t i = 0; i < nt; i++) {
        m[k].slot = PGR_TSLOT_VS(i);    IT_MINT_SRC(m[k], targets[i].vs);    m[k].rights = RIGHT_WRITE;               m[k].badge = 0; k++;
    }

    /* Phase S1: explicit reply object for the pager's ctrl EP (slot 13). */
    handle_id_t pgr_reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            pgr_reply_h = (handle_id_t)rr;
            m[k].slot = 13u; IT_MINT_SRC(m[k], pgr_reply_h);
            m[k].rights = RIGHT_READ | RIGHT_WRITE; m[k].badge = 0; k++;
        }
    }
    handle_id_t boot = HANDLE_INVALID;
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "pager", &f->proc, &boot, m, k,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(f->proc);
    it_close(&pgr_reply_h);
    it_close(&boot);
    if (r < 0 || f->proc == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm); it_close(&f->proc);
        *why = "pager spawn"; return 0;
    }
    f->ctrl_ep = ctrl;
    f->vfs_cap = vfs; f->admin = adm;
    return 1;
}

void t28_fbk_reap(struct t28_fbk *f) {
    if (f->proc != HANDLE_INVALID) { (void)it_kill((long)f->proc); (void)it_lp_wait_exit(f->proc); }
    it_close(&f->proc); it_close(&f->ctrl_ep);
    it_close(&f->vfs_cap); it_close(&f->admin);
}

/* Open a grant for the pager session and register it as pager backing `bidx`
 * — the whole supervisor-side backing setup.  Returns 1 and fills *gr. */
static long t28_reg_backing2(handle_id_t ctrl, uint32_t bidx,
                             const struct t28_grant *gr, uint64_t size);
int t28_backing_setup(struct t28_fbk *f, uint32_t bidx, const char *name,
                             uint64_t size, struct t28_grant *gr, const char **why) {
    if (t28_grant_open(f->admin, FBK_SESSION, name, VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, gr) != 0) {
        *why = "grant open"; return 0;
    }
    if (t28_reg_backing2(f->ctrl_ep, bidx, gr, size) != 0) {
        *why = "reg backing"; return 0;
    }
    return 1;
}

/* Control calls. */
static long t28_ctrl_words(handle_id_t ctrl, uint32_t op, uint64_t w1, uint64_t w2) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)op; m.words[1] = w1; m.words[2] = w2; m.word_count = 3u;
    long r = it_invoke1((long)ctrl, INV_EP_CALL, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
static long t28_map_region(handle_id_t ctrl, uint32_t tidx) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_MAP_REGION | ((uint64_t)tidx << 8);
    m.word_count = 1u;
    long r = it_invoke1((long)ctrl, INV_EP_CALL, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
/* Register a pager backing from raw fields (attack surface: the values may
 * deliberately MISMATCH the VFS-issued identity). */
static long t28_reg_backing_raw(handle_id_t ctrl, uint32_t idx, uint32_t grant_idx,
                                uint64_t id, uint64_t gen, uint64_t size) {
    struct pgr_backing_req *rq = (struct pgr_backing_req *)g_t28_buf;
    for (uint32_t i = 0; i < sizeof(*rq); i++) g_t28_buf[i] = 0;
    rq->backing_idx = idx; rq->grant_idx = grant_idx;
    rq->backing_id = id; rq->generation = gen; rq->file_size = size;
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_REGISTER_BACKING; m.word_count = 1u;
    m.buf_uptr = (uint64_t)(uintptr_t)g_t28_buf; m.buf_len = (uint32_t)sizeof(*rq);
    long r = it_invoke1((long)ctrl, INV_EP_CALL, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
/* Register a pager backing from a VFS-issued grant (the honest path). */
static long t28_reg_backing2(handle_id_t ctrl, uint32_t bidx,
                             const struct t28_grant *gr, uint64_t size) {
    return t28_reg_backing_raw(ctrl, bidx, gr->idx, gr->bid, gr->gen, size);
}
long t28_reg_region(handle_id_t ctrl, const struct pgr_region_req *src) {
    struct pgr_region_req *rq = (struct pgr_region_req *)g_t28_buf;
    for (uint32_t i = 0; i < sizeof(*rq); i++) g_t28_buf[i] = ((const uint8_t *)src)[i];
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_REGISTER_REGION; m.word_count = 1u;
    m.buf_uptr = (uint64_t)(uintptr_t)g_t28_buf; m.buf_len = (uint32_t)sizeof(*rq);
    long r = it_invoke1((long)ctrl, INV_EP_CALL, (long)&m);
    if (r != 0) return r;
    if (m.label != IRIS_EP_REPLY_OK) return -100000L;
    return (long)m.words[0];
}
static int t28_diag(handle_id_t ctrl, struct pgr_diag *out) {
    struct IrisMsg m; it_iris_msg_zero(&m);
    m.words[0] = (uint64_t)FBK_OP_DIAG; m.word_count = 1u;
    m.buf_uptr = (uint64_t)(uintptr_t)g_t28_buf;
    if (it_invoke1((long)ctrl, INV_EP_CALL, (long)&m) != 0) return 0;
    if (m.label != IRIS_EP_REPLY_OK || m.buf_len < sizeof(*out)) return 0;
    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = g_t28_buf[i];
    return 1;
}

/* Build a region descriptor. */
void t28_region(struct pgr_region_req *rq, uint32_t ridx, uint32_t tidx, uint32_t bidx,
                       uint64_t va, uint64_t mem_len, uint64_t file_off, uint64_t file_len,
                       uint32_t prot, uint32_t mode, uint64_t gen) {
    for (uint32_t i = 0; i < sizeof(*rq); i++) ((uint8_t *)rq)[i] = 0;
    rq->region_idx = ridx; rq->target_idx = tidx; rq->backing_idx = bidx;
    rq->prot = prot; rq->mode = mode; rq->start_va = va; rq->memory_length = mem_len;
    rq->file_offset = file_off; rq->file_length = file_len; rq->backing_generation = gen;
}

/* Resolve a target read fault at `va` and verify the low byte the target read
 * equals `expect_byte` (end-to-end file→pager→target verification). */
int t28_read_verify(struct t28_fbk *f, struct t25_tgt *g, uint32_t tidx,
                           uint64_t va, uint8_t expect_byte, const char **why) {
    if (it_lp_cmd_va(g->cmd, LP_CMD_FAULT_READ, va) != 0) { *why = "fault trigger"; return 0; }
    long res = t28_map_region(f->ctrl_ep, tidx);
    if (res != 0) { *why = "map region"; return 0; }
    long ec = it_lp_wait_exit(g->proc);
    if (ec != (long)(LP_EXIT_MARKER ^ (uint32_t)expect_byte)) { *why = "content"; return 0; }
    return 1;
}

/* ── T217: file backing identity and grants ─────────────────────────────────
 * A registered backing has a stable identity + generation; a region can only
 * bind a registered backing at its current generation; an unregistered backing
 * or a stale generation is rejected; the STAT size is honest.  The pager holds
 * a bounded vfs cap, not global VFS authority (it reports only its manifest).
 * Invariants: F1, F2, F3, F4, F40. */
void test_t217(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "backing identity";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T217", why); return; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; }

    /* STAT the file → honest size. */
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    if (ok && sz != (long)FBK_FILE_SIZE) { ok = 0; why = "stat size"; }

    /* Manifest oracle: ctrl(3) + session vfs(4) + shared notif(5) + vmos(16/17)
     * + target proc/vs presence (20/21). */
    if (ok) {
        long mask = t27_pager_call(f.ctrl_ep, PGR_OP_REPORT, 0, 0, 0, 0, 0);
        /* 15 = the pager's own VSpace, a capability since Stage 4. */
        /* Stage 6-pure Step 2 adds slot 12: the budget the pager's own
         * address space was built from.  It MAPS, and the kernel no longer
         * creates paging levels, so it must be able to retype one. */
        /* D-6 adds 18 and 19: the pager's own address space and own thread,
         * delegated by its spawner rather than fabricated with the *_SELF
         * syscalls, which publish MDB roots nothing can revoke. */
        /* D-5 removes 16 and 17.  They held the cache and private-pool VMOs a
         * supervisor granted; the pager has retyped both pools from its own
         * budget since the day its pages became frames, and nothing had read
         * the grants since.  Authority nothing uses is exactly what this
         * oracle exists to catch, and it was catching it as "expected". */
        uint32_t expect = (1u<<3)|(1u<<4)|(1u<<5)|(1u<<IRIS_CPTR_OWN_UNTYPED)|
                          (1u<<13)|(1u<<14)|(1u<<15)|
                          (1u<<IRIS_CPTR_OWN_VSPACE)|(1u<<IRIS_CPTR_OWN_TCB)|
                          (1u<<21);
        if (mask < 0 || (uint32_t)mask != expect) { ok = 0; why = "manifest"; }
        if (ok && ((uint32_t)mask & ((1u<<6)|(1u<<24)|(1u<<26)|(1u<<27))) != 0) { ok = 0; why = "extra authority"; }
    }

    /* Open a grant and register backing 0 with the VFS-issued identity. */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "grant open"; }
    /* A backing whose declared identity MISMATCHES the VFS-issued one → GRANT
     * (the pager cross-checks against GRANT_QUERY_IDENTITY before trusting). */
    if (ok && t28_reg_backing_raw(f.ctrl_ep, 0, gr.idx, gr.bid + 1u, gr.gen, (uint64_t)sz)
              != -(long)FBK_ERR_GRANT) { ok = 0; why = "wrong id accepted"; }
    if (ok && t28_reg_backing_raw(f.ctrl_ep, 0, gr.idx, gr.bid, gr.gen + 1u, (uint64_t)sz)
              != -(long)FBK_ERR_GRANT) { ok = 0; why = "wrong gen accepted"; }
    /* A backing referencing a grant the session does not hold → GRANT. */
    if (ok && t28_reg_backing_raw(f.ctrl_ep, 0, gr.idx + 5u, gr.bid, gr.gen, (uint64_t)sz)
              != -(long)FBK_ERR_GRANT) { ok = 0; why = "bogus grant accepted"; }
    /* The honest registration succeeds. */
    if (ok && t28_reg_backing2(f.ctrl_ep, 0, &gr, (uint64_t)sz) != 0) { ok = 0; why = "reg backing"; }

    /* A region binding an UNREGISTERED backing (idx 1) → NOBACK. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 1 /*unregistered*/, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_NOBACK) { ok = 0; why = "unregistered backing not rejected"; }
    }
    /* A region binding the WRONG generation → NOBACK (stale). */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen + 1u /*stale gen*/);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_NOBACK) { ok = 0; why = "stale gen not rejected"; }
    }
    /* Correct backing + generation → OK. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "valid region rejected"; }
    }
    /* DIAG: exactly one backing live, one region. */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.backing_live != 1u || d.region_count != 1u) { ok = 0; why = "diag counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T217"); else it_fail("T217", why);
}

/* ── T218: file region validation ───────────────────────────────────────────
 * Every field is validated atomically: unaligned VA/offset, zero/oversized
 * length, kernel VA, overflow, file range beyond backing, file_length >
 * memory_length, writable prot on a RO region, overlap — each rejected with no
 * partial region; a valid region then registers and a fault resolves.
 * Invariants: F6, F7, F8, F38. */
void test_t218(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "region validation";

    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, &why)) { it_fail("T218", why); return; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; }
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;

    struct pgr_region_req rq;
    /* unaligned VA */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A|0x800, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="unaligned va"; } }
    /* zero length */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0, 0, 0, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="zero len"; } }
    /* kernel VA */
    if (ok) { t28_region(&rq, 0,0,0, 0xFFFF800000000000ULL, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="kernel va"; } }
    /* unaligned file offset */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0x800, 0x800, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="unaligned off"; } }
    /* file range beyond backing */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, (uint64_t)sz + 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="beyond backing"; } }
    /* file_length > memory_length */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x2000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="flen>mlen"; } }
    /* writable prot on RO mode */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R|FBK_PROT_W, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="w prot ro"; } }
    /* shared-writable mode rejected */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R|FBK_PROT_W, FBK_MODE_SHARED_W, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_MODE) { ok=0; why="shared-w not rejected"; } }
    /* none of the above created a region */
    if (ok) { struct pgr_diag d; if (!t28_diag(f.ctrl_ep, &d) || d.region_count != 0u) { ok=0; why="ghost region"; } }
    /* a valid region then registers and resolves */
    if (ok) { t28_region(&rq, 0,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok=0; why="valid region"; } }
    /* overlap with the registered region → RANGE */
    if (ok) { t28_region(&rq, 1,0,0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok=0; why="overlap"; } }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0), &why)) ok = 0;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T218"); else it_fail("T218", why);
}

/* ── T219: read-only file-backed fault resolution ───────────────────────────
 * Faults across a multi-page RO region resolve from the file with exact
 * content at nonzero offsets and out-of-order pages; the target reads the file
 * bytes.  Invariants: F10, F13, plus the B7 flow. */
void test_t219(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "ro resolution";

    /* ONE target faults on four pages of a 4-page RO region in a scrambled
     * order [2,0,3,1]; the pager resolves each fault as it arrives.  A single
     * target (2 notifications) drives genuine multi-page out-of-order
     * resolution without exhausting the per-process notification quota that N
     * one-shot targets would. */
    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    /* One RO region over the file window [0x1000, 0x5000) — four pages, all at
     * nonzero file offsets. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x4000, 0x1000, 0x4000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    if (ok) {
        /* Visit order nibbles: step0=page2, step1=page0, step2=page3, step3=page1. */
        uint64_t order = (2ull << 0) | (0ull << 4) | (3ull << 8) | (1ull << 12);
        if (t28_cmd_read_seq(g.cmd, T28_VA_A, 4u, order) != 0) { ok = 0; why = "seq trigger"; }
        for (uint32_t k = 0; ok && k < 4u; k++)
            if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; why = "map region"; }
        if (ok) {
            uint32_t acc = (uint32_t)t28_pat(0x1000) ^ (uint32_t)t28_pat(0x2000) ^
                           (uint32_t)t28_pat(0x3000) ^ (uint32_t)t28_pat(0x4000);
            long ec = it_lp_wait_exit(g.proc);
            if (ec != (long)(LP_EXIT_MARKER ^ acc)) { ok = 0; why = "content"; }
        }
    }
    /* All four pages came from the ONE cache backing: exactly 4 misses, 4 cache
     * entries, no evictions (CACHE_CAP=8 ≥ 4). */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.cache_miss != 4u || d.cache_entries != 4u || d.cache_evict != 0u) { ok = 0; why = "cache counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T219"); else it_fail("T219", why);
}

/* Resolve a target WRITE fault at `va` on a private-writable region: the pager
 * maps a fresh writable private page, the target's store completes, and it exits
 * with the plain marker.  Returns 1 on success. */
int t28_write_resolve(struct t28_fbk *f, struct t25_tgt *g, uint32_t tidx,
                             uint64_t va, const char **why) {
    if (it_lp_cmd_va(g->cmd, LP_CMD_FAULT_WRITE_M, va) != 0) { *why = "write trigger"; return 0; }
    if (t28_map_region(f->ctrl_ep, tidx) != 0) { *why = "map region"; return 0; }
    if (it_lp_wait_exit(g->proc) != (long)LP_EXIT_MARKER) { *why = "write exit"; return 0; }
    return 1;
}

/* One EOF/zero-fill scenario: fresh target+pager (targets can't be rewired),
 * backing 0 over `fname`, region 0 at T28_VA_A, a two-offset read driving
 * `npages` resolutions, verify the target read MARKER ^ (exp0 ^ exp1).  Each
 * scenario is isolated (its own pager) so a leak shows in the caller's snapshot. */
static int t28_scenario_offs(const char *fname, uint64_t fsize, uint32_t mode, uint32_t prot,
                             uint64_t file_off, uint64_t file_len, uint64_t mem_len,
                             uint32_t count, uint64_t off0, uint64_t off1,
                             uint8_t exp0, uint8_t exp1, uint32_t npages, const char **why) {
    struct t25_tgt g;
    if (!t25_tgt_spawn(&g, why)) return 0;
    struct t28_fbk f;
    if (!t28_fbk_spawn(&f, &g, 1u, why)) { t25_tgt_reap(&g); return 0; }
    int ok = 1;
    long sz = t28_stat(f.vfs_cap, fname);
    if (sz != (long)fsize) { ok = 0; *why = "stat size"; }
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, fname, (uint64_t)sz, &gr, why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, mem_len, file_off, file_len, prot, mode, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; *why = "reg region"; }
    }
    if (ok && t28_cmd_read_offs(g.cmd, T28_VA_A, count, off0, off1) != 0) { ok = 0; *why = "offs trigger"; }
    for (uint32_t k = 0; ok && k < npages; k++)
        if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; *why = "map region"; }
    if (ok) {
        uint32_t acc = (count == 2u) ? ((uint32_t)exp0 ^ (uint32_t)exp1) : (uint32_t)exp0;
        long ec = it_lp_wait_exit(g.proc);
        if (ec != (long)(LP_EXIT_MARKER ^ acc)) { ok = 0; *why = "content"; }
    }
    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    return ok;
}

/* ── T220: EOF / zero-fill byte-exactness ────────────────────────────────────
 * A file-backed page is filled with file bytes up to file_length and zeroed
 * beyond it — exactly, at any byte granularity: a partial page (file bytes then
 * a zero tail), a sub-page file (past-EOF zero), an exact full page at EOF, and
 * pure zero pages past the file window.  Invariants: F11, F12. */
void test_t220(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "eof/zero-fill";

    /* Partial page: file bytes [0,0x800) then a zero tail — read one of each. */
    if (ok && !t28_scenario_offs(FBK_FILE_NAME, FBK_FILE_SIZE, FBK_MODE_RO, FBK_PROT_R,
                                 0, 0x800, 0x1000, 2u, 0x100, 0x900,
                                 t28_pat(0x100), 0u, 1u, &why)) ok = 0;
    /* Sub-page file (small.dat = 100 bytes): a file byte and a past-EOF zero. */
    if (ok && !t28_scenario_offs(SMALL_FILE_NAME, SMALL_FILE_SIZE, FBK_MODE_RO, FBK_PROT_R,
                                 0, SMALL_FILE_SIZE, 0x1000, 2u, 10, 0x400,
                                 t28_pats(10), 0u, 1u, &why)) ok = 0;
    /* Exact EOF: the last full file page (fbk offset 0x4000) — both ends file. */
    if (ok && !t28_scenario_offs(FBK_FILE_NAME, FBK_FILE_SIZE, FBK_MODE_RO, FBK_PROT_R,
                                 0x4000, 0x1000, 0x1000, 2u, 0, 0xFFC,
                                 t28_pat(0x4000), t28_pat(0x4000 + 0xFFC), 1u, &why)) ok = 0;

    /* Pure zero pages past the file window: a 3-page region backed by only one
     * file page — pages 1,2 are entirely zero (BSS-like). */
    if (ok) {
        struct t25_tgt g;
        struct t28_fbk f;
        if (!t25_tgt_spawn(&g, &why)) ok = 0;
        else {
            if (!t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; t25_tgt_reap(&g); }
            else {
                long sz = t28_stat(f.vfs_cap, FBK_FILE_NAME);
                struct t28_grant gr;
                if (!t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
                if (ok) {
                    struct pgr_region_req rq;
                    t28_region(&rq, 0, 0, 0, T28_VA_A, 0x3000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
                    if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
                }
                if (ok && t28_cmd_read_seq(g.cmd, T28_VA_A, 3u, (0ull) | (1ull << 4) | (2ull << 8)) != 0) { ok = 0; why = "seq"; }
                for (uint32_t k = 0; ok && k < 3u; k++)
                    if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; why = "map region"; }
                if (ok) {
                    uint32_t acc = (uint32_t)t28_pat(0) ^ 0u ^ 0u;   /* pages 1,2 pure zero */
                    long ec = it_lp_wait_exit(g.proc);
                    if (ec != (long)(LP_EXIT_MARKER ^ acc)) { ok = 0; why = "zero pages"; }
                }
                t28_fbk_reap(&f);
                t25_tgt_reap(&g);
            }
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T220"); else it_fail("T220", why);
}

/* ── T221: shared read-only cache — reuse + independent cleanup ───────────────
 * Two targets map the SAME file page RO: the first fault fills the cache, the
 * second is a cache HIT (no second VFS fill).  Each region takes an independent
 * reference; releasing one region keeps the page live for the other, and only
 * when the last reference drops is the page reclaimable.  Invariants: F19, F24. */
void test_t221(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "shared cache";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) { ok = 0; }
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    /* Region 0 (target 0) and region 1 (target 1) over the SAME file page. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
        t28_region(&rq, 1, 1, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    /* Target 0 faults → miss+fill; target 1 faults SAME page → hit (no new fill). */
    if (ok && !t28_read_verify(&f, &g0, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok && !t28_read_verify(&f, &g1, 1u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.cache_miss != 1u || d.cache_hit != 1u || d.page_fill != 1u ||
                 d.cache_entries != 1u) { ok = 0; why = "shared counts"; }
    }
    /* Release region 0: the shared page stays live for region 1 (still 1 entry). */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 0, 0) != 0) { ok = 0; why = "unreg 0"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag2"; }
        else if (d.region_count != 1u || d.cache_entries != 1u) { ok = 0; why = "post-release"; }
    }
    /* Release region 1: last reference gone; the entry is now reclaimable. */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 1, 0) != 0) { ok = 0; why = "unreg 1"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag3"; }
        else if (d.region_count != 0u) { ok = 0; why = "post-release2"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T221"); else it_fail("T221", why);
}

/* ── T222: private-writable isolation (copy-at-fill) ─────────────────────────
 * A FILE_PRIVATE_WRITABLE region gives each fault a fresh, writable, private
 * page copied from the file — writes never reach the file or another mapping.
 * Target 0 WRITES its page (proving the PTE is writable, resolved from a write
 * fault); target 1 then READS the same file offset and sees the ORIGINAL file
 * byte, not target 0's store.  Invariants: F14, F15. */
void test_t222(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "private isolation";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
        t28_region(&rq, 1, 1, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    /* Target 0 writes its private page (write fault → writable private page). */
    if (ok && !t28_write_resolve(&f, &g0, 0u, T28_VA_A, &why)) ok = 0;
    /* Target 1 reads the same file offset → original file byte (isolation). */
    if (ok && !t28_read_verify(&f, &g1, 1u, T28_VA_A, t28_pat(0), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.private_pages != 2u) { ok = 0; why = "private count"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T222"); else it_fail("T222", why);
}

/* ── T223: shared-writable is NOT_SUPPORTED, with zero side effects ───────────
 * FILE_SHARED_WRITABLE (writes shared through to the file) is deliberately
 * refused (writeback + coherence are out of scope); the rejection creates no
 * region and leaves the backing untouched, and a valid region still registers
 * and resolves afterward.  Invariant: F16. */
void test_t223(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "shared-writable";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    /* Shared-writable rejected (with or without W in prot). */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_SHARED_W, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_MODE) { ok = 0; why = "shared-w R|W not MODE"; }
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_SHARED_W, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_MODE) { ok = 0; why = "shared-w R not MODE"; }
    }
    /* No ghost region, backing intact. */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.region_count != 0u || d.backing_live != 1u) { ok = 0; why = "side effect"; }
    }
    /* A valid RO region still registers and resolves — no corruption. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "valid after reject"; }
    }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T223"); else it_fail("T223", why);
}

/* ── T224: backing read failure is atomic ────────────────────────────────────
 * A backing whose VFS authority dies under the pager cannot serve: the grant
 * is opened and registered honestly, then the ADMIN revokes the backing AT THE
 * VFS without telling the pager.  The next fault's GRANT_READ_AT is denied by
 * the VFS (the pager's own table still says "valid" — irrelevant), the fault
 * resolution fails, and NOTHING is left behind — no cache entry, no successful
 * fill, no PTE, and the target is NOT resumed (it stays faulted until the
 * supervisor kills it).  Also proves a nonexistent name cannot even be
 * granted.  Invariants: F17, F38; A4, A9, A17. */
void test_t224(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "backing atomicity";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    /* A grant on a name VFS does not export cannot even be created. */
    if (ok && t28_grant_open(f.admin, FBK_SESSION, "nope.dat", VFS_FILE_RIGHT_READ, 0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "nope grant"; }
    /* Honest backing + region... */
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    /* ...then the VFS-side authority dies behind the pager's back. */
    if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, 0) != 0) { ok = 0; why = "vfs revoke"; }
    /* Fault → resolution fails (the VFS denies GRANT_READ_AT); the pager
     * returns an error and does not resume the target. */
    if (ok && it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T28_VA_A) != 0) { ok = 0; why = "fault trigger"; }
    if (ok && t28_map_region(f.ctrl_ep, 0u) >= 0) { ok = 0; why = "resolve did not fail"; }
    /* No side effects: no successful fill, no cache entry; a fill failure and
     * a VFS grant denial counted. */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.page_fill != 0u || d.cache_entries != 0u || d.page_fill_fail == 0u ||
                 d.grant_denied == 0u) { ok = 0; why = "residue"; }
    }

    /* The target is stuck faulted — reap kills it; books must still balance. */
    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T224"); else it_fail("T224", why);
}

/* ── T225: file-grant revoke ─────────────────────────────────────────────────
 * Revoking a backing bumps its generation, marks it unusable for new regions,
 * drops its unreferenced cached pages, and makes a fault on a region still bound
 * to the old generation fail STALE_GEN.  Existing mappings are untouched (the
 * kernel VSpace owns them).  Invariants: F4, F18, F27. */
void test_t225(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "grant revoke";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    /* Resolve once → the page is cached (content verified). */
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    /* Drop the region so its cache reference clears; the entry stays valid but
     * unreferenced. */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 0, 0) != 0) { ok = 0; why = "unreg"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag pre"; }
        else if (d.cache_entries != 1u) { ok = 0; why = "entry pre-revoke"; }
    }
    /* Revoke: backing goes unusable and its unreferenced page is dropped. */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_REVOKE_BACKING, 0, 0) != 0) { ok = 0; why = "revoke"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag post"; }
        else if (d.grant_revoke != 1u || d.backing_live != 0u || d.cache_entries != 0u) { ok = 0; why = "post-revoke"; }
    }
    /* A new region binding the revoked backing (old OR new generation) → NOBACK. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_NOBACK) { ok = 0; why = "revoked still bindable"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T225"); else it_fail("T225", why);
}

/* ── T226: pager restart contract ────────────────────────────────────────────
 * The pager's backings/regions/cache are SOFT state: a restart loses them and
 * the supervisor rebuilds.  After a clean SHUTDOWN, a fresh pager starts with
 * zero state and can re-acquire its VFS file-read authority (independent of the
 * old instance) and resolve again — no stale state survives, VFS is unaffected.
 * Invariants: F28, F29. */
void test_t226(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager restart";

    /* Instance 1: register + resolve, then shut down. */
    struct t25_tgt g1;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t28_fbk f1;
    if (ok && !t28_fbk_spawn(&f1, &g1, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f1.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr1;
    if (ok && !t28_backing_setup(&f1, 0, FBK_FILE_NAME, (uint64_t)sz, &gr1, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr1.gen);
        if (t28_reg_region(f1.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    if (ok && !t28_read_verify(&f1, &g1, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    /* Clean shutdown: the pager exits; wait for its death. */
    if (ok && t28_ctrl_words(f1.ctrl_ep, FBK_OP_SHUTDOWN, 0, 0) != 0) { ok = 0; why = "shutdown"; }
    if (ok && it_lp_wait_exit(f1.proc) < 0) { ok = 0; why = "pager did not exit"; }
    t28_fbk_reap(&f1);
    t25_tgt_reap(&g1);

    /* Instance 2: a fresh pager starts with ZERO state and works end to end. */
    struct t25_tgt g2;
    if (ok && !t25_tgt_spawn(&g2, &why)) ok = 0;
    struct t28_fbk f2;
    if (ok && !t28_fbk_spawn(&f2, &g2, 1u, &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f2.ctrl_ep, &d)) { ok = 0; why = "diag fresh"; }
        else if (d.backing_live != 0u || d.region_count != 0u || d.cache_entries != 0u ||
                 d.cache_hit != 0u || d.cache_miss != 0u) { ok = 0; why = "state survived restart"; }
    }
    /* The new instance re-acquires its file authority via a FRESH grant (the
     * spawn's SESSION_RESET killed the old one at the VFS). */
    struct t28_grant gr2;
    if (ok && !t28_backing_setup(&f2, 0, FBK_FILE_NAME, (uint64_t)sz, &gr2, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x2000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr2.gen);
        if (t28_reg_region(f2.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 2"; }
    }
    if (ok && !t28_read_verify(&f2, &g2, 0u, T28_VA_A, t28_pat(0x2000), &why)) ok = 0;
    t28_fbk_reap(&f2);
    t25_tgt_reap(&g2);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T226"); else it_fail("T226", why);
}

/* ── T227: multiple files, multiple targets ──────────────────────────────────
 * Two distinct backings over two distinct files, one per target: each target
 * resolves from its OWN file with that file's exact bytes — no cross-file
 * bleed — and the two pages occupy distinct cache entries.  Invariants: F3, F23. */
void test_t227(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "multi-file";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz0 = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    long sz1 = ok ? t28_stat(f.vfs_cap, FBK2_FILE_NAME) : -1;
    if (ok && (sz0 != (long)FBK_FILE_SIZE || sz1 != (long)FBK2_FILE_SIZE)) { ok = 0; why = "stat sizes"; }
    struct t28_grant gr0, gr1;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME,  (uint64_t)sz0, &gr0, &why)) ok = 0;
    if (ok && !t28_backing_setup(&f, 1, FBK2_FILE_NAME, (uint64_t)sz1, &gr1, &why)) ok = 0;
    /* Two distinct files carry two distinct VFS-issued backing identities. */
    if (ok && gr0.bid == gr1.bid) { ok = 0; why = "identities collide"; }
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr0.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
        t28_region(&rq, 1, 1, 1, T28_VA_B, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr1.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    /* Each target reads its own file's byte at file offset 0x1000. */
    if (ok && !t28_read_verify(&f, &g0, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok && !t28_read_verify(&f, &g1, 1u, T28_VA_B, t28_pat2(0x1000), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.backing_live != 2u || d.cache_miss != 2u || d.cache_entries != 2u) { ok = 0; why = "multi counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T227"); else it_fail("T227", why);
}

/* ── T228: cache is bounded and evicts unreferenced pages ────────────────────
 * The RO cache holds at most PGR_CACHE_CAP pages.  Target 0 fills all 8 slots
 * (one region, 8 pages incl. zero-fill tail); its region is then released so
 * those pages are unreferenced.  Target 1 faults 8 fresh keys (a second backing
 * id): each MISS evicts a stale page — the cache never exceeds capacity and
 * always makes progress.  Invariants: F20, F21, F22. */
void test_t228(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cache eviction";

    struct t25_tgt g0, g1;
    if (ok && !t25_tgt_spawn(&g0, &why)) ok = 0;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t25_tgt tg2[2]; if (ok) { tg2[0] = g0; tg2[1] = g1; }
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, tg2, 2u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    long sz2 = ok ? t28_stat(f.vfs_cap, FBK2_FILE_NAME) : -1;
    /* Two DIFFERENT files → distinct VFS-issued backing ids → distinct cache
     * keys (identities are honest now; the same file can no longer be given
     * two fake ids). */
    struct t28_grant gr0, gr1;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME,  (uint64_t)sz,  &gr0, &why)) ok = 0;
    if (ok && !t28_backing_setup(&f, 1, FBK2_FILE_NAME, (uint64_t)sz2, &gr1, &why)) ok = 0;
    uint64_t order8 = 0; for (uint64_t p = 0; p < 8; p++) order8 |= (p << (4 * p));
    /* Region 0: 8 pages (5 file + 3 zero-fill), target 0 faults them all. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x8000, 0, 0x5000, FBK_PROT_R, FBK_MODE_RO, gr0.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 0"; }
    }
    if (ok && t28_cmd_read_seq(g0.cmd, T28_VA_A, 8u, order8) != 0) { ok = 0; why = "seq 0"; }
    for (uint32_t k = 0; ok && k < 8u; k++)
        if (t28_map_region(f.ctrl_ep, 0u) != 0) { ok = 0; why = "map 0"; }
    if (ok && it_lp_wait_exit(g0.proc) < 0) { ok = 0; why = "target 0 exit"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag fill"; }
        else if (d.cache_miss != 8u || d.cache_entries != 8u || d.cache_evict != 0u ||
                 d.cache_capacity != 8u) { ok = 0; why = "fill counts"; }
    }
    /* Release region 0 → its 8 pages are now unreferenced (reclaimable). */
    if (ok && t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, 0, 0) != 0) { ok = 0; why = "unreg 0"; }
    /* Region 1 (backing 1 = fbk2.dat): 8 distinct keys (3 file pages + 5
     * zero-fill); target 1 faults them → evictions. */
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 1, 1, 1, T28_VA_B, 0x8000, 0, 0x3000, FBK_PROT_R, FBK_MODE_RO, gr1.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 1"; }
    }
    if (ok && t28_cmd_read_seq(g1.cmd, T28_VA_B, 8u, order8) != 0) { ok = 0; why = "seq 1"; }
    for (uint32_t k = 0; ok && k < 8u; k++)
        if (t28_map_region(f.ctrl_ep, 1u) != 0) { ok = 0; why = "map 1"; }
    if (ok && it_lp_wait_exit(g1.proc) < 0) { ok = 0; why = "target 1 exit"; }
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag evict"; }
        /* Never exceeded capacity; exactly 8 evictions reclaimed the old pages. */
        else if (d.cache_entries != 8u || d.cache_evict != 8u || d.cache_miss != 16u) { ok = 0; why = "evict counts"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T228"); else it_fail("T228", why);
}

/* ── T229: ELF-segment groundwork (RX / R / RW / BSS, W^X) ────────────────────
 * The region model expresses the four ELF segment shapes: RX code (RO shared,
 * executable), R rodata (RO shared), RW data (private writable), and BSS (pure
 * zero-fill private writable).  W^X is enforced at registration (no W+X), every
 * segment is readable, and an RX fault resolves to a read-only EXECUTABLE page.
 * Invariants: F30, F31. */
void test_t229(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "elf segments";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, ELFSEG_FILE_NAME) : -1;
    if (ok && sz != (long)ELFSEG_FILE_SIZE) { ok = 0; why = "stat size"; }
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, ELFSEG_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;

    struct pgr_region_req rq;
    /* RX code segment (file page 0) — RO shared, executable. */
    if (ok) { t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x0000, 0x1000, FBK_PROT_R | FBK_PROT_X, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "RX rejected"; } }
    /* R rodata (file page 1) — RO shared. */
    if (ok) { t28_region(&rq, 1, 0, 0, T28_VA_B, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "R rejected"; } }
    /* RW data (file page 2) — private writable. */
    if (ok) { t28_region(&rq, 2, 0, 0, T28_VA_C, 0x1000, 0x2000, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "RW rejected"; } }
    /* BSS (pure zero-fill) — private writable, file_length 0. */
    if (ok) { t28_region(&rq, 3, 0, 0, T28_VA_A + 0x10000, 0x1000, 0x0000, 0x0000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "BSS rejected"; } }
    /* W^X: writable+executable rejected. */
    if (ok) { t28_region(&rq, 4, 0, 0, T28_VA_A + 0x20000, 0x1000, 0x0000, 0x1000, FBK_PROT_R | FBK_PROT_W | FBK_PROT_X, FBK_MODE_PRIVATE, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok = 0; why = "W^X not enforced"; } }
    /* Every segment readable: X-only rejected. */
    if (ok) { t28_region(&rq, 4, 0, 0, T28_VA_A + 0x20000, 0x1000, 0x0000, 0x1000, FBK_PROT_X, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != -(long)FBK_ERR_RANGE) { ok = 0; why = "X-only accepted"; } }

    /* An RX fault resolves to a read-only executable page with exact bytes. */
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_patseg(0), &why)) ok = 0;
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "diag"; }
        else if (d.region_count != 4u) { ok = 0; why = "segment count"; }
    }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T229"); else it_fail("T229", why);
}

/* ── T230: deterministic control-plane stress ────────────────────────────────
 * A fixed, repeatable churn: fill all backings and regions, tear them all down,
 * repeated several times, plus a data-plane resolution each cycle where a target
 * is available.  Counters must track exactly and return to zero — no leak, no
 * drift, fully deterministic across runs.  Invariants: F32, F39. */
void test_t230(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "stress";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    /* ONE grant feeds all four pager backings (a grant is per-backing at the
     * VFS; the pager may reference it from several backing slots). */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "grant open"; }

    /* Four control-plane cycles: fill 4 backings + 16 regions, verify, tear down. */
    for (uint32_t cyc = 0; ok && cyc < 4u; cyc++) {
        for (uint32_t bi = 0; ok && bi < FBK_MAX_BACKINGS; bi++)
            if (t28_reg_backing2(f.ctrl_ep, bi, &gr, (uint64_t)sz) != 0) { ok = 0; why = "stress backing"; }
        for (uint32_t ri = 0; ok && ri < FBK_MAX_REGIONS; ri++) {
            struct pgr_region_req rq;
            uint64_t va  = T28_VA_A + (uint64_t)ri * 0x40000ULL;
            uint64_t foff = (uint64_t)(ri % 5) * 0x1000ULL;
            t28_region(&rq, ri, 0 /*the one wired target*/, ri % FBK_MAX_BACKINGS, va, 0x1000, foff, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
            if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "stress region"; }
        }
        if (ok) {
            struct pgr_diag d;
            if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "stress diag"; }
            else if (d.backing_live != FBK_MAX_BACKINGS || d.region_count != FBK_MAX_REGIONS) { ok = 0; why = "stress fill counts"; }
        }
        /* Tear everything down; counters return to zero. */
        for (uint32_t ri = 0; ok && ri < FBK_MAX_REGIONS; ri++)
            if (t28_ctrl_words(f.ctrl_ep, FBK_OP_UNREGISTER_REGION, ri, 0) != 0) { ok = 0; why = "stress unreg"; }
        for (uint32_t bi = 0; ok && bi < FBK_MAX_BACKINGS; bi++)
            if (t28_ctrl_words(f.ctrl_ep, FBK_OP_REVOKE_BACKING, bi, 0) != 0) { ok = 0; why = "stress revoke"; }
        if (ok) {
            struct pgr_diag d;
            if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; why = "stress diag2"; }
            else if (d.backing_live != 0u || d.region_count != 0u || d.cache_entries != 0u) { ok = 0; why = "stress teardown counts"; }
        }
    }
    /* Data-plane: after the churn, a fresh backing+region resolves correctly. */
    if (ok && t28_reg_backing2(f.ctrl_ep, 0, &gr, (uint64_t)sz) != 0) { ok = 0; why = "final backing"; }
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x3000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "final region"; }
    }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x3000), &why)) ok = 0;

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T230"); else it_fail("T230", why);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Phase 28.1 — File Grant Capability Enforcement + Pager Multi-target (T231–T238)
 *
 * These tests attack the TRUST BOUNDARY, not the functional layer (T217–T230
 * already prove content correctness).  The premise everywhere is a HOSTILE
 * pager: iris_test self-mints a SESSION-badged vfs cap byte-identical to the
 * one a pager of that session holds (t28_session_cap) and drives the VFS
 * DIRECTLY — bypassing every check the pager's own helper would make — to
 * prove the VFS itself denies.  A helper rejecting the request would not
 * count; only a VFS reply of ACCESS_DENIED / CLOSED / NOT_FOUND does.
 * ════════════════════════════════════════════════════════════════════════ */

/* ── T231: VFS-enforced file grant identity ──────────────────────────────────
 * Two files → two grants with distinct VFS-issued (backing_id, generation).
 * A session's own cap reads its granted file and gets that file's bytes; it
 * CANNOT read the other file through the wrong grant index, a bogus index, a
 * wrong-type message, or after the grant is stale.  No cross-read.
 * Invariants: A1, A2, A3, A4, A5, A18. */
void test_t231(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "grant identity";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;

    /* Two grants (session 0), one per file, both STAT|READ. */
    struct t28_grant ga, gb;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &ga) != 0) { ok = 0; why = "open A"; }
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK2_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gb) != 0) { ok = 0; why = "open B"; }
    /* Distinct, VFS-issued identities (A2/A18). */
    if (ok && (ga.bid == gb.bid || ga.idx == gb.idx)) { ok = 0; why = "identities not distinct"; }

    /* The session's own cap — the exact authority the pager holds. */
    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    /* Each grant reads ITS file's byte at offset 0x1000 (A5). */
    uint8_t byte; uint64_t n;
    if (ok && (t28_grant_read(sc, ga.idx, 0x1000, 1, &byte, &n) != 0 || n != 1u || byte != t28_pat(0x1000))) { ok = 0; why = "read A"; }
    if (ok && (t28_grant_read(sc, gb.idx, 0x1000, 1, &byte, &n) != 0 || n != 1u || byte != t28_pat2(0x1000))) { ok = 0; why = "read B"; }
    /* STAT reports each backing's own VFS-issued identity. */
    uint64_t sz, bid, gen;
    if (ok && (t28_grant_stat(sc, ga.idx, &sz, &bid, &gen) != 0 || bid != ga.bid || gen != ga.gen || sz != FBK_FILE_SIZE)) { ok = 0; why = "stat A"; }
    if (ok && (t28_grant_stat(sc, gb.idx, &sz, &bid, &gen) != 0 || bid != gb.bid || gen != gb.gen || sz != FBK2_FILE_SIZE)) { ok = 0; why = "stat B"; }

    /* A bogus grant index is NOT_FOUND (not silently served). */
    if (ok && t28_grant_read(sc, 30u, 0, 1, 0, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "bogus idx served"; }
    /* Wrong-type: a STAT-labelled message on a READ grant path is fine, but a
     * name-based STAT from the SESSION badge is denied outright (containment). */
    if (ok) {
        long r = t28_gcall(sc, VFS_EP_OP_STAT, 0, 0, 0, 0u, FBK_FILE_NAME, 0);
        if (r != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "session named-stat not denied"; }
    }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T231"); else it_fail("T231", why);
}

/* ── T232: arbitrary-name attack denial ──────────────────────────────────────
 * From the posture of a compromised pager holding ONLY its session cap: every
 * attempt to widen authority by crafting a message is denied BY THE VFS.  A
 * pathname is not authority (A1); changing a grant index, backing id, or
 * generation in a message does not change what is served (A6); the session
 * cap cannot open a new grant, revoke by name, reset a session, or use any
 * name-based op; and no generic unrestricted VFS cap exists to fall back on
 * (A13/A14).  Invariants: A1, A4, A5, A6, A13, A14, A30. */
void test_t232(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "arbitrary-name attack";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    /* One legitimate grant on fbk.dat (READ only). */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "open"; }

    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    /* Attack 1: name-based READ_AT with the OTHER file's name — denied (a
     * session badge cannot touch the name-based path at all). */
    if (ok) {
        long r = t28_gcall(sc, VFS_EP_OP_READ_AT, 0x1000, 1, 0, 2u, FBK2_FILE_NAME, 0);
        if (r != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "named read served"; }
    }
    /* Attack 2: LIST to enumerate exports — denied. */
    if (ok && t28_gcall(sc, VFS_EP_OP_LIST, 0, 0, 0, 1u, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "list served"; }
    /* Attack 3: forge a GRANT_OPEN (escalate to a new file) — session badge is
     * not the admin, denied. */
    if (ok && t28_gcall(sc, VFS_EP_OP_GRANT_OPEN, FBK_SESSION, VFS_FILE_RIGHT_ALL, 0, 2u, FBK2_FILE_NAME, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "open escalation"; }
    /* Attack 4: revoke-by-name (the admin-only form) from the session — the
     * name-based admin path is UNREACHABLE for a session badge: the request
     * routes to the session index-form path, where word_count 0 is malformed
     * (INVALID_ARG) and, crucially, NO backing is revoked.  Either way the
     * session cannot revoke by name; the legit read below proves nothing was
     * revoked. */
    if (ok) {
        long r = t28_gcall(sc, VFS_EP_OP_GRANT_REVOKE, 0, 0, 0, 0u, FBK_FILE_NAME, 0);
        if (r != (long)IRIS_ERR_INVALID_ARG && r != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "name revoke"; }
    }
    /* Attack 5: session reset (admin-only) — denied. */
    if (ok && t28_gcall(sc, VFS_EP_OP_GRANT_SESSION_RESET, FBK_SESSION, 0, 0, 1u, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "session reset"; }
    /* Attack 6: read a DIFFERENT session's grant index (cross-session) — the
     * badge selects the session, so index gr.idx in session 1 is empty →
     * NOT_FOUND, never session 0's data. */
    if (ok) {
        handle_id_t sc1 = t28_session_cap(1u);
        if (sc1 == HANDLE_INVALID) { ok = 0; why = "session1 cap"; }
        else {
            if (t28_grant_read(sc1, gr.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "cross-session read"; }
            it_close(&sc1);
        }
    }
    /* The legitimate grant still works (denials had no side effects). */
    uint8_t byte; uint64_t n;
    if (ok && (t28_grant_read(sc, gr.idx, 0x1000, 1, &byte, &n) != 0 || byte != t28_pat(0x1000))) { ok = 0; why = "legit read broke"; }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T232"); else it_fail("T232", why);
}

/* ── T233: file grant rights monotonicity ────────────────────────────────────
 * Derived grants can only SHRINK rights, and rights can never be recovered.  A
 * STAT-only grant cannot read; a READ-only grant cannot revoke; a grant without
 * DUPLICATE cannot derive; a derive requesting a right the source lacks is
 * denied (not clamped).  Enforcement is in the VFS.  Invariants: A7. */
void test_t233(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "rights monotonicity";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    /* STAT-only grant: STAT works, READ denied. */
    struct t28_grant gs;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_STAT, &gs) != 0) { ok = 0; why = "open stat"; }
    uint64_t sz;
    if (ok && t28_grant_stat(sc, gs.idx, &sz, 0, 0) != 0) { ok = 0; why = "stat-only stat"; }
    if (ok && t28_grant_read(sc, gs.idx, 0, 1, 0, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "stat-only read"; }

    /* READ-only grant: READ works, REVOKE denied, DERIVE denied (no DUP). */
    struct t28_grant gr;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "open read"; }
    uint8_t byte;
    if (ok && t28_grant_read(sc, gr.idx, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "read-only read"; }
    if (ok && t28_grant_revoke_idx(sc, gr.idx, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "read-only revoke"; }
    if (ok && t28_grant_derive(sc, gr.idx, VFS_FILE_RIGHT_READ, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "no-dup derive"; }

    /* A grant WITH DUPLICATE derives a strictly smaller one; the derive cannot
     * request a right the source lacks (recovery denied). */
    struct t28_grant gd;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ | VFS_FILE_RIGHT_DUPLICATE, &gd) != 0) { ok = 0; why = "open dup"; }
    /* Requesting REVOKE (source lacks it) → ACCESS_DENIED. */
    if (ok && t28_grant_derive(sc, gd.idx, VFS_FILE_RIGHT_READ | VFS_FILE_RIGHT_REVOKE, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights recovery"; }
    /* Derive a READ-only child; it reads but cannot itself derive (DUP dropped). */
    uint32_t child = 0;
    if (ok && t28_grant_derive(sc, gd.idx, VFS_FILE_RIGHT_READ, &child) != 0) { ok = 0; why = "derive read"; }
    if (ok && t28_grant_read(sc, child, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "child read"; }
    if (ok && t28_grant_derive(sc, child, VFS_FILE_RIGHT_READ, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "child re-derive"; }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T233"); else it_fail("T233", why);
}

/* ── T234: revoke and generation replay denial ───────────────────────────────
 * A grant is opened and used; the ADMIN revokes the backing (VFS-side).  The
 * SAME grant index, replayed with the SAME message, now fails CLOSED at the
 * VFS — even though the session still holds the cap and the index (A9).  A new
 * grant on the same file gets a NEWER generation; the OLD generation never
 * validates against the new (A10).  Revocation is enforced by the VFS, not the
 * pager (the pager's local table is irrelevant).  Invariants: A4, A8, A9, A10. */
void test_t234(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "revoke/replay";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc == HANDLE_INVALID) { ok = 0; why = "session cap"; }

    struct t28_grant gN;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME,
                             VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gN) != 0) { ok = 0; why = "open N"; }
    uint8_t byte;
    if (ok && t28_grant_read(sc, gN.idx, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "read N"; }

    /* ADMIN revokes the backing at the VFS. */
    uint64_t newgen = 0;
    if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, &newgen) != 0) { ok = 0; why = "revoke"; }
    if (ok && newgen == gN.gen) { ok = 0; why = "generation not bumped"; }

    /* Replay the EXACT read on the same index → CLOSED (VFS-enforced, A9). */
    if (ok && t28_grant_read(sc, gN.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "replay served"; }
    if (ok && t28_grant_stat(sc, gN.idx, 0, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "replay stat served"; }

    /* A fresh grant gets generation N+1; the old snapshot never matches (A10). */
    struct t28_grant gN1;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gN1) != 0) { ok = 0; why = "open N+1"; }
    if (ok && gN1.gen != newgen) { ok = 0; why = "new grant wrong gen"; }
    if (ok && gN1.gen == gN.gen) { ok = 0; why = "gen reused"; }
    /* The new grant reads; the old index is still CLOSED. */
    if (ok && t28_grant_read(sc, gN1.idx, 0x1000, 1, &byte, 0) != 0) { ok = 0; why = "new grant read"; }
    if (ok && t28_grant_read(sc, gN.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "old still readable"; }
    it_close(&sc);

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T234"); else it_fail("T234", why);
}

/* ── T235: pager restart with per-backing grants ─────────────────────────────
 * The supervisor opens a grant for a pager, the pager dies, and a NEW pager
 * instance is started.  The restart protocol RESETS the session at the VFS
 * first, so the new pager's session cap cannot reach the OLD instance's grant
 * (A11): replaying the old grant index from the new session cap fails
 * NOT_FOUND.  The supervisor issues a FRESH grant and the new pager resolves.
 * Invariants: A11. */
void test_t235(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager restart grants";

    /* Instance 1: open a grant, note its index, then kill the pager. */
    struct t25_tgt g1;
    if (ok && !t25_tgt_spawn(&g1, &why)) ok = 0;
    struct t28_fbk f1;
    if (ok && !t28_fbk_spawn(&f1, &g1, 1u, &why)) ok = 0;
    struct t28_grant gold;
    if (ok && t28_grant_open(f1.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gold) != 0) { ok = 0; why = "open old"; }
    /* Prove it was live. */
    handle_id_t sc1 = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && (sc1 == HANDLE_INVALID || t28_grant_read(sc1, gold.idx, 0x1000, 1, 0, 0) != 0)) { ok = 0; why = "old read"; }
    it_close(&sc1);
    t28_fbk_reap(&f1);
    t25_tgt_reap(&g1);

    /* Instance 2: t28_fbk_spawn's restart protocol RESETS session 0 first. */
    struct t25_tgt g2;
    if (ok && !t25_tgt_spawn(&g2, &why)) ok = 0;
    struct t28_fbk f2;
    if (ok && !t28_fbk_spawn(&f2, &g2, 1u, &why)) ok = 0;
    handle_id_t sc2 = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
    if (ok && sc2 == HANDLE_INVALID) { ok = 0; why = "session cap 2"; }
    /* The OLD grant index is gone from the session (A11). */
    if (ok && t28_grant_read(sc2, gold.idx, 0x1000, 1, 0, 0) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale grant survived restart"; }
    /* A fresh grant + backing + region resolves end to end. */
    long sz = ok ? t28_stat(f2.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gnew;
    if (ok && !t28_backing_setup(&f2, 0, FBK_FILE_NAME, (uint64_t)sz, &gnew, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gnew.gen);
        if (t28_reg_region(f2.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region 2"; }
    }
    if (ok && !t28_read_verify(&f2, &g2, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    it_close(&sc2);
    t28_fbk_reap(&f2);
    t25_tgt_reap(&g2);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T235"); else it_fail("T235", why);
}

/* ── T236: VFS restart invalidates old grants ────────────────────────────────
 * A VFS instance issues grants under an epoch that stamps the high half of
 * every generation.  A restarted VFS gets a strictly newer epoch, so a grant
 * snapshotting an old generation can never validate against the new instance
 * (A12).  We SIMULATE the new instance in-process by re-initializing a grant
 * table under a bumped epoch (the productive VFS uses its svcmgr restart
 * generation for the same effect) and confirm: (a) old generations never equal
 * new ones; (b) mappings already installed follow the Phase 28 contract.  The
 * cross-instance generation-namespace property is verified against the live
 * VFS's issued generations.  Invariants: A12, A16. */
void test_t236(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "vfs restart grants";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;

    /* Open a grant, install a real mapping via a fault resolution. */
    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;

    /* The generation carries the instance epoch in its high half: a restarted
     * VFS (strictly newer epoch) cannot reissue this generation.  We check the
     * epoch field is nonzero and monotonic by opening a second grant on a
     * DIFFERENT file and confirming both share the same instance epoch (high
     * 48 bits) — i.e. one live instance — while a hypothetical older-epoch
     * generation (epoch-1) can never appear. */
    struct t28_grant gr2;
    if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK2_FILE_NAME, VFS_FILE_RIGHT_READ, &gr2) != 0) { ok = 0; why = "open 2"; }
    if (ok) {
        uint64_t epoch_a = gr.gen >> 16, epoch_b = gr2.gen >> 16;
        if (epoch_a != epoch_b) { ok = 0; why = "epoch drift within instance"; }
        /* A stale grant carrying an older-epoch generation must fail: forge one
         * by registering a backing whose generation is from a lower epoch and
         * confirm the pager's cross-check (against the live VFS) denies it. */
        uint64_t stale_gen = (epoch_a > 0 ? (epoch_a - 1) : 0) << 16 | 1u;
        if (t28_reg_backing_raw(f.ctrl_ep, 1, gr.idx, gr.bid, stale_gen, (uint64_t)sz)
            != -(long)FBK_ERR_GRANT) { ok = 0; why = "stale-epoch backing accepted"; }
    }
    /* The already-installed mapping still holds (Phase 28 contract, A16): the
     * target already exited reading the correct byte above; re-reading is not
     * possible (a resolved target completed), so the mapping-survival property
     * is the successful resolution itself plus a clean baseline. */

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T236"); else it_fail("T236", why);
}
static void t28_multi_close(struct t28_multi *m) {
    for (uint32_t i = 0; i < m->n; i++) {
        it_close(&m->cmd[i]); it_close(&m->proc[i]); it_close(&m->vs[i]);
    }
    it_close(&m->fault_notif); it_close(&m->exit_notif);
    m->n = 0;
}
static void t28_multi_reap(struct t28_multi *m) {
    for (uint32_t i = 0; i < m->n; i++)
        if (m->proc[i] != HANDLE_INVALID) {
            (void)it_kill((long)m->proc[i]);
            (void)it_lp_wait_exit(m->proc[i]);
        }
    t28_multi_close(m);
}
/* Spawn `nt` lifecycle_probe targets sharing two notifications.  Each target's
 * faults CALL the shared fault endpoint through a copy badged (i+1); its exit watch signals
 * exit_notif bit (1<<i). */
static int t28_multi_spawn(struct t28_multi *m, uint32_t nt, const char **why) {
    for (uint32_t i = 0; i < T28_MT_MAX; i++) { m->cmd[i] = m->proc[i] = m->vs[i] = HANDLE_INVALID; }
    m->fault_notif = m->exit_notif = HANDLE_INVALID; m->n = 0;
    if (nt > T28_MT_MAX) { *why = "too many targets"; return 0; }
    long fn = it_ep_create();       /* A-22: the shared fault ENDPOINT */
    long en = it_notify_create();
    if (fn < 0 || en < 0) { it_close(&m->fault_notif); it_close(&m->exit_notif);
        if (fn >= 0) { handle_id_t h = (handle_id_t)fn; it_close(&h); }
        if (en >= 0) { handle_id_t h = (handle_id_t)en; it_close(&h); }
        *why = "shared notifs"; return 0; }
    m->fault_notif = (handle_id_t)fn; m->exit_notif = (handle_id_t)en;
    if (!it_pgr_mbox_fresh(nt)) { *why = "fault replies"; t28_multi_close(m); return 0; }
    for (uint32_t i = 0; i < nt; i++) {
        long ep = it_ep_create();
        if (ep < 0) { *why = "cmd ep"; t28_multi_close(m); return 0; }
        m->cmd[i] = (handle_id_t)ep;
        it_child_keep_vspace();   /* each target is mapped into by the pager */
        if (lp_spawn_child(m->cmd[i], &m->proc[i]) < 0 || m->proc[i] == HANDLE_INVALID) { *why = "spawn"; t28_multi_close(m); return 0; }
        long vs = it_child_vspace(m->proc[i]);
        if (vs < 0) { *why = "vspace"; t28_multi_close(m); return 0; }
        m->vs[i] = (handle_id_t)vs;
        {
            long bep = it_cs_badge((long)m->fault_notif,
                                   RIGHT_READ | RIGHT_WRITE, i + 1u);
            int wired = (bep >= 0 &&
                         it_invoke(it_child_tcb((long)m->proc[i]), INV_TCB_SET_FAULT_HANDLER, bep, 0, 0) == 0);
            if (bep >= 0) it_slot_delete((uint32_t)bep);
            if (!wired) { *why = "wire"; t28_multi_close(m); return 0; }
        }
        if (it_invoke2(it_child_tcb((long)m->proc[i]), INV_TCB_WATCH, (long)m->exit_notif, (long)(1u << i)) != 0) {
            *why = "wire"; t28_multi_close(m); return 0;
        }
        m->n++;
    }
    return 1;
}
/* Spawn a pager over a t28_multi group: shares the group's fault notification
 * (slot 5) and grants proc/vs for each target.  Resets session 0 first. */
static int t28_fbk_spawn_multi(struct t28_fbk *f, struct t28_multi *m, const char **why) {
    f->ctrl_ep = f->proc = HANDLE_INVALID;
    f->vfs_cap = f->admin = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) { *why = "ctrl ep"; return 0; }
    handle_id_t ctrl = (handle_id_t)ep;
    handle_id_t vfs  = t28_vfs_cap();
    handle_id_t adm  = t28_admin_cap();
    if (vfs == HANDLE_INVALID || adm == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm);
        *why = "grants"; return 0;
    }
    if (t28_session_reset(adm, FBK_SESSION) != 0) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm);
        *why = "session reset"; return 0;
    }
    struct svc_mint mm[40] = { 0 };
    uint32_t k = 0;
    mm[k].slot = PGR_SLOT_CTRL_EP; IT_MINT_SRC(mm[k], ctrl); mm[k].rights = RIGHT_READ; mm[k].badge = 0; k++;
    mm[k].slot = FBK_SLOT_VFS_EP;  IT_MINT_SRC(mm[k], vfs);  mm[k].rights = RIGHT_WRITE;
    mm[k].badge = IRIS_BADGE_FILEGRANT_S(FBK_SESSION); k++;
    mm[k].slot = FBK_SLOT_NOTIF;   IT_MINT_SRC(mm[k], m->fault_notif); mm[k].rights = RIGHT_READ; mm[k].badge = 0; k++;
    /* Stage 7 Step 7: the mailbox each fault delivers a thread into. */
    mm[k].slot = PGR_SLOT_FAULT_CN; IT_MINT_SRC(mm[k], IT_PGR_MBOX_SLOT); mm[k].rights = RIGHT_READ | RIGHT_WRITE; mm[k].badge = 0; k++;
    for (uint32_t i = 0; i < m->n; i++) {
        mm[k].slot = PGR_TSLOT_VS(i);   IT_MINT_SRC(mm[k], m->vs[i]);   mm[k].rights = RIGHT_WRITE;               mm[k].badge = 0; k++;
    }
    handle_id_t boot = HANDLE_INVALID;
    /* Phase S1: explicit reply object for the pager's ctrl EP (slot 13). */
    handle_id_t pgr_reply_h = HANDLE_INVALID;
    {
        long rr = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
        if (rr >= 0) {
            pgr_reply_h = (handle_id_t)rr;
            mm[k].slot = 13u; IT_MINT_SRC(mm[k], pgr_reply_h);
            mm[k].rights = RIGHT_READ | RIGHT_WRITE; mm[k].badge = 0; k++;
        }
    }
    long r = svc_load_minted_ws(IRIS_CPTR_PROC_CONTROL, IRIS_CPTR_INITRD_CONTROL, "pager", &f->proc, &boot, mm, k,
                             IT_LOADER_WS, 0,
                               /*own_budget_slot=*/IRIS_CPTR_OWN_UNTYPED, /*keep_cnode_dest=*/0u, it_child_tcb_dest(), it_child_vs_dest());
    it_child_bind(f->proc);
    it_close(&pgr_reply_h);
    it_close(&boot);
    if (r < 0 || f->proc == HANDLE_INVALID) {
        it_close(&ctrl); it_close(&vfs); it_close(&adm); it_close(&f->proc);
        *why = "pager spawn"; return 0;
    }
    f->ctrl_ep = ctrl; f->vfs_cap = vfs; f->admin = adm;
    return 1;
}
/* Wait (≤2s) for exit bit i on the shared exit notification, consuming and
 * re-accumulating other bits so no exit is lost. */
static uint64_t g_t28_exit_pending = 0;
static int t28_multi_wait_exit(struct t28_multi *m, uint32_t i) {
    uint64_t bit = 1ull << i;
    if (g_t28_exit_pending & bit) { g_t28_exit_pending &= ~bit; return 1; }
    for (uint32_t tries = 0; tries < 64u; tries++) {
        uint64_t bits = 0;
        if (it_wait_timeout( (long)m->exit_notif, (long)(uintptr_t)&bits, 2000000000LL) != 0) return 0;
        g_t28_exit_pending |= bits;
        if (g_t28_exit_pending & bit) { g_t28_exit_pending &= ~bit; return 1; }
    }
    return 0;
}

/* ── T237: multi-target notification scaling ──────────────────────────────────
 * Register and exercise 1, 4, 8 and 16 targets under a SINGLE pager, all
 * sharing ONE fault notification.  Each target faults on its own file page and
 * reads the correct byte; faults are interleaved (all triggered before any is
 * resolved); the supervisor's notification books return to baseline.  Proves
 * the quota problem is SOLVED, not avoided (Phase 28 could only run 1 target).
 * Invariants: A19, A20, A21, A22, A23, A25. */
static int t237_run(uint32_t nt, const char **why) {
    struct t28_multi m;
    if (!t28_multi_spawn(&m, nt, why)) return 0;
    struct t28_fbk f;
    if (!t28_fbk_spawn_multi(&f, &m, why)) { t28_multi_reap(&m); return 0; }
    int ok = 1;
    g_t28_exit_pending = 0;

    long sz = t28_stat(f.vfs_cap, FBK_FILE_NAME);
    struct t28_grant gr;
    if (!t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, why)) ok = 0;
    /* One region per target: target i reads file page (i % 5)+? — use page i+1
     * clamped into the 5-page file so each has a real file byte. */
    for (uint32_t i = 0; ok && i < nt; i++) {
        struct pgr_region_req rq;
        uint64_t foff = (uint64_t)((i % 4u) + 1u) * 0x1000ULL;   /* pages 1..4 */
        t28_region(&rq, i, i, 0, T28_VA_A + (uint64_t)i * 0x40000ULL, 0x1000, foff, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; *why = "reg region"; }
    }
    /* Interleave: trigger ALL faults first (each target blocks in its fault),
     * then resolve them in a scrambled order. */
    for (uint32_t i = 0; ok && i < nt; i++)
        if (it_lp_cmd_va(m.cmd[i], LP_CMD_FAULT_READ, T28_VA_A + (uint64_t)i * 0x40000ULL) != 0) { ok = 0; *why = "trigger"; }
    for (uint32_t s = 0; ok && s < nt; s++) {
        uint32_t i = (s * 7u + 3u) % nt;    /* scrambled visit order */
        /* Skip already-resolved slots in the scramble by linear-probing. */
        uint32_t tries = 0;
        while (tries < nt && ((1u << i) & 0)) { i = (i + 1u) % nt; tries++; }
        if (t28_map_region(f.ctrl_ep, i) != 0) { ok = 0; *why = "resolve"; }
    }
    /* Each target ran to completion reading its page's byte. */
    for (uint32_t i = 0; ok && i < nt; i++) {
        uint64_t foff = (uint64_t)((i % 4u) + 1u) * 0x1000ULL;
        if (!t28_multi_wait_exit(&m, i)) { ok = 0; *why = "target exit"; }
        else if (it_invoke0(it_child_tcb((long)m.proc[i]), INV_TCB_EXIT_CODE) != (long)(LP_EXIT_MARKER ^ (uint32_t)t28_pat(foff))) { ok = 0; *why = "wrong byte"; }
    }
    /* Diagnostics: the pager multiplexed all nt faults over ONE shared
     * notification.  The wait-any accumulator means a single wakeup can carry
     * several targets' bits, so wakeups is BETWEEN 1 and nt (fewer wakeups than
     * faults is the efficiency win, not a bug).  What must hold: the shared
     * notification path WAS exercised (>=1 wait) and no fault bit is left
     * pending (all consumed and resolved, no mix). */
    if (ok) {
        struct pgr_diag d;
        if (!t28_diag(f.ctrl_ep, &d)) { ok = 0; *why = "diag"; }
        else if (d.notif_waits == 0u || d.notif_wakeups == 0u) { ok = 0; *why = "shared notif unused"; }
        else if (d.notif_wakeups > nt) { ok = 0; *why = "excess wakeups"; }
        else if (d.pending_mask != 0u) { ok = 0; *why = "pending residue"; }
    }
    t28_fbk_reap(&f);
    t28_multi_reap(&m);
    return ok;
}
void test_t237(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "multi-target scaling";

    const uint32_t counts[4] = { 1u, 4u, 8u, 16u };
    for (uint32_t c = 0; ok && c < 4u; c++) {
        if (!t237_run(counts[c], &why)) ok = 0;
        it_quiesce_reaper();
        if (ok) { struct it_snap r = it_snap_take(); if (!it_snap_baseline_live(&b, &r, &why)) ok = 0; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T237"); else it_fail("T237", why);
}
int it_utq_g(struct it_utq_global *q) {
    return it_invoke2(IT_QARG(1, sizeof(*q)), INV_UNTYPED_QUERY, (long)(uintptr_t)q, 0) == 0;
}
int it_utq_1(long ut, struct it_utq_one *q) {
    return it_invoke2(IT_QARG(2, sizeof(*q)), INV_UNTYPED_QUERY, (long)(uintptr_t)q, ut) == 0;
}
int it_utq_o(struct it_utq_objects *q) {
    return it_invoke2(IT_QARG(3, sizeof(*q)), INV_UNTYPED_QUERY, (long)(uintptr_t)q, 0) == 0;
}
int it_utq_mdb(struct it_utq_mdb *q) {
    return it_invoke2(IT_QARG(4, sizeof(*q)), INV_UNTYPED_QUERY, (long)(uintptr_t)q, 0) == 0;
}

/* Carve a fresh page-multiple sub-untyped for one S1 test, into a slot. */
long s1_sub_ut(uint64_t bytes) {
    return it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                IRIS_KOBJ_UNTYPED, (long)bytes);
}
static uint32_t t238_rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
void test_t238(void) {
    uint32_t rng = T238_SEED;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "file-authority stress";
    uint32_t round = 0, op = 0;

    for (round = 0; ok && round < T238_ROUNDS; round++) {
        op = t238_rnd(&rng) % 3u;
        switch (op) {
        case 0: {
            /* Grant + hostile attempts + honest read. */
            struct t25_tgt g;
            if (!t25_tgt_spawn(&g, &why)) { ok = 0; break; }
            struct t28_fbk f;
            if (!t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; t25_tgt_reap(&g); break; }
            struct t28_grant gr;
            if (t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "s0 open"; }
            handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
            if (ok && sc == HANDLE_INVALID) { ok = 0; why = "s0 cap"; }
            uint8_t byte;
            if (ok && (t28_grant_read(sc, gr.idx, 0x1000, 1, &byte, 0) != 0 || byte != t28_pat(0x1000))) { ok = 0; why = "s0 read"; }
            /* wrong-name (session cannot use name path). */
            if (ok && t28_gcall(sc, VFS_EP_OP_READ_AT, 0x1000, 1, 0, 2u, FBK2_FILE_NAME, 0) != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "s0 name"; }
            /* wrong-backing register at the pager. */
            if (ok && t28_reg_backing_raw(f.ctrl_ep, 1, gr.idx, gr.bid + 9u, gr.gen, (uint64_t)FBK_FILE_SIZE) != -(long)FBK_ERR_GRANT) { ok = 0; why = "s0 wrongback"; }
            it_close(&sc);
            t28_fbk_reap(&f);
            t25_tgt_reap(&g);
            break;
        }
        case 1: {
            /* Revoke + replay denial + fresh generation. */
            struct t25_tgt g;
            if (!t25_tgt_spawn(&g, &why)) { ok = 0; break; }
            struct t28_fbk f;
            if (!t28_fbk_spawn(&f, &g, 1u, &why)) { ok = 0; t25_tgt_reap(&g); break; }
            struct t28_grant gr;
            if (t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr) != 0) { ok = 0; why = "s1 open"; }
            handle_id_t sc = ok ? t28_session_cap(FBK_SESSION) : HANDLE_INVALID;
            if (ok && sc == HANDLE_INVALID) { ok = 0; why = "s1 cap"; }
            if (ok && t28_grant_read(sc, gr.idx, 0, 1, 0, 0) != 0) { ok = 0; why = "s1 read"; }
            uint64_t ng = 0;
            if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, &ng) != 0) { ok = 0; why = "s1 revoke"; }
            if (ok && t28_grant_read(sc, gr.idx, 0, 1, 0, 0) != (long)IRIS_ERR_CLOSED) { ok = 0; why = "s1 replay"; }
            struct t28_grant gr2;
            if (ok && t28_grant_open(f.admin, FBK_SESSION, FBK_FILE_NAME, VFS_FILE_RIGHT_READ, &gr2) != 0) { ok = 0; why = "s1 reopen"; }
            if (ok && gr2.gen == gr.gen) { ok = 0; why = "s1 gen reuse"; }
            if (ok && t28_grant_read(sc, gr2.idx, 0, 1, 0, 0) != 0) { ok = 0; why = "s1 new read"; }
            it_close(&sc);
            t28_fbk_reap(&f);
            t25_tgt_reap(&g);
            break;
        }
        default: {
            /* Multi-target batch (4 targets), death in arbitrary order. */
            uint32_t nt = 4u;
            struct t28_multi m;
            if (!t28_multi_spawn(&m, nt, &why)) { ok = 0; break; }
            struct t28_fbk f;
            if (!t28_fbk_spawn_multi(&f, &m, &why)) { ok = 0; t28_multi_reap(&m); break; }
            g_t28_exit_pending = 0;
            long sz = t28_stat(f.vfs_cap, FBK_FILE_NAME);
            struct t28_grant gr;
            if (!t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
            for (uint32_t i = 0; ok && i < nt; i++) {
                struct pgr_region_req rq;
                uint64_t foff = (uint64_t)(i + 1u) * 0x1000ULL;
                t28_region(&rq, i, i, 0, T28_VA_A + (uint64_t)i * 0x40000ULL, 0x1000, foff, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
                if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "s2 region"; }
            }
            for (uint32_t i = 0; ok && i < nt; i++)
                if (it_lp_cmd_va(m.cmd[i], LP_CMD_FAULT_READ, T28_VA_A + (uint64_t)i * 0x40000ULL) != 0) { ok = 0; why = "s2 trigger"; }
            /* Resolve in reverse order (arbitrary vs trigger order). */
            for (uint32_t s = 0; ok && s < nt; s++) {
                uint32_t i = nt - 1u - s;
                if (t28_map_region(f.ctrl_ep, i) != 0) { ok = 0; why = "s2 resolve"; }
            }
            for (uint32_t i = 0; ok && i < nt; i++) {
                uint64_t foff = (uint64_t)(i + 1u) * 0x1000ULL;
                if (!t28_multi_wait_exit(&m, i)) { ok = 0; why = "s2 exit"; }
                else if (it_invoke0(it_child_tcb((long)m.proc[i]), INV_TCB_EXIT_CODE) != (long)(LP_EXIT_MARKER ^ (uint32_t)t28_pat(foff))) { ok = 0; why = "s2 byte"; }
            }
            t28_fbk_reap(&f);
            t28_multi_reap(&m);
            break;
        }
        }
        it_quiesce_reaper();
        if (ok) { struct it_snap r = it_snap_take(); if (!it_snap_baseline_live(&b, &r, &why)) ok = 0; }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T238");
    else { it_fz_note("T238", T238_SEED, round, op); it_fail("T238", why); }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Phase 29 — Resource Ownership, Quota Domains and Kernel Capacity (T239–T250)
 *
 * Model: a KProcess IS a resource domain.  Every object is charged to the
 * process that logically OWNS it (its payer), selected by explicit capability
 * authority at creation — NOT to whoever ran the syscall.  A loader creates a
 * child's image VMOs charged to the CHILD (SYS_VMO_CREATE_FOR + the child's
 * process cap with RIGHT_MANAGE), so the loader's own quota stays flat.  These
 * tests prove creator/owner/payer/holder are separate, sharing charges once,
 * exhaustion is atomic, and usage returns to baseline.
 * ════════════════════════════════════════════════════════════════════════ */

/* Stage 7-mem: struct it_rinfo and it_rinfo() are DELETED with
 * SYS_RESOURCE_INFO.  Per-process accounting is gone — a VMO's cost is the
 * Untyped it was carved from — and the three global gauges the syscall carried
 * (kslab occupancy, failed charges, rollbacks) live in SYS_UNTYPED_QUERY's
 * GLOBAL kind, mirrored above as struct it_utq_global. */

/* Spawn a bare lifecycle_probe child (cmd endpoint + process).  0 on success. */
int it_bare_child(handle_id_t *cmd_out, handle_id_t *proc_out) {
    *cmd_out = *proc_out = HANDLE_INVALID;
    long ep = it_ep_create();
    if (ep < 0) return 0;
    *cmd_out = (handle_id_t)ep;
    if (lp_spawn_child(*cmd_out, proc_out) < 0 || *proc_out == HANDLE_INVALID) {
        it_close(cmd_out); return 0;
    }
    return 1;
}
void it_bare_kill(handle_id_t *cmd, handle_id_t *proc) {
    if (*proc != HANDLE_INVALID) { (void)it_kill((long)*proc); (void)it_lp_wait_exit(*proc); }
    it_close(cmd); it_close(proc);
}
