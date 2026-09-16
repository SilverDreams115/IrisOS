/*
 * it_t298_t325.c — tests T298 through T325.
 *
 * The suite's numbering is chronological, not thematic: T298 was written
 * stages before T325, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T298 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
void test_t298(void) {
    int ok = 1;
    const char *why = "untyped pays for headers";

    if (!it_setup_self_vspace()) { it_fail("T298", "vspace self"); return; }

    /* A dedicated sub-untyped so the measurement is not perturbed by whatever
     * else the suite fabricates from its pool. */
    long sub = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_UNTYPED, 256u * 1024u);
    if (sub < 0) { it_fail("T298", "sub-untyped"); return; }

    uint64_t before = 0, after = 0;
    if (it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&before) != 0) {
        it_fail("T298", "info"); return;
    }

    long fr = it_frame_create_slot(sub, 4096u);
    if (fr < 0) { it_fail("T298", "frame retype"); return; }
    if (it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
        it_fail("T298", "info2"); return;
    }

    /* 1. the page and the header both came out of this Untyped. */
    uint64_t cost = before - after;
    if (cost <= 4096u)      { ok = 0; why = "header not charged"; }
    if (ok && cost >= 8192u) { ok = 0; why = "header cost a whole page"; }

    /* 2. density: three more frames cost three more pages, plus headers. */
    if (ok) {
        uint64_t base = after;
        for (int i = 0; ok && i < 3; i++) {
            long f2 = it_frame_create_slot(sub, 4096u);
            if (f2 < 0) { ok = 0; why = "frame retype n"; }
        }
        if (ok && it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
            ok = 0; why = "info3";
        }
        uint64_t cost3 = base - after;
        if (ok && cost3 >= 3u * 8192u) { ok = 0; why = "frames not page-dense"; }
        if (ok && cost3 <= 3u * 4096u) { ok = 0; why = "headers not charged n"; }
    }

    /* 3. the frame is a real frame: map it, write, read back, unmap. */
    if (ok && it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)T298_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "map";
    } else if (ok) {
        volatile uint64_t *pg = (volatile uint64_t *)(uintptr_t)T298_VA;
        /* A freshly retyped frame is zero-filled; kernel bookkeeping placed
         * inside the page would show up right here. */
        if (pg[0] != 0u || pg[63] != 0u) { ok = 0; why = "frame not clean"; }
        if (ok) {
            pg[0] = 0xA5A5A5A5A5A5A5A5ULL;
            if (pg[0] != 0xA5A5A5A5A5A5A5A5ULL) { ok = 0; why = "frame not writable"; }
        }
        if (it_invoke2(fr, INV_FRAME_UNMAP, IT_VS, (long)T298_VA) != 0) {
            ok = 0; why = "unmap";
        }
    }

    if (ok) it_pass("T298"); else it_fail("T298", why);
}

void test_t299(void) {
    int ok = 1;
    const char *why = "page-table budget";

    it_slot_delete(T299_SLOT_POOL);
    it_slot_delete(T299_SLOT_PROC);

    /*
     * Stage 7-proc: legs 1 and 2 retired with SYS_PROCESS_CREATE.
     *
     * They asserted that its address-space argument was REQUIRED and was a
     * capability of a specific type — real claims about a syscall that no
     * longer exists.  A thread is given its address space by
     * SYS_TCB_CONFIGURE, whose equivalents (CPTR_NULL refused, a wrong type
     * refused either way round) are T297's.
     */

    /* A budget of our own to measure. */
    long pool = -1;
    if (ok) {
        pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_UNTYPED, 256u * 1024u);
        if (pool < 0) { ok = 0; why = "pool carve"; }
    }

    uint64_t before = 0, after = 0;
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&before) != 0) {
        ok = 0; why = "info";
    }

    /* 3. an address space of our own making, out of that budget, handed to a
     *    process.  Page tables appear on the FIRST map into it, which is what
     *    a loader would do and what this leg does explicitly below. */
    long cvs = -1, ccn = -1;
    if (ok) {
        cvs = it_retype_slot_alloc(pool, IRIS_KOBJ_VSPACE, 4096);
        if (cvs < 0) { ok = 0; why = "vspace retype"; }
    }
    /* Stage 6-pure Step 5: the CSpace is ours to make too, and its width is
     * ours to choose — the kernel used to pick 256 for everyone. */
    if (ok) {
        ccn = it_retype_slot_alloc(pool, IRIS_KOBJ_CNODE, 16);
        if (ccn < 0) { ok = 0; why = "cnode retype"; }
    }
    /*
     * Stage 7-proc: the "bound twice" probes retired with the binding.
     *
     * A walk and a CSpace were EXCLUSIVE to one process, because teardown was
     * per-process and a shared one would have been emptied by the first
     * death.  Teardown is per-object now — an address space ends when its last
     * capability does — so several threads sharing a CSpace and a VSpace is
     * not a hazard to refuse: it is the definition of a process.
     */

    /* Stage 6 Step 3: creating the address space already costs the budget —
     * its PML4 is a page of it and its VSpace header a block of it, where both
     * used to be kernel memory. */
    if (ok) {
        uint64_t created = 0;
        if (it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&created) != 0) {
            ok = 0; why = "info created";
        } else if (before - created < 4096u) {
            ok = 0; why = "vspace not charged";
        }
    }

    long vmo = -1;
    if (ok) {
        vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096u);
        if (vmo < 0) { ok = 0; why = "vmo"; }
    }
    /* A window no other mapping of this child touches, so the map must build
     * the levels rather than reuse them. */
    /* Stage 7 Step 15: this test RETYPED the address space a few lines up and
     * still holds it at `cvs` — asking the process for it was always the long
     * way round, and SYS_PROCESS_VSPACE is retired. */
    long t299_vs = ok ? cvs : -1;
    if (ok && t299_vs < 0) { ok = 0; why = "child vspace"; }
    /*
     * Stage 7-proc: the levels are charged to THIS pool, said explicitly.
     *
     * The suite's ordinary map wrapper fixes a MISSING_TABLE by retyping a
     * level from IRIS_CPTR_TEST_UNTYPED — its own budget — so the levels this
     * leg is about were never charged to `pool` at all.  The assertion below
     * passed anyway, on the KProcess allocation that SYS_PROCESS_CREATE used
     * to make out of the same region: it was measuring the wrong thing and the
     * right number.  Retiring the process took the accident away and left the
     * claim, so the claim is made properly — the fixup names `pool`, which is
     * what a loader holding its child's budget does.
     */
    if (ok) {
        long mr = iris_invoke(vmo, INV_FRAME_MAP, t299_vs,
                              (long)0x80C0000000ULL, 1);
        if (mr == (long)IRIS_ERR_MISSING_TABLE)
            mr = iris_vspace_fixup(INV_FRAME_MAP, vmo, t299_vs,
                                   (long)0x80C0000000ULL, 1,
                                   IT_VS, pool,
                                   (long)(((uint64_t)252 << 32) | IT_OBJ_CNODE_SLOT),
                                   (long)IT_PT_SCRATCH,
                                   (long)(((uint64_t)253 << 32) | IT_OBJ_CNODE_SLOT),
                                   (long)IT_PT_VS_SCRATCH);
        if (mr != 0) { ok = 0; why = "map into child"; }
    }
    /* Drop it immediately: this test's whole point is that the budget becomes
     * RESET-able once the child dies, and a VSpace capability held here keeps
     * that address space — and every page table in it, each a child entry on
     * the budget — alive past the death.  Naming the address space to map into
     * it (Stage 7 Step 9) means the caller must also let go of it. */
    if (t299_vs >= 0) { handle_id_t h = (handle_id_t)t299_vs; it_close(&h); }
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
        ok = 0; why = "info2";
    }
    /* The levels came out of the budget — page-sized, and more than one. */
    if (ok && before - after < 2u * 4096u) { ok = 0; why = "levels not charged"; }

    /* ...and while they are live the budget cannot be reclaimed under them:
     * a page table counts as a child of the Untyped that paid for it. */
    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) == 0) {
        ok = 0; why = "budget reset while bound";
    }

    /* 4. once the address space is gone the budget is reclaimable again: the
     *    pages stay where they are (a bump allocator does not rewind), but the
     *    tables stop counting as children, so a RESET can reuse the region.
     *
     *    Stage 7-proc: an address space is an object WE made, and DELETING
     *    THE CAPABILITY is the whole of ending it.  Nothing was ever started
     *    here — there is no process to start — so this leg is the pure form of
     *    the claim: the levels a map built are charged to the budget, and they
     *    stop counting the moment nobody names the space they are in. */
    if (ok) {
        if (cvs >= 0) it_slot_delete((uint32_t)cvs);
        if (ccn >= 0) it_slot_delete((uint32_t)ccn);
        it_quiesce_reaper();
        long rr = it_invoke0(pool, INV_UNTYPED_RESET);
        if (ok && rr == (long)IRIS_ERR_BUSY) { ok = 0; why = "budget still bound after death"; }
        else if (ok && rr != 0)              { ok = 0; why = "reclaim failed"; }
        /* And the reclaimed region really is reusable. */
        if (ok) {
            uint64_t fresh = 0;
            (void)it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&fresh);
            if (fresh < before) { ok = 0; why = "reset did not reclaim"; }
        }
    }

    it_slot_delete(T299_SLOT_PROC);
    it_slot_delete((uint32_t)(pool >= 0 ? (uint32_t)pool : 0u));
    if (ok) it_pass("T299"); else it_fail("T299", why);
}

void test_t300(void) {
    int ok = 1;
    const char *why = "vmo budget";

    if (!it_setup_self_vspace()) { it_fail("T300", "vspace self"); return; }

    long pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                     IRIS_KOBJ_UNTYPED, 256u * 1024u);
    if (pool < 0) { it_fail("T300", "pool carve"); return; }

    uint64_t before = 0, after = 0;
    if (it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&before) != 0) {
        it_fail("T300", "info"); return;
    }

    /* 3. the budget is a capability of a specific type. */
    if (it_retype2_at((long)IRIS_CPTR_SVCMGR_EP, IRIS_KOBJ_FRAME,
                      S1_SLOT_E, 1u, 4096) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "endpoint accepted as budget";
    }
    it_slot_delete(S1_SLOT_E);

    /* 1. a VMO against that budget consumes it. */
    long vmo = -1;
    if (ok) {
        if (it_retype2_at(pool, IRIS_KOBJ_FRAME, S1_SLOT_E, 1u,
                          3u * 4096u) != 0) {
            ok = 0; why = "create against budget";
        } else vmo = (long)S1_SLOT_E;
    }
    /* Pages are populated at map time, so map first, then measure. */
    if (ok && it_invoke(vmo, INV_FRAME_MAP, IT_VS, (long)T300_VA, 1) != 0) {
        ok = 0; why = "map";
    }
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
        ok = 0; why = "info2";
    }
    if (ok && before - after < 3u * 4096u) { ok = 0; why = "pages not charged"; }

    /* 2. the memory is real, and clean. */
    if (ok) {
        volatile uint64_t *pg = (volatile uint64_t *)(uintptr_t)T300_VA;
        if (pg[0] != 0u) { ok = 0; why = "vmo not zero-filled"; }
        if (ok) {
            pg[0] = 0x5A5A5A5A5A5A5A5AULL;
            if (pg[0] != 0x5A5A5A5A5A5A5A5AULL) { ok = 0; why = "vmo not writable"; }
        }
    }

    /* 4. while it lives the budget is bound; once it is gone, reclaimable. */
    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) == 0) {
        ok = 0; why = "budget reset while bound";
    }
    if (ok) {
        (void)it_invoke2(vmo, INV_FRAME_UNMAP, IT_VS, (long)T300_VA);
        it_slot_delete(S1_SLOT_E);
        it_quiesce_reaper();
        if (it_invoke0(pool, INV_UNTYPED_RESET) != 0) {
            ok = 0; why = "budget not reclaimable";
        }
    }

    it_slot_delete(S1_SLOT_E);
    if (ok) it_pass("T300"); else it_fail("T300", why);
}

void test_t301(void) {
    int ok = 1;
    const char *why = "refused retype residue";
    int saw_built = 0, saw_refusal = 0;

    it_slot_delete(T301_SLOT_VS);

    for (uint32_t pages = 1u; ok && pages <= T301_MAX_PAGES; pages++) {
        long pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                         IRIS_KOBJ_UNTYPED, (long)(pages * 4096u));
        if (pool < 0) { ok = 0; why = "pool carve"; break; }

        struct it_utq_one before, after;
        if (!it_utq_1(pool, &before)) { ok = 0; why = "query"; break; }

        it_slot_delete(T301_SLOT_VS);
        long r = it_retype2_at(pool, IRIS_KOBJ_VSPACE, T301_SLOT_VS, 1u, 4096);

        if (r == 0) {
            saw_built = 1;
            it_slot_delete(T301_SLOT_VS);
            it_quiesce_reaper();
        } else if (r == (long)IRIS_ERR_NO_MEMORY) {
            saw_refusal = 1;
            /* 1+2. the refusal kept nothing. */
            if (!it_utq_1(pool, &after)) { ok = 0; why = "query2"; }
            else if (after.child_count != before.child_count) {
                ok = 0; why = "refused retype left a child";
            }
            /* 3. and the region is still whole. */
            if (ok && it_invoke0(pool, INV_UNTYPED_RESET) != 0) {
                ok = 0; why = "budget not reclaimable after refusal";
            }
        } else {
            ok = 0; why = "unexpected retype error";
        }

        it_slot_delete((uint32_t)pool);
    }

    /* 4. the sweep straddled the boundary, so the rounds above were real. */
    if (ok && !saw_built)   { ok = 0; why = "no budget built one"; }
    if (ok && !saw_refusal) { ok = 0; why = "no budget was refused"; }

    it_slot_delete(T301_SLOT_VS);
    if (ok) it_pass("T301"); else it_fail("T301", why);
}

void test_t302(void) {
    int ok = 1;
    const char *why = "page table capability";

    if (!it_setup_self_vspace()) { it_fail("T302", "vspace self"); return; }

    long pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                     IRIS_KOBJ_UNTYPED, 256u * 1024u);
    if (pool < 0) { it_fail("T302", "pool carve"); return; }

    /* 2. a level is one page, or it is not a level. */
    it_slot_delete(T302_SLOT_PT);
    if (it_retype2_at(pool, IRIS_KOBJ_PAGE_TABLE, T302_SLOT_PT, 1u, 8192)
        != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "two-page table accepted";
    }
    it_slot_delete(T302_SLOT_PT);

    uint64_t before = 0, after = 0;
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&before) != 0) {
        ok = 0; why = "info";
    }

    /* 1. a real object of its own type. */
    long pt = -1;
    if (ok) {
        pt = it_retype_slot_alloc(pool, IRIS_KOBJ_PAGE_TABLE, 4096);
        if (pt < 0) { ok = 0; why = "retype"; }
    }
    if (ok && it_invoke0(pt, INV_CAP_IDENTIFY) != (long)IRIS_HANDLE_TYPE_PAGE_TABLE) {
        ok = 0; why = "wrong type reported";
    }

    /* 5. the address is authority, not a hint.  Probed BEFORE the productive
     *    installs so a refusal cannot be confused with "already complete". */
    if (ok && it_invoke2(pt, INV_PAGE_TABLE_MAP, IT_VS, (long)0xFFFF800000000000ULL)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "kernel-half install accepted";
    }
    if (ok && it_invoke2(pt, INV_PAGE_TABLE_MAP, IT_VS, 0x1000L)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "low address install accepted";
    }

    /* 3. fill the window, one invocation per missing level, until the walk is
     *    complete.  HOW MANY levels are missing is not the test's to assume:
     *    an address space that already maps something in this 512 GiB slot
     *    shares its PDPT, so the depth depends on what the caller has mapped
     *    before.  What is asserted is the contract — each install fills
     *    exactly one level, and the run ends with the walk reporting complete. */
    uint32_t installed = 0;
    if (ok && it_invoke2(pt, INV_PAGE_TABLE_MAP, IT_VS, (long)T302_VA) != 0) {
        ok = 0; why = "first install";
    } else if (ok) {
        installed = 1;
    }
    /* 4. and that table is spent: it is part of a walk now.  BUSY, not
     *    ALREADY_EXISTS — a client loop has to tell "this object is spent, get
     *    another" from "this level is already there, stop", and one code for
     *    both left it unable to act on either. */
    if (ok && it_invoke2(pt, INV_PAGE_TABLE_MAP, IT_VS, (long)(T302_VA + 0x40000000ULL))
              != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "spent table not refused as BUSY";
    }

    int complete = 0;
    for (int lvl = 0; ok && lvl < 4 && !complete; lvl++) {
        long more = it_retype_slot_alloc(pool, IRIS_KOBJ_PAGE_TABLE, 4096);
        if (more < 0) { ok = 0; why = "retype level"; break; }
        long mr = it_invoke2(more, INV_PAGE_TABLE_MAP, IT_VS, (long)T302_VA);
        if (mr == 0)                                  installed++;
        else if (mr == (long)IRIS_ERR_ALREADY_EXISTS) complete = 1;
        else if (mr == (long)IRIS_ERR_BUSY)           { ok = 0; why = "fresh table reported spent"; }
        else { ok = 0; why = "install level"; }
    }
    if (ok && !complete)     { ok = 0; why = "walk never completed"; }
    if (ok && installed == 0){ ok = 0; why = "nothing was ever installed"; }

    /* 6. every level came out of the budget the holder named. */
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
        ok = 0; why = "info2";
    }
    if (ok && before - after < (uint64_t)installed * 4096u) {
        ok = 0; why = "levels not charged";
    }

    /* 7. and the walk the holder built is REAL: a mapping through it works.
     *    This is what separates installing a table from writing a number into
     *    a page — the levels above this address are ones the holder retyped
     *    and named, and memory mapped under them reads and writes. */
    if (ok) {
        long vmo = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
        if (vmo < 0) { ok = 0; why = "vmo"; }
        else if (it_invoke(vmo, INV_FRAME_MAP, IT_VS, (long)T302_VA, 1) != 0) {
            ok = 0; why = "map through holder-built walk";
        } else {
            volatile uint64_t *pg = (volatile uint64_t *)(uintptr_t)T302_VA;
            if (pg[0] != 0u) { ok = 0; why = "not zero-filled"; }
            if (ok) {
                pg[0] = 0xA5A5A5A5A5A5A5A5ULL;
                if (pg[0] != 0xA5A5A5A5A5A5A5A5ULL) { ok = 0; why = "not writable"; }
            }
            (void)it_invoke2(vmo, INV_FRAME_UNMAP, IT_VS, (long)T302_VA);
        }
    }

    it_slot_delete(T302_SLOT_PT);
    if (ok) it_pass("T302"); else it_fail("T302", why);
}

/* ── T303: a running thread outlives every capability to it (Stage 7) ─────
 * A thread has two owners and they are independent: whoever holds its TCB
 * capability, and the scheduler that is running it.  A pool-born thread got
 * the scheduler's reference from ktcb_object_init — its refcount of 1 WAS the
 * scheduler's.  A RETYPED thread's refcount of 1 belongs to the CSpace slot
 * the retype published it into, so until Stage 7 the slot was the only owner:
 * SYS_TCB_CONFIGURE built the execution state without taking a reference for
 * the scheduler that would run it.
 *
 * Deleting that slot after starting the thread therefore destroyed the
 * thread's storage while it was the thing executing — the block was returned
 * to its Untyped and zeroed underneath a live kernel stack.  It stayed
 * invisible for two stages because the only caller (init's S8 selftest) held
 * its slot forever.  A spawner does not: the child's TCB is the spawner's to
 * drop once the child runs, and svc_loader drops it on every spawn.
 *
 * Asserted here, on the suite's own thread so the observation is direct:
 *
 *   1. a thread composed from capabilities runs;
 *   2. dropping the LAST capability to it does not stop it or corrupt it —
 *      it keeps running and keeps producing the right answers;
 *   3. and the budget it was retyped from is still held, because a live
 *      thread is a child of that region whoever holds the capability.
 *
 * Leg 2 is the regression: before the fix it was a page fault in
 * context_switch with a zeroed kernel stack pointer, three call levels from
 * anything that named a TCB.
 * Invariants: O5, S-gate. */
static volatile uint32_t g_t303_ticks;
static volatile int      g_t303_go;

static void t303_thread(void) {
    while (g_t303_go) {
        g_t303_ticks++;
        it_sys0(SYS_YIELD);
    }
    it_invoke0(0, INV_TCB_EXIT);
    for (;;) it_sys0(SYS_YIELD);
}

static uint8_t g_t303_stack[8192] __attribute__((aligned(16)));

void test_t303(void) {
    int ok = 1;
    const char *why = "thread outlives its capability";

    if (!it_setup_self_vspace()) { it_fail("T303", "vspace self"); return; }
    long cs = it_cspace_self();
    if (cs < 0) { it_fail("T303", "cspace self"); return; }

    struct it_utq_one q0, q1;
    if (!it_utq_1((long)IRIS_CPTR_TEST_UNTYPED, &q0)) {
        it_fail("T303", "query"); return;
    }

    long tcb = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_TCB, 0);
    if (tcb < 0) { it_fail("T303", "tcb retype"); return; }

    g_t303_go    = 1;
    g_t303_ticks = 0;

    uint64_t entry = (uint64_t)(uintptr_t)t303_thread;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t303_stack + sizeof(g_t303_stack)))
                     & ~0xFULL;
    if (it_invoke(tcb, INV_TCB_CONFIGURE, cs, IT_VS, 0) != 0) {
        ok = 0; why = "configure";
    }
    if (ok && it_invoke(tcb, INV_TCB_WRITE_REGS, (long)entry, (long)(rsp - 8), 0) != 0) {
        ok = 0; why = "write regs";
    }
    if (ok && it_invoke0(tcb, INV_TCB_RESUME) != 0) { ok = 0; why = "resume"; }

    /* 1. it runs. */
    if (ok) {
        for (int i = 0; i < 400 && g_t303_ticks == 0; i++) it_sys0(SYS_YIELD);
        if (g_t303_ticks == 0) { ok = 0; why = "thread never ran"; }
    }

    /* 2. drop the LAST capability to it while it is running. */
    if (ok) {
        it_slot_delete((uint32_t)tcb);
        uint32_t seen = g_t303_ticks;
        for (int i = 0; i < 400 && g_t303_ticks == seen; i++) it_sys0(SYS_YIELD);
        if (g_t303_ticks == seen) { ok = 0; why = "thread stopped when its cap went"; }
    }

    /* 3. and the region still counts it: a live thread is a child of the
     *    budget it was retyped from, capability or no capability. */
    if (ok && !it_utq_1((long)IRIS_CPTR_TEST_UNTYPED, &q1)) { ok = 0; why = "query2"; }
    if (ok && q1.child_count <= q0.child_count) {
        ok = 0; why = "live thread not charged";
    }

    /* Let it exit on its own; its storage goes back with the last owner. */
    g_t303_go = 0;
    it_quiesce_reaper();

    if (ok) it_pass("T303"); else it_fail("T303", why);
}

void test_t304(void) {
    int ok = 1;
    const char *why = "live process ceiling";
    uint32_t got = 0;
    long fail_rc = 0;

    it_quiesce_reaper();
    it_slot_delete(T304_CN_SLOT);
    it_slot_delete(T304_VS_SLOT);
    it_slot_delete(T304_CS_SLOT);

    /* One budget for the whole experiment, laddered so a small boot block
     * still runs the test rather than skipping it silently. */
    static const uint64_t sizes[3] = { 4u << 20, 2u << 20, 1u << 20 };
    long pool = -1;
    for (uint32_t i = 0; i < 3u && pool < 0; i++)
        pool = s1_sub_ut(sizes[i]);
    if (pool < 0) { it_fail("T304", "pool carve"); return; }

    /* The process capabilities need somewhere to live that is not the rotating
     * object pool — 80 of them are held at once, which that pool would wrap. */
    if (it_invoke(pool, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_CNODE | (1ULL << 32)), (long)((uint64_t)T304_CN_SLOT << 32), 128) != 0) {
        it_fail("T304", "cnode carve"); return;
    }

    /*
     * Stage 7-proc: the thing being counted is a THREAD.
     *
     * This used to create 80 processes, each composed of an address space and
     * a CSpace, because a process was the object a ceiling had been invented
     * for.  There is no process object: a "process" is threads sharing a
     * CSpace and a VSpace.  The claim is unchanged and the subject is simpler
     * — how many of a kernel object can one budget hold, and what stops you —
     * so the loop retypes TCBs and the answer must be "the budget, cleanly".
     */
    for (uint32_t i = 0; i < T304_MAX; i++) {
        long r = it_invoke(pool, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_TCB | (1ULL << 32)), (long)((uint64_t)T304_CN_SLOT | ((uint64_t)(i + 1u) << 32)), 0);
        if (r != 0) { fail_rc = r; break; }
        got++;
    }

    /* 1. the number that used to be refused. */
    if (got < T304_TARGET) { ok = 0; why = "fewer than 65 live objects"; }
    /* 2. and whatever stopped it said so cleanly. */
    if (ok && got < T304_MAX && fail_rc >= 0) { ok = 0; why = "stopped without an error"; }

    /* 3. delete every capability — none ever ran, so this IS the never-started
     *    reclamation — then the region must RESET, which it refuses while a
     *    single child of it is still alive. */
    for (uint32_t i = 0; i < got; i++)
        (void)it_invoke1((long)T304_CN_SLOT, INV_CNODE_DELETE, (long)(i + 1u));
    it_slot_delete(T304_VS_SLOT);
    it_slot_delete(T304_CS_SLOT);
    it_slot_delete(T304_CN_SLOT);
    it_quiesce_reaper();

    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) != 0) {
        ok = 0; why = "budget not fully returned";
    }
    it_slot_delete((uint32_t)pool);

    if (ok) it_pass("T304"); else it_fail("T304", why);
}

/* ── T305: every capability is traceable to an ancestor (charter A9) ──────
 *
 * A LEGACY_ROOT is a capability sitting in a CSpace with NO parent in the
 * derivation tree.  It is not reachable by revoking anything: SYS_CSPACE_REVOKE
 * walks descendants, and a root has no ancestor to be a descendant of.  The
 * ABI calls the gauge "must → 0" and charter A9 claims the property is MET,
 * and until this test nothing in the tree ever read the number.
 *
 * So this test does two things.  It ASSERTS the inventory — the count is a
 * known, bounded set of roots, not an open-ended leak — and it asserts the
 * count does not GROW across a spawn/fault/kill cycle, which is the shape a
 * new productive producer would have.
 *
 * The known roots today (Stage 7):
 *   - the boot path: the root task's initial capabilities, installed by the
 *     kernel before any CSpace exists to derive from.  seL4's BootInfo caps
 *     are roots too; this class is legitimate and permanent.
 *   - KVmo publishes: a VMO is fabricated from kernel memory rather than
 *     retyped from an Untyped, so it has no capability ancestor to name.
 *     Retires with the object (ledger D-5, memory server).
 *   - fault delivery: the faulting thread's capability published into a
 *     mailbox.  This one is NOT legitimate — its natural ancestor is the TCB
 *     slot the registrant named when it armed the handler, exactly as an
 *     IPC-delivered capability is a child of the sender's source slot
 *     (Stage 2).  Recorded in the roadmap as the A9 gap to close.
 * Invariants: A9. */
uint32_t it_ipc_buffer_gauge(void) {
    struct it_utq_global q;
    if (!it_utq_g(&q)) return 0xFFFFFFFFu;
    return q.ipc_buffers;
}

void test_t305(void) {
    struct it_utq_mdb q0, q1;
    int ok = 1;
    const char *why = "legacy roots";

    it_quiesce_reaper();
    if (!it_utq_mdb(&q0)) { it_fail("T305", "query"); return; }

    /*
     * A CEILING, not just a no-growth check.
     *
     * The no-growth check below catches a NEW producer of unparented
     * capabilities appearing during the cycle it runs.  It cannot catch one
     * that was there at boot, and that is exactly what A-14 was: an open-coded
     * `< 1024` in RETYPE2 published every object retyped from a second-level
     * Untyped as a root, for the whole life of the system, and this test
     * printed the number and passed.
     *
     * What is left is the BOOT PATH, which is legitimate and permanent —
     * seL4's BootInfo capabilities are roots too — plus a fault delivery whose
     * registration slot no longer holds the thread, which is the documented
     * honest failure.  The number may go DOWN.  If it goes up, something
     * started publishing without an ancestor and the change that did it is the
     * one to look at.
     */
    if (q0.mdb_legacy_roots > IT_MDB_LEGACY_ROOT_CEILING) {
        ok = 0; why = "legacy roots above the boot-path ceiling";
    }

    it_serial_write("[IRIS][TEST] T305 mdb_legacy_roots=");
    it_log_num(q0.mdb_legacy_roots);
    it_serial_write(" nodes_live="); it_log_num(q0.mdb_nodes_live);
    it_serial_write(" max_depth="); it_log_num(q0.mdb_max_depth);
    it_serial_write(" orphans="); it_log_num(q0.mdb_orphan_promotions);
    it_serial_write(" reparents="); it_log_num(q0.mdb_reparents);
    it_serial_write("\n");

    /* A full spawn/kill cycle plus a mint/revoke cycle: between them these
     * exercise every productive path that installs a capability into a CSpace
     * — retype, publish, mint, IPC delivery and teardown.  A new producer of
     * unparented capabilities would show up as a count that does not come
     * back down. */
    {
        handle_id_t cmd = HANDLE_INVALID, proc = HANDLE_INVALID;
        long ep = it_ep_create();
        if (ep < 0) { ok = 0; why = "ep"; }
        else {
            cmd = (handle_id_t)ep;
            if (lp_spawn_child(cmd, &proc) < 0 || proc == HANDLE_INVALID) {
                ok = 0; why = "spawn";
            } else {
                (void)it_kill((long)proc);
                (void)it_lp_wait_exit(proc);
            }
            it_close(&proc); it_close(&cmd);
        }
    }
    if (ok) {
        long n = it_notify_create_slot();
        if (n < 0) { ok = 0; why = "notif"; }
        else {
            long d = it_cs_reduce(n, RIGHT_READ);
            if (d < 0) { ok = 0; why = "mint"; }
            else if (it_invoke0(n, INV_CSPACE_REVOKE) < 0) { ok = 0; why = "revoke"; }
            handle_id_t nh = (handle_id_t)n; it_close(&nh);
        }
    }
    it_quiesce_reaper();

    if (ok && !it_utq_mdb(&q1)) { ok = 0; why = "query2"; }
    /* The cycle must not leave a root behind: whatever the delivery installed
     * is gone with the mailbox slot. */
    if (ok && q1.mdb_legacy_roots > q0.mdb_legacy_roots) {
        ok = 0; why = "legacy roots grew";
        it_serial_write("[IRIS][TEST] T305 grew to="); it_log_num(q1.mdb_legacy_roots);
        it_serial_write("\n");
    }

    /*
     * And the specific claim D-6 is about: a FAULT DELIVERY installs a
     * parented capability, not a root.  The kernel publishes the faulting
     * thread into a mailbox while the fault is pending, so the count is read
     * with the capability still sitting there — if it were installed as a root
     * the number would be strictly higher here than before the fault.
     */
    if (ok) {
        struct t25_tgt g;
        struct it_utq_mdb qa, qb;
        if (!t25_tgt_spawn(&g, &why)) { ok = 0; }
        /* Measured AFTER the spawn: a spawn creates objects of its own —
         * including VMOs, which are a known root class until the memory server
         * lands — and this assertion is about the DELIVERY, so the window has
         * to contain only that. */
        else if (!it_utq_mdb(&qa)) { ok = 0; why = "query3"; }
        else {
            struct it_fault f;
            if (it_lp_cmd_va(g.cmd, LP_CMD_FAULT_READ, T25_VA_A) != 0) { ok = 0; why = "fault cmd"; }
            if (ok && !t25_wait_fault(&g, &f)) { ok = 0; why = "fault pending"; }
            /* Read WITH the delivered capability live in the mailbox. */
            if (ok && !it_utq_mdb(&qb)) { ok = 0; why = "query4"; }
            if (ok && qb.mdb_legacy_roots > qa.mdb_legacy_roots) {
                ok = 0; why = "fault delivered an unparented cap";
                it_serial_write("[IRIS][TEST] T305 fault roots "); it_log_num(qa.mdb_legacy_roots);
                it_serial_write(" -> "); it_log_num(qb.mdb_legacy_roots);
                it_serial_write("\n");
            }
            t25_tgt_reap(&g);
        }
        it_quiesce_reaper();
    }

    if (ok) it_pass("T305"); else it_fail("T305", why);
}

/* ── T306: a CNode capability carries a GUARD (Stage 8-cap, ledger D-2) ───
 *
 * Until this stage IRIS resolved a CPtr as a pure radix walk: each level ate
 * ctz(slot_count) bits and indexed.  seL4 puts a GUARD in the CNode
 * capability — bits the address must carry to descend through it — which is
 * what lets a CSpace be sparse and lets levels be skipped instead of costing
 * their full radix.
 *
 * The property that makes it seL4's guard rather than some other feature is
 * that it belongs to the CAPABILITY, not the CNode: two capabilities to one
 * CNode can be guarded differently, so one holder's view can be made sparse
 * without touching anybody else's.  The host suite asserts that directly
 * (test_cnode_guard G-7); what this test adds is the ring-3 half — the guard
 * is installed and observed THROUGH the syscall boundary, by a task that sees
 * the kernel only as syscalls, which is the only place a CSpace addressing
 * change can be proven not to have quietly aliased something.
 *
 * Asserted here:
 *   1. the default is no guard and the plain address resolves (the additivity
 *      claim: every CPtr that predates guards still means what it meant);
 *   2. after installing one, the GUARDED address resolves;
 *   3. and the plain address does NOT — the bits are really consumed and
 *      compared, not merely stored;
 *   4. a wrong guard fails, and fails as NOT_FOUND rather than landing on some
 *      other slot;
 *   5. width 0 removes it and restores the original address;
 *   6. a guard is refused on a capability that is not a CNode.
 */
void test_t306(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cnode guard";

    /* A fresh 8-slot CNode, left in its leaf slot: the capability IS the slot,
     * and that slot is what carries the guard. */
    long cn = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                   IRIS_KOBJ_CNODE, 8);
    if (cn < 0) { it_fail("T306", "retype cnode"); return; }

    /* Something to find on the far side of it: a copy of our own root CNode
     * capability, minted into slot 3 of the new CNode. */
    if (ok && it_invoke2((long)IRIS_CPTR_TEST_UNTYPED, INV_CSPACE_MINT, (long)((uint64_t)cn | ((uint64_t)3u << 32)), (long)RIGHT_READ) != 0) {
        ok = 0; why = "mint into guarded cnode";
    }

    /* Path: root[80] -> obj cnode[leaf] -> new cnode[3].  `cn` already
     * addresses the first two levels; slot 3 sits above them at bit 16. */
    const long deep_plain = cn | (3L << 16);

    if (ok && it_invoke2(deep_plain, INV_CAP_IDENTIFY, 0, 0) < 0) {
        ok = 0; why = "plain address before guard";
    }

    /* Install a 2-bit guard of 0b10 on the CNode CAPABILITY (the slot `cn`
     * addresses), then re-derive the address with the guard above slot 3. */
    if (ok && it_invoke2(cn, INV_CSPACE_SET_GUARD, 0x2L, 2L) != 0) {
        ok = 0; why = "set guard";
    }
    const long deep_guarded = cn | (3L << 16) | (0x2L << 19);
    const long deep_wrong   = cn | (3L << 16) | (0x1L << 19);

    if (ok && it_invoke2(deep_guarded, INV_CAP_IDENTIFY, 0, 0) < 0) {
        ok = 0; why = "guarded address rejected";
    }
    if (ok && it_invoke2(deep_plain, INV_CAP_IDENTIFY, 0, 0) >= 0) {
        ok = 0; why = "plain address still resolves under a guard";
    }
    if (ok && it_invoke2(deep_wrong, INV_CAP_IDENTIFY, 0, 0) >= 0) {
        ok = 0; why = "wrong guard resolved";
    }

    /* Width 0 removes it; the address the CSpace had before guards existed
     * comes back unchanged. */
    if (ok && it_invoke2(cn, INV_CSPACE_SET_GUARD, 0L, 0L) != 0) {
        ok = 0; why = "clear guard";
    }
    if (ok && it_invoke2(deep_plain, INV_CAP_IDENTIFY, 0, 0) < 0) {
        ok = 0; why = "plain address not restored";
    }

    /* A guard on a non-CNode capability has no meaning and is refused, so it
     * can never make a slot lie about how it resolves. */
    if (ok && it_invoke2(deep_plain, INV_CSPACE_SET_GUARD, 0x1L, 1L)
              != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "guard accepted on non-cnode";
    }

    (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)((uint32_t)cn >> 8));
    it_quiesce_reaper();

    /* The CNode is gone and nothing else was created, so every gauge must be
     * back where it started: a guard is a field in a slot, not an allocation. */
    if (ok) {
        struct it_snap a = it_snap_take();
        ok = it_snap_baseline(&b, &a, &why);
    }
    if (ok) it_pass("T306"); else it_fail("T306", why);
}

/* ── T307: budget exhaustion is a FAULT a supervisor can answer (Stage 8-mcs)
 *
 * Ledger: the "MCS scheduling — partial" row.  IRIS enforced budget and period
 * — a thread that spent its budget blocked until the period refilled it — but
 * nothing was TOLD.  A temporal supervisor could not react to an overrun, and
 * a scheduling model where overrun is invisible is budget enforcement, not
 * MCS.  seL4 raises a Timeout fault to a handler the thread was given.
 *
 * This test is the proof that the mechanism actually fires, not merely that it
 * compiles: it starts a thread that does nothing but spin, gives it a budget
 * of one tick, arms a timeout handler, and waits.  What it asserts:
 *
 *   1. the handler is SIGNALLED — the thread really ran out and the kernel
 *      really delivered, from task context rather than the timer ISR;
 *   2. the record says IRIS_FAULT_VECTOR_TIMEOUT, so a handler can tell an
 *      overrun from a page fault and answer the right question;
 *   3. the thread is BLOCKED, not merely descheduled — it must not keep
 *      running on a budget it does not have;
 *   4. and the supervisor can end it, which is the authority the fault exists
 *      to grant.
 *
 * A thread with NO handler armed is the default and the pre-Stage-8 path, and
 * every other test in this suite is that case: they all still pass, which is
 * what says this was additive.
 */
static uint8_t g_t307_stack[8192];
static volatile int g_t307_spun;

static void t307_burn(void) {
    /* Spin.  The point is to be RUNNABLE and to consume ticks; the volatile
     * write keeps the loop from being optimised away. */
    for (;;) g_t307_spun++;
}

void test_t307(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "timeout fault";

    /* A-22: a timeout fault is IPC too, on its own endpoint — a temporal
     * supervisor is a server like a pager is. */
    long notif = it_ep_create();
    if (notif < 0) { it_fail("T307", "fault ep"); return; }

    uint64_t entry = (uint64_t)(uintptr_t)t307_burn;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t307_stack + sizeof(g_t307_stack))) & ~0xFULL;
    long tcb = it_thread_create(entry, rsp, 0);
    if (tcb < 0) { it_fail("T307", "thread"); return; }

    /* One tick of budget in a long period: it overruns almost immediately and
     * would not come back on its own for the rest of the period. */
    long sc = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                   IRIS_KOBJ_SCHED_CONTEXT, 0);
    if (sc < 0) { it_fail("T307", "sc"); return; }
    if (ok && it_invoke(sc, INV_SC_CONFIGURE, 1, 1000, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "sc configure"; }
    if (ok && it_invoke1(sc, INV_SC_BIND, tcb) != 0)          { ok = 0; why = "sc bind"; }

    /* Arm the timeout handler at that endpoint. */
    const uint32_t leaf = 3u;
    if (ok && it_invoke(tcb, INV_TCB_SET_TIMEOUT_HANDLER, notif, 0L, 0L) != 0) {
        ok = 0; why = "arm timeout handler";
    }

    /* (1) the handler is told, and (2) the message SAYS what happened — one
     * receive where it used to be a signal followed by a second syscall to
     * fetch what the signal could not carry. */
    struct it_fault tf;
    if (ok && !it_fault_wait_ep(notif, leaf)) {
        ok = 0; why = "timeout fault never delivered";
    }
    if (ok && it_fault_info(leaf, &tf) != 0) { ok = 0; why = "no fault record"; }
    if (ok && tf.vector != IRIS_FAULT_VECTOR_TIMEOUT) {
        ok = 0; why = "wrong fault vector";
    }

    /* (3) the thread is blocked on the fault, not still burning budget. */
    if (ok) {
        int before = g_t307_spun;
        for (volatile int i = 0; i < 200000; i++) { }
        (void)it_sys0(SYS_YIELD);
        if (g_t307_spun != before) { ok = 0; why = "thread still running after overrun"; }
    }

    /* (4) the supervisor ends it — the authority the fault delivers.  It
     * refuses to answer, and a fault nobody will answer destroys the thread. */
    if (ok && it_fault_kill(leaf) != 0) {
        ok = 0; why = "kill after timeout fault";
    }

    it_quiesce_reaper();
    if (ok) it_pass("T307"); else it_fail("T307", why);
}

/* ── T308: a PASSIVE server runs on its client's time (Stage 8-mcs) ───────
 *
 * The last MCS pillar: scheduling context DONATION.  A thread with no SC of
 * its own is passive — it has no time and cannot run on its own account.  When
 * a client Calls it, the client's SC is lent for the duration, so the server
 * runs on the requester's budget and gives it back on reply.  That is what
 * makes time an authority a client delegates rather than something a server is
 * born holding, and it is why a passive server can neither be starved into
 * uselessness nor spun up by a client with nothing to give.
 *
 * The discriminating observation, and the reason this test is worth its setup:
 * BEFORE donation, a thread with sched_ctx == NULL was never charged at all —
 * an SC-less server ran with UNLIMITED time, which is the opposite of the
 * property MCS is for.  So the test arms a timeout handler on the SERVER and
 * has it spin:
 *
 *   - with donation, the server spends the CLIENT's budget, exhausts it, and
 *     the timeout fault fires — which can only happen if the server was
 *     charged to a scheduling context it does not own;
 *   - without it, the server spins unbudgeted, no fault ever comes, and the
 *     bounded wait below fails cleanly instead of hanging.
 *
 * Three threads because the client has to be able to block in EP_CALL while
 * somebody else observes: the suite thread is the supervisor.
 */
static uint8_t g_t308_srv_stack[8192];
static uint8_t g_t308_cli_stack[8192];
static volatile long g_t308_ep;
static volatile long g_t308_reply;
static volatile int  g_t308_served;

static void t308_server(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    /* Passive: no SC.  Blocks here until a client donates the time to run. */
    (void)(m.reply = (long)g_t308_reply, iris_msg_recv((long)g_t308_ep, &m));
    g_t308_served = 1;
    /* Spin on the borrowed budget.  With donation this is bounded by the
     * client's budget and ends in a timeout fault; without it, it is not
     * bounded by anything, which is the bug. */
    for (;;) { }
}

/*
 * The client waits to be told to go, and that is not politeness.
 *
 * The claim is about a client WITH a budget calling a passive server, and the
 * budget is bound after this thread is created — there is no way round that,
 * since binding a scheduling context needs a thread to bind it to.  With one
 * processor the ordering came free: the supervisor held the CPU until it chose
 * to yield, so the client could not possibly run in between.  With four, this
 * thread starts on another core the instant it is created and can make its
 * call before the bind — and a call from a client with NO scheduling context
 * donates nothing, so the server runs unbudgeted, never overruns, and the
 * fault this test waits for never comes.
 */
static volatile int g_t308_go;

static void t308_client(void) {
    while (!g_t308_go) { (void)it_sys0(SYS_YIELD); }
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label = 0x8CULL;
    (void)iris_msg_call((long)g_t308_ep, &m);
    it_sys1(SYS_EXIT, 0);
    for (;;) { }
}

void test_t308(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "sc donation";

    long ep = it_ep_create_slot();
    long rp = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
    long notif = it_ep_create_slot();   /* A-22: the server's timeout endpoint */
    if (ep < 0 || rp < 0 || notif < 0) { it_fail("T308", "objects"); return; }
    g_t308_ep = ep; g_t308_reply = rp; g_t308_served = 0; g_t308_go = 0;

    /* The server: started with NO scheduling context. */
    long srv = it_thread_create((uint64_t)(uintptr_t)t308_server,
                                ((uint64_t)(uintptr_t)(g_t308_srv_stack +
                                    sizeof(g_t308_srv_stack))) & ~0xFULL, 0);
    if (srv < 0) { it_fail("T308", "server thread"); return; }

    /* Arm the server's timeout handler BEFORE it can overrun, so the fault
     * cannot be missed between exhaustion and registration. */
    const uint32_t leaf = 2u;
    if (ok && it_invoke(srv, INV_TCB_SET_TIMEOUT_HANDLER, notif, 0L, 0L) != 0) {
        ok = 0; why = "arm server timeout";
    }

    /* Let the server reach EP_RECV before anybody calls it.  A plain delay and
     * not an IT_AWAIT: `g_t308_served` is set AFTER the server's receive
     * returns, which cannot happen until the client below exists, so there is
     * no condition here to wait ON. */
    if (ok) it_settle(1);

    /* The client: a small budget in a long period, so what the server spends
     * is visibly the CLIENT's and runs out quickly. */
    long cli = it_thread_create((uint64_t)(uintptr_t)t308_client,
                                ((uint64_t)(uintptr_t)(g_t308_cli_stack +
                                    sizeof(g_t308_cli_stack))) & ~0xFULL, 0);
    if (cli < 0) { it_fail("T308", "client thread"); return; }

    long sc = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                   IRIS_KOBJ_SCHED_CONTEXT, 0);
    if (sc < 0) { it_fail("T308", "sc"); return; }
    if (ok && it_invoke(sc, INV_SC_CONFIGURE, 3, 4000, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "sc configure"; }
    if (ok && it_invoke1(sc, INV_SC_BIND, cli) != 0)          { ok = 0; why = "sc bind"; }

    /* Now it has a budget to lend. */
    g_t308_go = 1;

    /*
     * The assertion.  A timeout fault on the SERVER can only happen if the
     * server was charged against a scheduling context — and it has none of its
     * own, so it must be the client's.  Bounded wait: no donation means no
     * fault and a clean TIMED_OUT rather than a hung suite.
     */
    struct it_fault sf;
    if (ok && !it_fault_wait_ep(notif, leaf)) {
        ok = 0; why = "server never charged to the donated SC";
    }
    if (ok && it_fault_info(leaf, &sf) != 0) { ok = 0; why = "no fault record on server"; }
    if (ok && sf.vector != IRIS_FAULT_VECTOR_TIMEOUT) { ok = 0; why = "wrong vector"; }

    /* Tear down: killing the server cancels the reply binding, which is the
     * path that returns the loan to a client that never got its reply. */
    (void)it_fault_kill(leaf);
    (void)it_invoke1(sc, INV_SC_BIND, 0L);
    (void)it_invoke0(cli, INV_TCB_EXIT);
    (void)it_invoke0(srv, INV_TCB_EXIT);
    it_quiesce_reaper();
    /* Release what this test made.  It used to leave all of it in the rotating
     * pool, where the allocator would delete it under a later test. */
    it_slot_delete((uint32_t)sc);
    it_slot_delete((uint32_t)notif);
    it_slot_delete((uint32_t)rp);
    it_slot_delete((uint32_t)ep);

    if (ok) it_pass("T308"); else it_fail("T308", why);
}
static uint8_t g_t309_srv_stack[8192];
static uint8_t g_t309_cli_stack[8192];
static volatile long g_t309_ep;
static volatile long g_t309_reply;
static volatile int  g_t309_replies;
static volatile int  g_t309_bad;
static volatile int  g_t309_done;

static void t309_server(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    /* Passive: no SC of its own.  Blocks until a client donates the time. */
    if ((m.reply = (long)g_t309_reply, iris_msg_recv((long)g_t309_ep, &m)) != 0) {
        for (;;) { }
    }
    for (;;) {
        /* Answer this one and wait for the next, atomically. */
        m.label = m.label + 1ULL;              /* the service: n -> n+1 */
        m.reply = (long)g_t309_reply;
        if (iris_msg_reply_recv((long)g_t309_ep, &m) != 0)
            break;
    }
    for (;;) { }
}

static void t309_client(void) {
    for (int i = 0; i < T309_ROUNDS; i++) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = (uint64_t)(0x300 + i);
        if (iris_msg_call((long)g_t309_ep, &m) != 0) { g_t309_bad = 1; break; }
        if (m.label != (uint64_t)(0x300 + i + 1)) { g_t309_bad = 1; break; }
        g_t309_replies++;
    }
    g_t309_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) { }
}

void test_t309(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "reply_recv loop";

    long ep = it_ep_create_slot();
    long rp = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_REPLY, 0);
    if (ep < 0 || rp < 0) { it_fail("T309", "objects"); return; }
    g_t309_ep = ep; g_t309_reply = rp;
    g_t309_replies = 0; g_t309_bad = 0; g_t309_done = 0;

    long srv = it_thread_create((uint64_t)(uintptr_t)t309_server,
                                ((uint64_t)(uintptr_t)(g_t309_srv_stack +
                                    sizeof(g_t309_srv_stack))) & ~0xFULL, 0);
    if (srv < 0) { it_fail("T309", "server thread"); return; }

    long cli = it_thread_create((uint64_t)(uintptr_t)t309_client,
                                ((uint64_t)(uintptr_t)(g_t309_cli_stack +
                                    sizeof(g_t309_cli_stack))) & ~0xFULL, 0);
    if (cli < 0) { it_fail("T309", "client thread"); return; }

    /* Give the client real time to donate: a budget large enough for the whole
     * conversation, so what is being tested is the loop and not exhaustion. */
    long sc = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                   IRIS_KOBJ_SCHED_CONTEXT, 0);
    if (sc < 0) { it_fail("T309", "sc"); return; }
    if (ok && it_invoke(sc, INV_SC_CONFIGURE, 200, 400, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "sc configure"; }
    if (ok && it_invoke1(sc, INV_SC_BIND, cli) != 0)           { ok = 0; why = "sc bind"; }

    /* Bounded: a server that stops after one request never sets done. */
    if (ok) IT_AWAIT(g_t309_done, 4000);

    if (ok && !g_t309_done) {
        ok = 0;
        why = (g_t309_replies == 0) ? "no request served at all"
            : (g_t309_bad ? "wrong reply payload" : "server stopped mid-loop");
        it_fz_note("T309", (uint32_t)g_t309_replies, (uint32_t)g_t309_bad, 0u);
    }
    if (ok && g_t309_bad)                   { ok = 0; why = "wrong reply payload"; }
    if (ok && g_t309_replies != T309_ROUNDS) { ok = 0; why = "server stopped early"; }

    (void)it_invoke1(sc, INV_SC_BIND, 0L);
    (void)it_invoke0(srv, INV_TCB_EXIT);
    (void)it_invoke0(cli, INV_TCB_EXIT);
    it_quiesce_reaper();
    it_slot_delete((uint32_t)sc);
    it_slot_delete((uint32_t)rp);
    it_slot_delete((uint32_t)ep);

    if (ok) it_pass("T309"); else it_fail("T309", why);
}

/* ── T310: a blocking syscall is RE-EXECUTED, not parked (Stage 9-evt) ────
 *
 * Ledger D-1, step 1.  seL4 is an event kernel: no thread blocks inside the
 * kernel.  A syscall that cannot finish records what it needs in the THREAD,
 * returns, and is re-executed when the thread runs again.  IRIS parked the
 * thread mid-syscall on an 8 KiB kernel stack instead, which is why it could
 * bound neither in-kernel latency nor kernel memory per thread.
 *
 * The subject was SYS_SLEEP, the first handler converted.  Ledger A-24 retired
 * it — a kernel that can block on time owns a policy about time — so the
 * property moved to the blocking syscall that remains the simplest:
 * SYS_NOTIFY_WAIT.  The claim is unchanged, and it is now made about a
 * mechanism that will still be here in ten years.
 *
 * From ring 3 a restartable wait and a stack-parked wait are
 * indistinguishable: both block and both wake.  So the assertion is on the
 * kernel's restart gauge, which only moves when a handler asked to be
 * re-entered.  Three things:
 *
 *   1. the wait really BLOCKS — nothing was pending when it was made, and it
 *      returned only once the timer service signalled;
 *   2. the restart counter ADVANCED across it, so the handler returned and was
 *      re-dispatched rather than resuming a parked frame;
 *   3. a wait that CAN complete — bits already pending — does not restart,
 *      because a syscall that can finish must never take the slow path.
 */
void test_t310(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "restartable wait";

    long n = it_notify_create();
    if (n < 0) { it_fail("T310", "notif"); return; }

    struct it_utq_global g0, g1, g2;
    if (!it_utq_g(&g0)) { it_fail("T310", "query"); return; }

    /* (3) a wait that needs no blocking must not restart. */
    if (ok && it_invoke1(n, INV_NOTIFY_SIGNAL, 0x2u) != 0) { ok = 0; why = "signal"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 || bits != 0x2u) {
            ok = 0; why = "pending wait failed";
        }
    }
    if (ok && !it_utq_g(&g1))             { ok = 0; why = "query"; }
    /* THIS THREAD'S restarts, not the machine's.  The global counter answers a
     * different question once other processors are running threads of their
     * own, and every claim below is about the syscall this thread just made. */
    if (ok && g1.syscall_restarts_self != g0.syscall_restarts_self) {
        ok = 0; why = "a wait that could finish took the restart path";
    }

    /* (1) and (2): a wait with nothing pending blocks, and blocking means
     *     re-execution.  The timer service is what ends it — which is also the
     *     shape A-24 left behind: waiting is somebody else's job. */
    if (ok) {
        long give = it_cs_reduce(n, RIGHT_WRITE | RIGHT_TRANSFER);
        uint64_t tok = 0;
        if (give < 0 || iris_timer_arm((long)IRIS_CPTR_TIMER_EP, give, 0x4ull,
                                       30000000ull, &tok) != 0) { ok = 0; why = "arm"; }
        it_xfer_release(give);       /* A-29: the copy was ours to give away */
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            (bits & 0x4ull) == 0) { ok = 0; why = "blocking wait failed"; }
    }
    if (ok && !it_utq_g(&g2))             { ok = 0; why = "query"; }
    if (ok && g2.syscall_restarts_self <= g1.syscall_restarts_self) {
        ok = 0; why = "blocking wait did not re-execute";
    }

    { handle_id_t h = (handle_id_t)n; it_close(&h); }
    it_quiesce_reaper();
    if (ok) it_pass("T310"); else it_fail("T310", why);
}

void test_t311(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "preemptible revoke";

    /* A source capability worth deriving from: a fresh endpoint. */
    long src = it_ep_create_slot();
    if (src < 0) { it_fail("T311", "source"); return; }

    /* Fan out more children than one slice can revoke. */
    /*
     * ABOVE the rotating pool, not inside it.
     *
     * These were leaves 100..123, which is inside the rotating object pool
     * (4..IT_OBJ_SLOT_SPAN): the source endpoint this test derives from is
     * itself allocated from that pool, and once the rotation reached 117 the
     * loop deleted its own source half way through and the fan-out stopped.
     * The codebase already had this hazard named for T307's mailbox — a fixed
     * slot inside a rotating range is a latent failure waiting for the test
     * count to change — and this is the second one it caught.
     */
    uint32_t made = 0;
    for (uint32_t i = 0; ok && i < T311_COPIES; i++) {
        uint32_t leaf = T311_LEAF_BASE + i;
        (void)it_invoke1((long)IT_OBJ_CNODE_SLOT, INV_CNODE_DELETE, (long)leaf);
        if (it_invoke2(src, INV_CSPACE_MINT, (long)(((uint64_t)leaf << 32) | (uint64_t)IT_OBJ_CNODE_SLOT), (long)RIGHT_SAME_RIGHTS) == 0)
            made++;
    }
    if (ok && made < T311_COPIES) { ok = 0; why = "could not build the subtree"; }

    struct it_utq_global g0, g1;
    if (ok && !it_utq_g(&g0)) { ok = 0; why = "query"; }

    long revoked = 0;
    if (ok) revoked = it_invoke0(src, INV_CSPACE_REVOKE);
    if (ok && revoked < 0) { ok = 0; why = "revoke failed"; }

    if (ok && !it_utq_g(&g1)) { ok = 0; why = "query"; }

    /* (1) it really gave the CPU up part-way. */
    if (ok && g1.syscall_restarts_self <= g0.syscall_restarts_self) {
        ok = 0; why = "revoke ran to completion without preempting";
    }
    /* (2) and still answered for the whole job, not just the last slice. */
    if (ok && (uint32_t)revoked != made) {
        ok = 0; why = "sliced revoke lost part of its count";
    }
    /* the subtree is actually gone */
    if (ok) {
        for (uint32_t i = 0; ok && i < T311_COPIES; i++) {
            if (it_invoke2((long)IT_OBJ_CPTR(T311_LEAF_BASE + i), INV_CAP_IDENTIFY, 0, 0) >= 0) {
                ok = 0; why = "descendant survived the revoke";
            }
        }
    }

    it_slot_delete((uint32_t)src);
    it_quiesce_reaper();
    if (ok) it_pass("T311"); else it_fail("T311", why);
}

/* ── T312: the ROOT CSpace capability carries a guard too (D-2 complete) ──
 *
 * Guards below the root landed in Stage 8-cap and live in the SLOT, because a
 * KCSlot is the capability.  The root is the one capability a thread does not
 * reach through a slot — it is a structural pointer — so its guard lives on
 * the thread, installed by SYS_TCB_CONFIGURE's arg3, which is seL4's
 * `cspace_root_data`: the same argument, in the same position of the same
 * operation, meaning the same thing.
 *
 * The property this test exists for is the one that would be lost by putting
 * the guard on the KCNode instead: the parent and the child here share the
 * SAME root CNode object, and address it DIFFERENTLY.  The child is configured
 * with a guard and must carry it; the parent has none and must not.  If the
 * guard lived on the object, one of those two would be wrong — and it is the
 * same property (a guard belongs to a capability, not to what it names) that
 * host G-7 asserts for the levels below.
 */
static uint8_t g_t312_stack[8192];
static volatile int g_t312_done;
static volatile int g_t312_plain_ok;    /* did the UNGUARDED address resolve? */
static volatile int g_t312_guarded_ok;  /* did the GUARDED address resolve?   */

static void t312_child(void) {
    /* Configured with a root guard: the plain address must NOT resolve and the
     * guarded one must. */
    g_t312_plain_ok   = (it_invoke2(T312_PROBE, INV_CAP_IDENTIFY, 0, 0) >= 0);
    g_t312_guarded_ok = (it_invoke2(T312_PROBE_G, INV_CAP_IDENTIFY, 0, 0) >= 0);
    g_t312_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) { }
}

void test_t312(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "root guard";

    /* The PARENT has no root guard: its plain address resolves and the guarded
     * one does not.  Establish that first, so the child's opposite answers
     * cannot be explained by anything but the guard. */
    if (ok && it_invoke2(T312_PROBE, INV_CAP_IDENTIFY, 0, 0) < 0) {
        ok = 0; why = "parent lost its own plain address";
    }
    if (ok && it_invoke2(T312_PROBE_G, INV_CAP_IDENTIFY, 0, 0) >= 0) {
        ok = 0; why = "parent resolved a guarded address it has no guard for";
    }

    /* A child on the SAME root CNode, configured WITH a guard. */
    long cs = it_cspace_self();
    if (ok && cs < 0) { ok = 0; why = "cspace self"; }
    long tcb = -1;
    if (ok) {
        tcb = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_TCB, 0);
        if (tcb < 0) { ok = 0; why = "retype tcb"; }
    }
    if (ok && it_invoke(tcb, INV_TCB_CONFIGURE, cs, IT_VS, (long)(((uint64_t)T312_GUARD_BITS << 32) |
                             (uint64_t)T312_GUARD)) != 0) {
        ok = 0; why = "configure with root guard";
    }
    if (ok) {
        uint64_t rsp = ((uint64_t)(uintptr_t)(g_t312_stack +
                        sizeof(g_t312_stack))) & ~0xFULL;
        g_t312_done = 0;
        if (it_invoke(tcb, INV_TCB_WRITE_REGS, (long)(uintptr_t)t312_child, (long)rsp, 0) != 0 ||
            it_invoke0(tcb, INV_TCB_RESUME) != 0) {
            ok = 0; why = "start child";
        }
    }
    if (ok) {
        IT_AWAIT(g_t312_done, 4000);
        if (!g_t312_done) { ok = 0; why = "child never ran"; }
    }

    /* Same CNode, opposite addressing — which is only possible because the
     * guard belongs to the capability rather than to the object. */
    if (ok && g_t312_plain_ok) {
        ok = 0; why = "guarded child resolved an unguarded address";
    }
    if (ok && !g_t312_guarded_ok) {
        ok = 0; why = "guarded child could not resolve its guarded address";
    }

    /* A guard that does not fit its width is refused, not truncated. */
    if (ok && tcb >= 0) {
        long t2 = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_TCB, 0);
        if (t2 >= 0 &&
            it_invoke(t2, INV_TCB_CONFIGURE, cs, IT_VS, (long)(((uint64_t)2 << 32) | (uint64_t)0x7)) !=
            (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "oversized root guard accepted";
        }
        if (t2 >= 0) it_slot_delete((uint32_t)t2);
    }

    if (tcb >= 0) it_slot_delete((uint32_t)tcb);
    it_quiesce_reaper();
    if (ok) it_pass("T312"); else it_fail("T312", why);
}

static uint8_t g_t313_srv_stack[8192];
static volatile long g_t313_ep, g_t313_reply, g_t313_srv_frame;
static volatile int  g_t313_srv_ready, g_t313_srv_err;
static volatile int  g_t313_srv_len, g_t313_srv_uptr_ok, g_t313_rounds;

static void t313_server(uint64_t self_tcb) {
    long self = (long)self_tcb;
    if (self <= 0) { g_t313_srv_err = 1; g_t313_srv_ready = 1; for (;;) { } }
    if (it_invoke2(self, INV_TCB_SET_IPC_BUFFER, (long)g_t313_srv_frame, (long)T313_SRV_VA) != 0) {
        g_t313_srv_err = 2; g_t313_srv_ready = 1; for (;;) { }
    }
    g_t313_srv_ready = 1;

    struct iris_msg m;
    iris_msg_zero(&m);
    if ((m.reply = (long)g_t313_reply, iris_msg_recv((long)g_t313_ep, &m)) != 0) {
        g_t313_srv_err = 3; for (;;) { }
    }
    for (;;) {
        volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)T313_SRV_VA;
        g_t313_srv_len     = (int)m.buf_len;
        g_t313_srv_uptr_ok = 1;   /* A-33: no address is named, see T313 */
        /* The service: invert every byte, in place, in a page the server owns. */
        for (uint32_t i = 0; i < m.buf_len && i < 4096u; i++)
            b[i] = (uint8_t)(b[i] ^ 0xFFu);
        g_t313_rounds++;
        m.reply = (long)g_t313_reply;
        if (iris_msg_reply_recv((long)g_t313_ep, &m) != 0)
            break;
    }
    for (;;) { }
}


void test_t313(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "ipc buffer frame";

    if (!it_setup_self_vspace()) { it_fail("T313", "vspace self"); return; }

    long self = it_own_tcb_derived();
    long cfr  = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096u);
    long sfr  = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096u);
    if (self < 0 || cfr < 0 || sfr < 0) { it_fail("T313", "objects"); return; }
    if (it_invoke(cfr, INV_FRAME_MAP, IT_VS, (long)T313_CLI_VA, (long)IT_MAP_W) != 0 ||
        it_invoke(sfr, INV_FRAME_MAP, IT_VS, (long)T313_SRV_VA, (long)IT_MAP_W) != 0) {
        it_fail("T313", "map"); return;
    }

    /* ── 1. registration is authority-checked ───────────────────────────*/
    if (ok && it_invoke2((long)IRIS_CPTR_TEST_UNTYPED, INV_TCB_SET_IPC_BUFFER, cfr, (long)T313_CLI_VA) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "non-TCB accepted";
    }
    /* A-30: the same syscall used to answer WRONG_TYPE for arg0 and
     * INVALID_ARG for arg1 — one call, two answers for one kind of mistake.
     * Both are WRONG_TYPE now. */
    if (ok && it_invoke2(self, INV_TCB_SET_IPC_BUFFER, (long)IRIS_CPTR_TEST_UNTYPED, (long)T313_CLI_VA) != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "non-frame accepted";
    }
    if (ok && it_invoke2(self, INV_TCB_SET_IPC_BUFFER, cfr, (long)(T313_CLI_VA + 8u)) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "unaligned address accepted";
    }
    if (ok && it_invoke2(self, INV_TCB_SET_IPC_BUFFER, cfr, (long)0xFFFF800000000000ULL) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "kernel address accepted";
    }
    /* Unregistering names no address — otherwise "give it back" and "move it"
     * would be the same call with a field nobody reads. */
    if (ok && it_invoke2(self, INV_TCB_SET_IPC_BUFFER, 0L, (long)T313_CLI_VA) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "unregister with an address accepted";
    }
    /* A frame cap without WRITE cannot become a buffer the kernel writes. */
    if (ok) {
        long ro = it_cs_reduce(cfr, RIGHT_READ);
        if (ro < 0) { ok = 0; why = "reduce"; }
        else if (it_invoke2(self, INV_TCB_SET_IPC_BUFFER, ro, (long)T313_CLI_VA)
                 != (long)IRIS_ERR_ACCESS_DENIED) {
            ok = 0; why = "read-only frame accepted";
        }
    }
    if (!ok) { it_fail("T313", why); return; }

    /* ── 2 and 3: a payload larger than the staging buffer, with no pointer ──*/
    g_t313_ep    = it_ep_create_slot();
    g_t313_reply = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                        IRIS_KOBJ_REPLY, 0);
    g_t313_srv_frame = sfr;
    g_t313_srv_ready = 0; g_t313_srv_err = 0;
    g_t313_srv_len = -1;  g_t313_srv_uptr_ok = 0; g_t313_rounds = 0;
    if (g_t313_ep < 0 || g_t313_reply < 0) { it_fail("T313", "ep/reply"); return; }

    struct it_utq_global gb0, gb1, gb2;
    /* This thread already holds a buffer — the suite gives every thread one —
     * so give it up first: registering over an existing one is a REPLACEMENT
     * and the gauge is a live count, not a tally.  Measuring the move means
     * measuring an actual arrival. */
    (void)it_invoke2(self, INV_TCB_SET_IPC_BUFFER, 0L, 0);
    if (!it_utq_g(&gb0)) { it_fail("T313", "query"); return; }

    if (it_invoke2(self, INV_TCB_SET_IPC_BUFFER, cfr, (long)T313_CLI_VA) != 0) {
        it_fail("T313", "register self"); return;
    }
    /* ── 5. the SERVICES really registered, and so did we ────────────────
     * This leg exists because the migration off the kernel's 256-byte staging
     * fails silently: a service whose registration is refused keeps working on
     * the old path, and the whole suite passes either way.  That is not a
     * hypothetical — it happened on the first service tried, and only a
     * deliberate check found it.  So the live count is asserted, and the
     * number has to be re-stated as services migrate, the same way the first
     * unassigned syscall number does.
     *
     * Migrated: init, console, sh, svcmgr, vfs.  Plus this thread. */
    if (ok && !it_utq_g(&gb1)) { ok = 0; why = "query"; }
    if (ok && gb1.ipc_buffers != gb0.ipc_buffers + 1u) {
        ok = 0; why = "registering did not move the gauge";
        it_fz_note("T313", gb0.ipc_buffers, gb1.ipc_buffers, 0u);
    }
    if (ok && gb0.ipc_buffers < 5u) {
        ok = 0; why = "fewer services registered a buffer than expected";
        it_fz_note("T313", gb0.ipc_buffers, 5u, 0u);
    }
    /* The pager is spawned per-test rather than at boot, so it is not in the
     * standing count; T201's manifest run is where it registers. */

    long srv = it_thread_create((uint64_t)(uintptr_t)t313_server,
                                ((uint64_t)(uintptr_t)(g_t313_srv_stack +
                                    sizeof(g_t313_srv_stack))) & ~0xFULL, IT_THREAD_ARG_SELF_TCB);
    if (srv < 0) { ok = 0; why = "server thread"; }
    if (ok) IT_AWAIT(g_t313_srv_ready, 2000);
    if (ok && !g_t313_srv_ready) { ok = 0; why = "server never registered"; }
    if (ok && g_t313_srv_err)    { ok = 0; why = "server registration failed"; }

    volatile uint8_t *cb = (volatile uint8_t *)(uintptr_t)T313_CLI_VA;
    if (ok) {
        for (uint32_t i = 0; i < T313_LEN; i++) cb[i] = (uint8_t)(i * 7u + 3u);

        struct iris_msg m;
        iris_msg_zero(&m);
        m.label    = 0x313;
        m.buf_len  = T313_LEN;
        if (iris_msg_call((long)g_t313_ep, &m) != 0) {
            ok = 0; why = "call failed";
        }
        if (ok && g_t313_srv_len != (int)T313_LEN) {
            ok = 0; why = "server saw the wrong length";
            it_fz_note("T313", (uint32_t)g_t313_srv_len, T313_LEN, 0u);
        }
        /*
         * A-33: neither end is TOLD where its buffer is, and that is the
         * point.  A message carries a LENGTH; the page it refers to is the one
         * the thread registered, because there is nowhere else a payload could
         * be.  This used to assert that the kernel handed back the registered
         * address in `buf_uptr` — a field that could only ever hold the one
         * value, and whose other values the kernel had to refuse.
         *
         * What replaces it is the assertion underneath: the bytes are in the
         * registered page, which is checked directly below.
         */
        if (ok && m.buf_len != T313_LEN) { ok = 0; why = "reply length lost"; }
        /* The whole oversized payload came back inverted, in the client's own
         * page.  256 bytes of kernel staging could not have carried it. */
        for (uint32_t i = 0; ok && i < T313_LEN; i++) {
            if (cb[i] != (uint8_t)(~(uint8_t)(i * 7u + 3u))) {
                ok = 0; why = "payload wrong";
                it_fz_note("T313", i, cb[i], (uint8_t)(~(uint8_t)(i * 7u + 3u)));
            }
        }
    }

    /* ── 4b. a payload has no address to get wrong ───────────────────────
     * This leg used to send with a payload pointer that named some other
     * buffer and require a refusal.  The silent version of that had cost a
     * boot's worth of corrupted console output and failed no test: the shared
     * console client marshalled into a buffer its caller passed, five services
     * passed their own static array, and the kernel sent whatever was at
     * offset 0 of their IPC buffer instead.  D-4 turned it into a refusal.
     *
     * A-33 removed the question.  A message carries a LENGTH; the bytes are in
     * the page the thread registered because there is nowhere else they could
     * be, and `buf_uptr` is deleted.  What is left to assert is that the
     * length alone still gets the bytes there and back, which is what the
     * round trip above does — so this leg asserts the WEAKER remaining thing:
     * a send of a payload works with nothing named at all.
     */
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label    = 0x316;
        m.buf_len  = 8u;
        if (iris_msg_call((long)g_t313_ep, &m) != 0) {
            ok = 0; why = "a payload with only a length was refused";
        }
    }

    /* ── 4. no buffer, no payload ────────────────────────────────────────
     * The closing half of D-4, and the assertion that replaced its opposite.
     *
     * This leg used to check that giving the buffer up fell back to the
     * kernel's 256 bytes of staging — the memory inside every TCB that the
     * user did not choose, did not pay for and could not name.  There is no
     * fallback now: the message registers travel in registers, and anything
     * longer needs somewhere to live that somebody owns.  That is seL4's
     * answer, and a test still expecting a clamp would be pinning the very
     * thing the row was opened to remove.
     */
    if (ok && it_invoke2(self, INV_TCB_SET_IPC_BUFFER, 0L, 0) != 0) {
        ok = 0; why = "unregister";
    }
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label   = 0x314;
        m.buf_len = 8u;
        if (iris_msg_call((long)g_t313_ep, &m)
            != (long)IRIS_ERR_INVALID_ARG) {
            ok = 0; why = "a payload with no buffer was accepted";
        }
        /* ...and a message with NO payload still goes: the registers are not
         * the buffer, and losing one must not cost the other. */
        iris_msg_zero(&m);
        m.label = 0x318;
        if (ok && iris_msg_call((long)g_t313_ep, &m) != 0) {
            ok = 0; why = "a register-only message was refused";
        }
        if (ok && g_t313_rounds != 3) { ok = 0; why = "server missed a round"; }
    }

    /* ── 6. the frame's CAPABILITY can go while the kernel still holds it ─
     * The registration takes its own reference, so deleting the slot that
     * named the frame must not destroy it under a thread that is about to
     * send through it.  Probed here because the first version of the suite's
     * per-thread buffers took its frames from a rotating slot pool and the
     * kernel panicked on an active-reference underflow within three tests. */
    if (ok) {
        long probe_fr = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096u);
        long probe_va = (long)(T313_CLI_VA + 0x10000ULL);
        if (probe_fr < 0) { ok = 0; why = "probe frame"; }
        else if (it_invoke(probe_fr, INV_FRAME_MAP, IT_VS, probe_va, (long)IT_MAP_W) != 0) {
            ok = 0; why = "probe map";
        } else if (it_invoke2(self, INV_TCB_SET_IPC_BUFFER, probe_fr, probe_va) != 0) {
            ok = 0; why = "probe register";
        } else {
            /* The capability goes; the buffer must not. */
            it_slot_delete((uint32_t)probe_fr);
            struct iris_msg m;
            iris_msg_zero(&m);
            m.label   = 0x317;
            m.buf_len = 8u;
            if (iris_msg_call((long)g_t313_ep, &m) != 0) {
                ok = 0; why = "a buffer died with its capability";
            }
            (void)it_invoke2(self, INV_TCB_SET_IPC_BUFFER, 0L, 0);
            (void)it_invoke2(probe_fr, INV_FRAME_UNMAP, IT_VS, probe_va);
        }
        /* Put the real buffer back for the legs below. */
        if (ok && it_invoke2(self, INV_TCB_SET_IPC_BUFFER, cfr, (long)T313_CLI_VA) != 0) {
            ok = 0; why = "re-register";
        }
    }

    /* ...and unregistering gives it back, so the gauge is a LIVE count and not
     * a tally of registrations that only ever grows.
     *
     * Measured immediately AROUND the act rather than across the whole test:
     * the first version compared against a sample taken at the top, which made
     * it an assertion about how many other threads happened to be alive — and
     * it broke the moment the suite started giving every thread a buffer,
     * which is a fact about the suite and not about the gauge. */
    if (ok && !it_utq_g(&gb2)) { ok = 0; why = "query"; }
    (void)it_invoke2(self, INV_TCB_SET_IPC_BUFFER, 0L, 0);
    if (ok) {
        struct it_utq_global gb3;
        if (!it_utq_g(&gb3) || gb3.ipc_buffers + 1u != gb2.ipc_buffers) {
            ok = 0; why = "unregistering did not give the gauge back";
        }
    }

    (void)it_invoke0(srv, INV_TCB_EXIT);
    it_quiesce_reaper();
    (void)it_invoke2(cfr, INV_FRAME_UNMAP, IT_VS, (long)T313_CLI_VA);
    (void)it_invoke2(sfr, INV_FRAME_UNMAP, IT_VS, (long)T313_SRV_VA);
    /* Give this thread its own buffer back: every test after this one sends
     * through it, and a thread without one can no longer send a payload. */
    {
        long tcb_self = it_own_tcb_derived();
        if (tcb_self >= 0) {
            long va = it_thread_ipc_buffer(tcb_self);
            if (va > 0) g_ep_io_buf = (uint8_t *)(uintptr_t)va;
            /* The registration is on the THREAD; this capability was only how
             * to name it, and leaving it in a recycling pool is a capability
             * waiting to be evicted under somebody else. */
            it_slot_delete((uint32_t)tcb_self);
        }
    }

    if (ok) it_pass("T313"); else it_fail("T313", why);
}

void test_t314(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "irq user context";

    struct it_utq_global g0, g1;
    if (!it_utq_g(&g0)) { it_fail("T314", "query"); return; }

    /* ── 2. register integrity across real preemption ───────────────────
     * Values distinct in every byte, so a restore that shifted a register by
     * one slot lands on something that cannot be mistaken for the right
     * answer.  rbp is left alone deliberately: the compiler may be using it
     * as this frame's pointer, and a test that breaks the frame it reports
     * from reports nothing. */
    uint64_t o12 = 0, o13 = 0, o14 = 0, o15 = 0, obx = 0;
    uint64_t n = T314_SPINS;
    __asm__ __volatile__(
        "movq $0x3333333300000003, %%r12\n\t"
        "movq $0x4444444400000004, %%r13\n\t"
        "movq $0x5555555500000005, %%r14\n\t"
        "movq $0x6666666600000006, %%r15\n\t"
        "movq $0x1111111100000001, %%rbx\n\t"
        "1:\n\t"
        "decq %[n]\n\t"
        "jnz 1b\n\t"
        "movq %%r12, %[a]\n\t"
        "movq %%r13, %[b]\n\t"
        "movq %%r14, %[c]\n\t"
        "movq %%r15, %[d]\n\t"
        "movq %%rbx, %[e]\n\t"
        : [a] "=m"(o12), [b] "=m"(o13), [c] "=m"(o14), [d] "=m"(o15),
          [e] "=m"(obx), [n] "+r"(n)
        :
        : "r12", "r13", "r14", "r15", "rbx", "cc", "memory");

    if (ok && obx != 0x1111111100000001ULL) { ok = 0; why = "rbx corrupted"; }
    if (ok && o12 != 0x3333333300000003ULL) { ok = 0; why = "r12 corrupted"; }
    if (ok && o13 != 0x4444444400000004ULL) { ok = 0; why = "r13 corrupted"; }
    if (ok && o14 != 0x5555555500000005ULL) { ok = 0; why = "r14 corrupted"; }
    if (ok && o15 != 0x6666666600000006ULL) { ok = 0; why = "r15 corrupted"; }

    /* ── 1. the path ran while all that was happening ───────────────────*/
    if (ok && !it_utq_g(&g1)) { ok = 0; why = "query"; }
    if (ok && g1.irq_ctx_saves <= g0.irq_ctx_saves) {
        ok = 0; why = "no ring-3 entry saved a user context";
        it_fz_note("T314", g0.irq_ctx_saves, g1.irq_ctx_saves, 0u);
    }

    if (ok) it_pass("T314"); else it_fail("T314", why);
}

/* ── T315: the replenishment depth is the SC's, and it is paid for ───────
 *
 * The last thing MCS scheduling was still doing seL4's way in shape but not in
 * substance: `refill_max` was a kernel constant of 8, so every scheduling
 * context carried storage for eight pending replenishments whether it needed
 * two or sixty.
 *
 * How many a thread needs is a fact about the THREAD.  A passive server woken
 * once per request produces one replenishment per request and needs a deep
 * queue to keep them distinct; a periodic task needs two.  When the queue
 * fills, entries merge and part of a replenishment arrives late — never early,
 * so the guarantee survives, but the thread runs less than it paid for.  seL4
 * makes the depth a parameter and sizes the object to match; IRIS does it at
 * RETYPE, where every other object's size is already chosen.
 *
 * Three things are asserted, and the second is the one that matters:
 *
 *   1. the depth is CHECKED — below the floor of two and above the ceiling are
 *      refused, because one replenishment can be in flight while the current
 *      run accumulates another and a queue of one cannot hold both;
 *   2. a deeper SC COSTS MORE out of the Untyped that made it.  That is the
 *      whole point: the memory is charged to whoever asked for the depth,
 *      instead of every SC paying for the worst case out of the kernel;
 *   3. an SC retyped with an explicit depth still schedules — it is a working
 *      scheduling context, not just a differently sized allocation.
 *
 * Invariants: M3, O2. */
void test_t315(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "per-SC refill depth";

    /* A dedicated sub-untyped, so the measurement is not perturbed by whatever
     * else the suite fabricates from its pool. */
    long sub = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_UNTYPED, 64u * 1024u);
    if (sub < 0) { it_fail("T315", "sub-untyped"); return; }

    /* ── 1. the depth is checked ────────────────────────────────────────*/
    if (ok && it_retype_slot_alloc(sub, IRIS_KOBJ_SCHED_CONTEXT, 1)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "a depth of one was accepted";
    }
    if (ok && it_retype_slot_alloc(sub, IRIS_KOBJ_SCHED_CONTEXT, 65)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "a depth above the ceiling was accepted";
    }

    /* ── 2. a deeper context costs more ─────────────────────────────────*/
    uint64_t a0 = 0, a1 = 0, a2 = 0;
    long shallow = -1, deep = -1;
    if (ok && it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&a0) != 0) {
        ok = 0; why = "info";
    }
    if (ok) {
        shallow = it_retype_slot_alloc(sub, IRIS_KOBJ_SCHED_CONTEXT, 2);
        if (shallow < 0) { ok = 0; why = "shallow retype"; }
    }
    if (ok && it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&a1) != 0) {
        ok = 0; why = "info2";
    }
    if (ok) {
        deep = it_retype_slot_alloc(sub, IRIS_KOBJ_SCHED_CONTEXT, 64);
        if (deep < 0) { ok = 0; why = "deep retype"; }
    }
    if (ok && it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&a2) != 0) {
        ok = 0; why = "info3";
    }
    if (ok) {
        uint64_t cost_shallow = a0 - a1;
        uint64_t cost_deep    = a1 - a2;
        /* 62 extra slots of 16 bytes each is 992 bytes; the carve rounds, so
         * the assertion is on the ORDER rather than the exact figure. */
        if (cost_deep <= cost_shallow) {
            ok = 0; why = "a deeper context cost no more";
            it_fz_note("T315", (uint32_t)cost_shallow, (uint32_t)cost_deep, 0u);
        }
        if (ok && cost_deep - cost_shallow < 900u) {
            ok = 0; why = "the extra depth was not actually charged";
            it_fz_note("T315", (uint32_t)cost_shallow, (uint32_t)cost_deep, 0u);
        }
    }

    /* ── 3. and it is a working scheduling context ──────────────────────*/
    if (ok && it_invoke(deep, INV_SC_CONFIGURE, 40, 200, (long)IRIS_CPTR_SCHED_CONTROL) != 0) {
        ok = 0; why = "configure";
    }
    if (ok && it_invoke(shallow, INV_SC_CONFIGURE, 40, 200, (long)IRIS_CPTR_SCHED_CONTROL) != 0) {
        ok = 0; why = "configure shallow";
    }

    if (ok) it_pass("T315"); else it_fail("T315", why);
}

/* ── T316: MMIO is handed over as a capability (ledger D-9) ──────────────
 *
 * seL4's BootInfo lists DEVICE Untypeds alongside RAM ones: that is how a
 * driver is given an MMIO region and retypes frames from it.  IRIS described
 * the shape and never used it — `iris_bootinfo_untyped.is_device` has been in
 * the ABI since v1 and boot always wrote 0 — so no device Untyped could exist,
 * the invariants about them (U11/U12) described an object the system could not
 * construct, and IRIS reached device memory the other way: the kernel
 * fabricating a KVMO over the framebuffer, which is the object family D-5
 * still records as un-retyped.  The two gaps were one gap seen from opposite
 * ends.
 *
 * What makes a device Untyped different, and the reason it took a syscall to
 * make it usable: a device region cannot hold the HEADERS of objects carved
 * from it.  MMIO is not storage — a `struct KFrame` written into a
 * framebuffer is pixels, and read back it is whatever the display controller
 * left there.  A RAM Untyped carves its objects' headers out of its own top
 * end; a device one must be told which RAM pays.  The alternative the kernel
 * used to take was its own slab, which is charter M3's exact prohibition.
 *
 * Four things asserted:
 *
 *   1. the capability EXISTS and reports itself as device memory;
 *   2. retyping from it UNPAIRED is refused — the kernel does not guess whose
 *      memory to spend, and the old behaviour (spend its own) is gone;
 *   3. once paired, a frame retypes, and the HEADER is charged to the RAM
 *      budget while the PAGE comes out of the device region.  Both halves are
 *      measured, because charging the wrong one to the wrong region is the
 *      mistake that would otherwise pass every other test;
 *   4. the pairing is SET ONCE.  A pairing that could move would let a holder
 *      point at a second budget and reset the first, reclaiming a region while
 *      the headers describing live device frames were still in it.
 *
 * Invariants: M3, O2, U11, U12. */
void test_t316(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "device untyped";

    const long dev = (long)IRIS_CPTR_DEVICE_UNTYPED;

    /* ── 1. it exists, and it says it is device memory ───────────────────*/
    struct it_utq_one q;
    if (!it_utq_1(dev, &q)) { it_fail("T316", "no device untyped"); return; }
    if (ok && !q.is_device) { ok = 0; why = "not reported as device memory"; }
    if (ok && q.total_bytes == 0u) { ok = 0; why = "empty device region"; }

    /* ── 2. fb REALLY retyped the framebuffer out of it ──────────────────
     * The end-to-end proof that D-5's last memory object is gone.  fb used to
     * receive a KVMO the kernel fabricated inside SYS_FRAMEBUFFER_VMO; it now
     * retypes one frame covering the whole region out of this Untyped, so the
     * region reads as consumed and carries a child.  A migration that had
     * quietly fallen back would leave it untouched, and every other test would
     * still pass — the screen is painted either way. */
    if (ok && q.child_count == 0u) {
        ok = 0; why = "nobody retyped the framebuffer";
    }
    if (ok && q.used_bytes < 4096u) {
        ok = 0; why = "the framebuffer region was never carved";
        it_fz_note("T316", (uint32_t)q.used_bytes, (uint32_t)q.total_bytes, 0u);
    }

    /* ── 3. the pairing is SET ONCE ──────────────────────────────────────
     * fb paired it at boot.  The budget is a property of the OBJECT — one
     * physical region, one set of headers — so the first holder to pair
     * decides for every holder, and a second attempt is refused rather than
     * silently moving the headers of live frames to another region. */
    long ram = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_UNTYPED, 8u * 1024u);
    if (ok && ram < 0) { ok = 0; why = "budget"; }
    if (ok && it_invoke1(dev, INV_UNTYPED_SET_DEVICE_BUDGET, ram)
              != (long)IRIS_ERR_ALREADY_EXISTS) {
        ok = 0; why = "the pairing moved";
    }
    /* A DEVICE region cannot pay for headers, not even its own. */
    if (ok && it_invoke1(dev, INV_UNTYPED_SET_DEVICE_BUDGET, dev)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "a device region was accepted as a header budget";
    }
    /* Nor is a RAM Untyped something to pair: it carves its own headers. */
    if (ok && it_invoke1(ram, INV_UNTYPED_SET_DEVICE_BUDGET, ram)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "a RAM untyped accepted a header budget";
    }

    it_quiesce_reaper();
    if (ok) it_pass("T316"); else it_fail("T316", why);
}

void test_t317(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "multi-page frame";

    if (!it_setup_self_vspace()) { it_fail("T317", "vspace self"); return; }

    long sub = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                    IRIS_KOBJ_UNTYPED, 256u * 1024u);
    if (sub < 0) { it_fail("T317", "sub-untyped"); return; }

    /* ── 1. it retypes, and it costs what it is ─────────────────────────*/
    uint64_t before = 0, after = 0;
    if (it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&before) != 0) {
        it_fail("T317", "info"); return;
    }
    long fr = it_frame_create_slot(sub, T317_SIZE);
    if (fr < 0) { it_fail("T317", "retype"); return; }
    if (it_invoke2(sub, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
        it_fail("T317", "info2"); return;
    }
    if (ok && (before - after) < T317_SIZE) {
        ok = 0; why = "a multi-page frame was not charged for its size";
    }

    /* ── 2. every page of it is there after ONE map ─────────────────────*/
    if (ok && it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)T317_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "map";
    }
    if (ok) {
        for (uint32_t p = 0; ok && p < T317_PAGES; p++) {
            volatile uint64_t *w =
                (volatile uint64_t *)(uintptr_t)(T317_VA + p * 4096u);
            if (w[0] != 0u) { ok = 0; why = "page not zero"; break; }
            w[0] = 0xA5000000ULL + p;      /* distinct per page */
        }
        /* Read them all back: if the map had aliased every page onto the
         * frame's first, they would all hold the last value written. */
        for (uint32_t p = 0; ok && p < T317_PAGES; p++) {
            volatile uint64_t *w =
                (volatile uint64_t *)(uintptr_t)(T317_VA + p * 4096u);
            if (w[0] != 0xA5000000ULL + p) {
                ok = 0; why = "pages alias each other";
                it_fz_note("T317", p, (uint32_t)w[0], 0u);
            }
        }
    }

    /* ── 4. an overlapping map is refused and changes nothing ───────────
     * Asserted while the frame is still mapped: a second map one page into
     * the window overlaps on three of its four pages.  If the occupancy check
     * ran per page as it installed, the first page would be left behind. */
    if (ok && it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)(T317_VA + 4096u), (long)IT_MAP_W) == 0) {
        ok = 0; why = "an overlapping map was allowed";
    }

    /* ── 3. one unmap removes all of it ─────────────────────────────────*/
    if (ok && it_invoke2(fr, INV_FRAME_UNMAP, IT_VS, (long)T317_VA) != 0) {
        ok = 0; why = "unmap";
    }
    /* Re-mapping the same window proves every PTE went: the map refuses a VA
     * that is already occupied, so a leftover anywhere in the range fails. */
    if (ok && it_invoke(fr, INV_FRAME_MAP, IT_VS, (long)T317_VA, (long)IT_MAP_W) != 0) {
        ok = 0; why = "unmap left pages behind";
    }
    if (ok && it_invoke2(fr, INV_FRAME_UNMAP, IT_VS, (long)T317_VA) != 0) {
        ok = 0; why = "unmap2";
    }

    it_slot_delete((uint32_t)fr);
    it_quiesce_reaper();
    if (ok) it_pass("T317"); else it_fail("T317", why);
}

static uint8_t g_t318_stacks[T318_THREADS][2048];
static volatile uint32_t g_t318_ran;

static void t318_body(void) {
    __atomic_fetch_add(&g_t318_ran, 1u, __ATOMIC_RELAXED);
    it_sys1(SYS_EXIT, 0);
    for (;;) { }
}

void test_t318(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "kernel memory per thread";

    struct it_utq_global g0, g1;
    if (!it_utq_g(&g0)) { it_fail("T318", "query"); return; }
    if (g0.kernel_free_pages == 0u) { it_fail("T318", "no pmm gauge"); return; }

    /* The kernel's HEAP is sealed too, and this is the only place ring 3 can
     * see it.  seL4 has none at all; IRIS's is a boot arena, and after boot
     * `kslab_alloc` panics — so "the kernel does not allocate after boot" is a
     * fact the build enforces rather than one a reader has to check.  Every
     * test after this one runs inside that claim. */
    if (!g0.kernel_heap_sealed) { it_fail("T318", "kernel heap not sealed"); return; }

    g_t318_ran = 0;
    long tids[T318_THREADS];
    uint32_t made = 0;
    for (uint32_t i = 0; i < T318_THREADS; i++) {
        tids[i] = it_thread_create((uint64_t)(uintptr_t)t318_body,
                                   ((uint64_t)(uintptr_t)(g_t318_stacks[i] +
                                       sizeof(g_t318_stacks[i]))) & ~0xFULL, 0);
        if (tids[i] < 0) break;
        made++;
    }
    if (made < 4u) { ok = 0; why = "could not make threads"; }

    /* Let them run, so the measurement covers a thread that has ENTERED the
     * kernel — a stack allocated lazily on first entry would show up here and
     * not in a count taken before any of them ran. */
    for (int i = 0; ok && i < 2000 && g_t318_ran < made; i++) (void)it_sys0(SYS_YIELD);

    if (ok && !it_utq_g(&g1)) { ok = 0; why = "query2"; }
    if (ok) {
        uint32_t spent = (g0.kernel_free_pages > g1.kernel_free_pages)
                       ? (g0.kernel_free_pages - g1.kernel_free_pages) : 0u;
        /* Two pages per thread is exactly what a per-thread kernel stack cost.
         * Anything at or above that is the old behaviour back. */
        if (spent >= made * 2u) {
            ok = 0; why = "the kernel paid for the threads' stacks";
            it_fz_note("T318", spent, made, 0u);
        }
    }

    for (uint32_t i = 0; i < made; i++) (void)it_invoke0(tids[i], INV_TCB_EXIT);
    it_quiesce_reaper();
    if (ok) it_pass("T318"); else it_fail("T318", why);
}

/* ── T319: no test destroys a capability another test needs ──────────────────
 * The suite fabricates from a rotating pool of leaves in a second-level CNode,
 * and a two-level CPtr is (leaf << 8) | root_slot.  Six call sites undid that
 * arithmetic before releasing a pool capability — `cptr >> 8` — and handed the
 * LEAF index to a delete that reads a bare number as a ROOT slot.  Leaves run
 * 4..199; the root slots the suite depends on live at 1..15, 55, 66, 80, 99.
 * So every run silently deleted a handful of root capabilities, and the run
 * survived only because the counter happened to miss the fatal ones.
 *
 * It stopped missing them the moment the pager fixtures became real frames:
 * the extra allocations shifted the counter, a delete landed on slot 80, and
 * sixty tests failed at once resolving a CNode that had been fine an hour
 * earlier — with 98 MiB free, so it never looked like memory.
 *
 * The arithmetic is gone.  What is left is this: it_slot_delete refuses a
 * root-level delete of a load-bearing slot and counts it, and the count must
 * be zero.  A gauge rather than a comment, because the comment above
 * it_slot_delete already warned about exactly this and six call sites did it
 * anyway.  Invariant: a capability the suite holds for its whole run is not
 * something any single test may release. */
/* ── T320: type is checked before rights ─────────────────────────────────────
 * A capability that is BOTH the wrong type AND missing the rights the
 * operation needs must be refused as WRONG_TYPE.  Not ACCESS_DENIED: rights
 * are a property OF a type, so "you lack RIGHT_READ on that frame" is a
 * statement about a frame, and the thing named is not one.  seL4 decodes the
 * capability type first and answers seL4_InvalidCapability; a resolver that
 * checked rights first knew exactly what the caller had named and reported
 * something else.
 *
 * Found closing D-5, and found the way these are always found: a "wrong type"
 * assertion in T079 was passing on ACCESS_DENIED, because its fixture happened
 * to lack a right the new call needed.  It was green and it was not testing
 * what it said.  Every probe here is deliberately double-wrong, so it can only
 * pass for the stated reason.  Invariants: M2, M9. */
void test_t320(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok && it_setup_self_vspace();
    const char *why = "type before rights";

    /* IRIS_CPTR_TEST_FIX_B is a svcmgr ENDPOINT carrying RIGHT_TRANSFER only:
     * the wrong type everywhere below, and short of the right each call asks
     * for as well. */
    const long wrong = (long)IRIS_CPTR_TEST_FIX_B;

    /* Frame resolver — SYS_FRAME_MAP wants RIGHT_READ on a KFrame. */
    if (ok && it_invoke(wrong, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "frame slot"; }
    /* ...and SYS_FRAME_SIZE, which wants RIGHT_READ too. */
    if (ok && it_invoke0(wrong, INV_FRAME_SIZE)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "frame size"; }

    /* VSpace resolver — a map target wants RIGHT_WRITE on a KVSpace. */
    long fr = ok ? it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096) : -1;
    handle_id_t fr_h = (fr >= 0) ? (handle_id_t)fr : HANDLE_INVALID;
    if (ok && fr < 0) { ok = 0; why = "frame"; }
    if (ok && it_invoke(fr, INV_FRAME_MAP, wrong, (long)T26_SELF_VA, 0L)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "vspace slot"; }

    /* Untyped resolver — a budget wants RIGHT_WRITE on a KUntyped. */
    if (ok && it_retype2_at(wrong, IRIS_KOBJ_FRAME, IT_SCRATCH_0, 1u, 4096)
              != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "budget slot"; }
    it_slot_delete(IT_SCRATCH_0);

    /* Endpoint resolver — EP_SEND wants RIGHT_WRITE on a KEndpoint.  A
     * READ-only derivation of a NOTIFICATION is double-wrong the same way. */
    long n = ok ? it_notify_create() : -1;
    handle_id_t n_h = (n >= 0) ? (handle_id_t)n : HANDLE_INVALID;
    long n_ro = (n >= 0) ? it_cs_reduce(n, RIGHT_READ) : -1;
    handle_id_t n_ro_h = (n_ro >= 0) ? (handle_id_t)n_ro : HANDLE_INVALID;
    if (ok && (n < 0 || n_ro < 0)) { ok = 0; why = "notif"; }
    if (ok) {
        struct iris_msg m;
        iris_msg_zero(&m);
        m.label = 0x320;
        if (iris_msg_nb_send(n_ro, &m) != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "endpoint slot";
        }
    }

    /* And the ordering does not swallow a real rights failure: the RIGHT type
     * without the right is still ACCESS_DENIED. */
    long fr_ro = ok ? it_cs_reduce(fr, RIGHT_READ) : -1;
    handle_id_t fr_ro_h = (fr_ro >= 0) ? (handle_id_t)fr_ro : HANDLE_INVALID;
    if (ok && fr_ro < 0) { ok = 0; why = "frame ro"; }
    if (ok && it_invoke(fr_ro, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 1L)
              != (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights no longer checked"; }

    it_close(&fr_ro_h); it_close(&n_ro_h); it_close(&n_h); it_close(&fr_h);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T320"); else it_fail("T320", why);
}

/* ── T321: a CSpace cycle is reclaimed by revoking its Untyped (D-7) ─────────
 * The ledger records that a CNode reachable only through another CNode keeps
 * its own reference count above zero and is never collected — refcounting is a
 * strictly weaker collector than a derivation tree, and this is the case that
 * shows it.  Nothing in the tree builds one, so it had never been exercised.
 *
 * This builds one and measures what actually happens, because the severity of
 * that row depends on a fact nobody had checked: whether the memory comes
 * BACK.  seL4's guarantee is not "unreachable objects are collected" — seL4
 * has no idle collector either — it is "revoking the Untyped reclaims
 * everything derived from it", and that holds through cycles because a
 * capability's MDB parent is where it was DERIVED, not where it is STORED.
 *
 * Three assertions, in the order that makes the answer unambiguous:
 *   1. with the cycle live and no external capability left, the budget is BUSY
 *      — nothing collected it, which is the refcount statement;
 *   2. revoking the Untyped's subtree destroys both CNodes anyway;
 *   3. the region resets, so the memory is genuinely back.
 * Invariants: O2, O6, M1. */
void test_t321(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "cspace cycle";

    long pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                     IRIS_KOBJ_UNTYPED, 64u * 1024u);
    if (pool < 0) { it_fail("T321", "pool"); return; }

    uint64_t before = 0, after = 0;
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&before) != 0) {
        ok = 0; why = "info"; }

    /* Two CNodes out of that budget, and a capability to each in the suite. */
    long ca = ok ? it_retype_slot_alloc(pool, IRIS_KOBJ_CNODE, 16) : -1;
    long cb = ok ? it_retype_slot_alloc(pool, IRIS_KOBJ_CNODE, 16) : -1;
    if (ok && (ca < 0 || cb < 0)) { ok = 0; why = "cnodes"; }

    /* The cycle: A holds a capability to B, B holds one to A. */
    if (ok && it_invoke2(cb, INV_CSPACE_MINT, IT_MINT_INTO(ca, 1), (long)(RIGHT_READ | RIGHT_WRITE)) != 0) { ok = 0; why = "A names B"; }
    if (ok && it_invoke2(ca, INV_CSPACE_MINT, IT_MINT_INTO(cb, 1), (long)(RIGHT_READ | RIGHT_WRITE)) != 0) { ok = 0; why = "B names A"; }

    struct it_utq_mdb m0, m1;
    if (ok && !it_utq_mdb(&m0)) { ok = 0; why = "mdb0"; }

    /* Let go of both.  Nothing outside the cycle names either CNode now. */
    if (ca >= 0) it_slot_delete((uint32_t)ca);
    if (cb >= 0) it_slot_delete((uint32_t)cb);
    it_quiesce_reaper();
    if (ok && !it_utq_mdb(&m1)) { ok = 0; why = "mdb1"; }
    /* Deleting a capability that has MDB children hands them to its PARENT,
     * not to nobody.  An orphan is a root, and a root is unreachable by every
     * revoke there is — which is how a subtree stops being reclaimable. */
    if (ok && m1.mdb_reparents - m0.mdb_reparents != 2u) {
        it_fz_note("T321", m1.mdb_reparents - m0.mdb_reparents, 2u, 0u);
        ok = 0; why = "children were not reparented";
    }
    if (ok && m1.mdb_orphan_promotions != m0.mdb_orphan_promotions) {
        ok = 0; why = "children were orphaned";
    }

    /* 1. Nothing collected them: the budget still has live children. */
    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "an unreachable cycle was collected"; }
    if (ok && it_invoke2(pool, INV_UNTYPED_INFO, 0, (long)(uintptr_t)&after) != 0) {
        ok = 0; why = "info2"; }
    if (ok && before <= after) { ok = 0; why = "the cycle cost nothing"; }

    /* 2. Revoking the Untyped's subtree reaches them regardless, because the
     *    MDB parent of each CNode capability is the slot holding `pool` — not
     *    the slot inside the other CNode. */
    long revoked = ok ? it_invoke0(pool, INV_CSPACE_REVOKE) : -1;
    /* One is enough, and the count is honest at one: destroying the capability
     * to A destroys A, whose close empties A's slots — and the capability to B
     * lived in one of them.  A revoke reports what IT destroyed, not what died
     * because of it.  Assertion 3 is what says both are gone. */
    if (ok && revoked < 1) {
        it_fz_note("T321", (uint32_t)(revoked & 0xFFFFu), 1u, 0u);
        ok = 0; why = "revoke did not reach the cycle";
    }
    it_quiesce_reaper();

    /* 3. ...and the memory is genuinely back. */
    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) != 0) { ok = 0; why = "region not reclaimed"; }

    it_slot_delete((uint32_t)pool);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T321"); else it_fail("T321", why);
}

void test_t322(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "capability lifetime";

    static const struct t322_case cases[] = {
        { IRIS_KOBJ_NOTIFICATION,  0,    "notification" },
        { IRIS_KOBJ_ENDPOINT,      0,    "endpoint"     },
        { IRIS_KOBJ_CNODE,         16,   "cnode"        },
        { IRIS_KOBJ_SCHED_CONTEXT, 4,    "sched ctx"    },
        { IRIS_KOBJ_UNTYPED,       8192, "untyped"      },
        { IRIS_KOBJ_REPLY,         0,    "reply"        },
        { IRIS_KOBJ_FRAME,         4096, "frame"        },
        { IRIS_KOBJ_PAGE_TABLE,    4096, "page table"   },
        { IRIS_KOBJ_VSPACE,        4096, "vspace"       },
        { IRIS_KOBJ_TCB,           0,    "tcb"          },
    };

    for (uint32_t i = 0; ok && i < (uint32_t)(sizeof(cases)/sizeof(cases[0])); i++) {
        long pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                         IRIS_KOBJ_UNTYPED, 128u * 1024u);
        if (pool < 0) { ok = 0; why = "pool"; break; }

        long a = it_retype_slot_alloc(pool, cases[i].type, cases[i].arg);
        if (a < 0) { ok = 0; why = cases[i].name; it_slot_delete((uint32_t)pool); break; }

        /* A second capability to the SAME object. */
        long c = it_cs_reduce(a, RIGHT_READ);
        if (c < 0) { ok = 0; why = cases[i].name; }

        /* The first capability goes; the object must not. */
        if (ok) {
            it_slot_delete((uint32_t)a);
            it_quiesce_reaper();
            if (it_invoke0(pool, INV_UNTYPED_RESET) != (long)IRIS_ERR_BUSY) {
                ok = 0; why = cases[i].name;
                it_fz_note("T322-early", cases[i].type, i, 0u);
            }
        }
        /* ...and when the last one goes, so does the object. */
        if (ok) {
            it_slot_delete((uint32_t)c);
            it_quiesce_reaper();
            if (it_invoke0(pool, INV_UNTYPED_RESET) != 0) {
                ok = 0; why = cases[i].name;
                it_fz_note("T322-late", cases[i].type, i, 0u);
            }
        }
        /* NOT deleted again here.  Both slots were released above, and a
         * released leaf of the shared rotating pool can be taken by another
         * thread the moment it_quiesce_reaper yields — so a second delete of
         * "my" slot destroys somebody else's capability.  Deleting twice is
         * only harmless when nobody else allocates, and something always does. */
        it_slot_delete((uint32_t)pool);
        it_quiesce_reaper();
    }

    struct it_snap a2 = it_snap_take();
    if (ok && !it_snap_baseline(&b, &a2, &why)) ok = 0;
    if (ok) it_pass("T322"); else it_fail("T322", why);
}
static uint32_t t323_rnd(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

void test_t323(void) {
    uint32_t rng = T323_SEED;
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "counter vs graph";
    uint32_t round = 0, step = 0;

    static const uint32_t types[] = {
        IRIS_KOBJ_NOTIFICATION, IRIS_KOBJ_ENDPOINT, IRIS_KOBJ_CNODE,
        IRIS_KOBJ_UNTYPED, IRIS_KOBJ_FRAME, IRIS_KOBJ_SCHED_CONTEXT,
    };
    static const long args[] = { 0, 0, 16, 8192, 4096, 4 };

    for (round = 0; ok && round < T323_ROUNDS; round++) {
        uint32_t ti = t323_rnd(&rng) % (uint32_t)(sizeof(types)/sizeof(types[0]));
        uint32_t n  = 2u + (t323_rnd(&rng) % (T323_MAXCAP - 1u));

        long pool = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED,
                                         IRIS_KOBJ_UNTYPED, 128u * 1024u);
        if (pool < 0) { ok = 0; why = "pool"; break; }

        long caps[T323_MAXCAP];
        uint32_t made = 0;
        caps[0] = it_retype_slot_alloc(pool, types[ti], args[ti]);
        if (caps[0] < 0) { ok = 0; why = "retype"; it_slot_delete((uint32_t)pool); break; }
        made = 1;
        /* Each copy derives from a randomly chosen EARLIER one, so the shape is
         * a tree rather than a fan — which is where a counter and a tree can
         * differ, and a fan is where they cannot. */
        while (made < n) {
            long src = caps[t323_rnd(&rng) % made];
            long c = it_cs_reduce(src, RIGHT_READ | RIGHT_DUPLICATE);
            if (c < 0) break;
            caps[made++] = c;
        }
        if (made < 2u) { ok = 0; why = "derive"; }

        /* Shuffle the deletion order: which capability goes last must not
         * matter, and if it does, the seed says which one it was. */
        for (uint32_t i = made; ok && i > 1u; i--) {
            uint32_t j = t323_rnd(&rng) % i;
            long t = caps[i - 1u]; caps[i - 1u] = caps[j]; caps[j] = t;
        }

        for (step = 0; ok && step < made; step++) {
            it_slot_delete((uint32_t)caps[step]);
            it_quiesce_reaper();
            long r = it_invoke0(pool, INV_UNTYPED_RESET);
            int last = (step + 1u == made);
            if (!last && r != (long)IRIS_ERR_BUSY) {
                ok = 0; why = "destroyed while a capability still named it";
            } else if (last && r != 0) {
                ok = 0; why = "outlived its last capability";
            }
        }

        /* The step loop released every capability; see T322 on why they are
         * not released again. */
        it_slot_delete((uint32_t)pool);
        it_quiesce_reaper();
    }

    /*
     * No global object-count baseline here, deliberately.  This test's subject
     * is the LIFETIME RULE, and its step assertions check it exactly: the
     * region is BUSY while a capability remains and resets when the last one
     * goes, after every single delete.  A baseline would additionally measure
     * whether the shared rotating pool is clean — which it is not, because
     * capabilities outlive the tests that made them and the allocator recycles
     * their leaves.  That is a real defect, it is T324's subject, and folding
     * it in here would make one failure say two things and neither clearly.
     */
    if (ok) it_pass("T323");
    else { it_fz_note("T323", T323_SEED, round, step); it_fail("T323", why); }
}
static volatile long     g_t325_notif;
static volatile uint32_t g_t325_woke;
static volatile uint32_t g_t325_blocked;
static uint8_t           g_t325_stacks[T325_THREADS][4096];

static void t325_body(void) {
    uint64_t bits = 0;
    __atomic_fetch_add(&g_t325_blocked, 1u, __ATOMIC_RELAXED);
    if (it_invoke1((long)g_t325_notif, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) == 0)
        __atomic_fetch_add(&g_t325_woke, 1u, __ATOMIC_RELAXED);
    for (;;) it_sys0(SYS_YIELD);
}

void test_t325(void) {
    it_quiesce_reaper();
    int ok = 1;
    const char *why = "notification waiters";

    long n = it_notify_create_slot();
    if (n < 0) { it_fail("T325", "notif"); return; }
    g_t325_notif = n; g_t325_woke = 0; g_t325_blocked = 0;

    long tids[T325_THREADS];
    uint32_t made = 0;
    for (uint32_t i = 0; i < T325_THREADS; i++) {
        tids[i] = it_thread_create((uint64_t)(uintptr_t)t325_body,
                                   ((uint64_t)(uintptr_t)(g_t325_stacks[i] +
                                       sizeof(g_t325_stacks[i]))) & ~0xFULL, 0);
        if (tids[i] < 0) break;
        made++;
    }
    if (made != T325_THREADS) { ok = 0; why = "threads"; }

    /* All six must reach the wait before any signal, so a signal cannot be
     * consumed by a thread that had not queued yet. */
    for (int i = 0; ok && i < 4000 && g_t325_blocked < T325_THREADS; i++)
        (void)it_sys0(SYS_YIELD);
    if (ok && g_t325_blocked != T325_THREADS) { ok = 0; why = "not all blocked"; }
    for (int i = 0; ok && i < 200; i++) (void)it_sys0(SYS_YIELD);

    /* One signal per waiter: a wait clears every pending bit, so a signal
     * wakes exactly one. */
    for (uint32_t i = 0; ok && i < T325_THREADS; i++) {
        if (it_invoke1(n, INV_NOTIFY_SIGNAL, 1) != 0) { ok = 0; why = "signal"; break; }
        for (int k = 0; k < 400 && g_t325_woke < i + 1u; k++) (void)it_sys0(SYS_YIELD);
    }
    if (ok && g_t325_woke != T325_THREADS) {
        it_fz_note("T325", g_t325_woke, T325_THREADS, 0u);
        ok = 0; why = "a waiter past the old ceiling never woke";
    }

    for (uint32_t i = 0; i < made; i++) (void)it_invoke0(tids[i], INV_TCB_EXIT);
    it_quiesce_reaper();
    it_slot_delete((uint32_t)n);
    if (ok) it_pass("T325"); else it_fail("T325", why);
}
volatile uint32_t g_t326_ran;
uint8_t           g_t326_stacks[T326_THREADS][2048];

void t326_body(void) {
    __atomic_fetch_add(&g_t326_ran, 1u, __ATOMIC_RELAXED);
    for (;;) it_sys0(SYS_YIELD);
}
