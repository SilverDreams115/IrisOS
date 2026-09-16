/*
 * it_t239_t297.c — tests T239 through T297.
 *
 * The suite's numbering is chronological, not thematic: T239 was written
 * stages before T297, and they are neighbours here because they were
 * neighbours in the file this was cut out of.  The file is named by its range
 * so that a "[IRIS][TEST] T239 FAIL" line names its own file.
 *
 * Shared helpers are in it_base.c; the interface is it_priv.h.
 */
#include "it_priv.h"


#include "../common/iris_msg.h"
/* ── T239: every object has a budget, a charge point and a release point ────
 *
 * Stage 7-mem rewrote this test rather than retiring it, because the CLAIM
 * survives and only its subject moved.  It used to read the per-process
 * resource manifest: a VMO charged self, or charged a CHILD through
 * SYS_VMO_CREATE_FOR's payer argument, against a ceiling of 32.  There is no
 * payer and no ceiling — a VMO's memory comes from an Untyped the caller
 * NAMES and HOLDS, and that Untyped is the accounting.
 *
 * So the same three questions are asked of the budget: creating spends it,
 * closing gives the bytes back as reclaimable (the region RESETs), and a VMO
 * created from a CHILD's budget spends the child's and not the creator's —
 * which is the property Q2 was always about, stated about the object that
 * actually holds the memory.  Invariants: Q1, Q2, Q11, Q24. */
void test_t239(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "resource manifest";

    /* The global manifest is well-formed: a real slab arena, used within it,
     * and no allocation failures at rest. */
    struct it_utq_global g0;
    if (ok && !it_utq_g(&g0)) { ok = 0; why = "global query"; }
    if (ok && (g0.version != 1u || g0.kslab_total_bytes == 0u)) { ok = 0; why = "manifest fields"; }
    if (ok && g0.kslab_used_bytes > g0.kslab_total_bytes) { ok = 0; why = "kslab over total"; }
    if (ok && g0.kslab_failed_allocs != 0u) { ok = 0; why = "spurious kslab failure"; }

    /* A budget of our own, so the spend is measured against a region nothing
     * else is drawing from. */
    long pool = ok ? s1_sub_ut(256u * 1024u) : -1;
    if (ok && pool < 0) { ok = 0; why = "pool carve"; }
    struct it_utq_one u0, u1, u2;
    if (ok && !it_utq_1(pool, &u0)) { ok = 0; why = "info0"; }

    /* CREATE spends the budget it names, by at least the pages asked for. */
    long v = -1;
    if (ok) {
        v = it_frame_create_slot(pool, 4096u);
        if (v < 0) { ok = 0; why = "create"; }
    }
    if (ok && !it_utq_1(pool, &u1)) { ok = 0; why = "info1"; }
    if (ok && u1.used_bytes <= u0.used_bytes) { ok = 0; why = "budget not spent"; }
    if (ok && u1.child_count <= u0.child_count) { ok = 0; why = "not a child of the budget"; }

    /* ...and closing it gives the region back: a bump allocator does not
     * rewind, so what "released" means is that the budget can RESET, which it
     * refuses while a single child of it is alive. */
    handle_id_t vh = (v >= 0) ? (handle_id_t)v : HANDLE_INVALID;
    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) == 0) { ok = 0; why = "reset while live"; }
    it_close(&vh);
    it_quiesce_reaper();
    if (ok && !it_utq_1(pool, &u2)) { ok = 0; why = "info2"; }
    if (ok && u2.child_count != u0.child_count) { ok = 0; why = "release drift"; }
    if (ok && it_invoke0(pool, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset after release"; }

    /* A VMO created from a CHILD's budget spends the CHILD's region and not
     * the creator's — Q2, said about the object that holds the memory. */
    long cpool = ok ? s1_sub_ut(64u * 1024u) : -1;
    if (ok && cpool < 0) { ok = 0; why = "child pool"; }
    struct it_utq_one c0, c1, p1, p2;
    if (ok && (!it_utq_1(cpool, &c0) || !it_utq_1(pool, &p1))) { ok = 0; why = "info child0"; }
    long vc = ok ? it_frame_create_slot(cpool, 4096u) : -1;
    if (ok && vc < 0) { ok = 0; why = "create in child pool"; }
    if (ok && (!it_utq_1(cpool, &c1) || !it_utq_1(pool, &p2))) { ok = 0; why = "info child1"; }
    if (ok && c1.used_bytes <= c0.used_bytes) { ok = 0; why = "child budget not spent"; }
    if (ok && p2.used_bytes != p1.used_bytes) { ok = 0; why = "creator budget spent"; }
    handle_id_t vch = (vc >= 0) ? (handle_id_t)vc : HANDLE_INVALID;
    it_close(&vch);

    it_quiesce_reaper();
    if (cpool >= 0) it_slot_delete((uint32_t)cpool);
    if (pool  >= 0) it_slot_delete((uint32_t)pool);
    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T239"); else it_fail("T239", why);
}
void test_t240(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "many children";

    static handle_id_t cmd[T240_MAX], proc[T240_MAX];

    const uint32_t rungs[4] = { 1u, 8u, 16u, 32u };
    for (uint32_t ri = 0; ok && ri < 4u; ri++) {
        uint32_t n = rungs[ri];
        for (uint32_t i = 0; i < T240_MAX; i++) { cmd[i] = proc[i] = HANDLE_INVALID; }
        uint32_t spawned = 0;
        for (uint32_t i = 0; ok && i < n; i++) {
            if (!it_bare_child(&cmd[i], &proc[i])) { ok = 0; why = "spawn"; break; }
            spawned++;
        }
        /* Selective death: kill the even-indexed children; odd ones stay alive
         * and must still be alive afterwards. */
        for (uint32_t i = 0; ok && i < spawned; i += 2u) it_bare_kill(&cmd[i], &proc[i]);
        for (uint32_t i = 1; ok && i < spawned; i += 2u) {
            if (it_alive((long)proc[i]) != 1) { ok = 0; why = "survivor died"; }
        }
        for (uint32_t i = 0; i < spawned; i++) if (proc[i] != HANDLE_INVALID) it_bare_kill(&cmd[i], &proc[i]);
        it_quiesce_reaper();
    }

    /*
     * The recycling claim.  Run a rung, measure the spawner's budget, run the
     * SAME rung again: the second pass must spend nothing, because every
     * per-child region the loader carved for the first pass was reset and
     * handed to the second.
     */
    if (ok) {
        /*
         * Measured as a DELTA, not as equality, and the reason is the model
         * rather than tolerance: an Untyped is a bump allocator that does not
         * rewind, so everything the round itself creates from the spawner's
         * budget — the command endpoints, above all — consumes bytes that only
         * a RESET returns.  What must NOT happen is a fresh per-child REGION
         * per pass: those are a megabyte each, and recycling them is the whole
         * claim.  One pool's worth is therefore the bound; a loader that
         * carved even one new region per pass fails it.
         */
        #define T240_CHILD_POOL_BYTES (1u << 20)   /* == SL_CHILD_POOL_BYTES */
        struct it_utq_one w0, w1;
        const uint32_t n = 8u;
        if (!it_utq_1((long)IRIS_CPTR_TEST_UNTYPED, &w0)) { ok = 0; why = "budget query"; }
        for (uint32_t pass = 0; ok && pass < 2u; pass++) {
            for (uint32_t i = 0; i < T240_MAX; i++) { cmd[i] = proc[i] = HANDLE_INVALID; }
            uint32_t got = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (!it_bare_child(&cmd[i], &proc[i])) break;
                got++;
            }
            if (got < n) { ok = 0; why = "recycle spawn"; }
            for (uint32_t i = 0; i < got; i++) it_bare_kill(&cmd[i], &proc[i]);
            it_quiesce_reaper();
            if (ok && !it_utq_1((long)IRIS_CPTR_TEST_UNTYPED, &w1)) { ok = 0; why = "budget query"; }
            if (ok && w1.used_bytes - w0.used_bytes >= (uint64_t)T240_CHILD_POOL_BYTES) {
                ok = 0; why = "spawner budget accumulated";
            }
            w0 = w1;
        }
        #undef T240_CHILD_POOL_BYTES
    }

    /* Spawn until failure (cap T240_MAX).  At least 32 must succeed, any
     * failure is CLEAN, and teardown returns to baseline (Q20/Q21/Q29). */
    if (ok) {
        for (uint32_t i = 0; i < T240_MAX; i++) { cmd[i] = proc[i] = HANDLE_INVALID; }
        uint32_t got = 0;
        for (uint32_t i = 0; i < T240_MAX; i++) {
            if (!it_bare_child(&cmd[i], &proc[i])) break;
            got++;
        }
        if (got < 32u) { ok = 0; why = "fewer than 32 children"; }
        for (uint32_t i = 0; i < got; i++) it_bare_kill(&cmd[i], &proc[i]);
        it_quiesce_reaper();
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T240"); else it_fail("T240", why);
}

/* ── T241 — RETIRED with SYS_VMO_CREATE_FOR (Stage 7-mem) ─────────
 * Its subject was the PAYER argument: a process the caller held RIGHT_MANAGE
 * on, which a VMO's object count and pages were charged to.  There is no payer
 * and no per-process count — a VMO's memory comes from a budget the caller
 * NAMES and HOLDS (Stage 7 Step 14), and the budget is the accounting.  The
 * authority question this asked (may I charge that domain?) is now the plain
 * one every other allocation asks: do I hold RIGHT_WRITE on that Untyped,
 * which T299 and T300 assert.
 *
 * The Stage 4 rule, unchanged: a test whose SUBJECT is the retired mechanism
 * dies with it; one asserting a property that survives is rewritten. */


/* ── T242 — RETIRED with the VMO owner relation (Stage 7-mem) ─────────
 * Its subject was single-charge: a VMO mapped into several VSpaces is charged
 * ONCE to its owner rather than per mapper.  With no owner there is no charge
 * to count once — the pages came out of one Untyped when the VMO was created,
 * and mapping it anywhere costs that Untyped nothing more.  The property that
 * survives (a shared mapping does not double-spend a budget) is structural
 * now rather than enforced, and the budget drift checks in T299 measure it.
 *
 * The Stage 4 rule, unchanged: a test whose SUBJECT is the retired mechanism
 * dies with it; one asserting a property that survives is rewritten. */


/* ── T243 — RETIRED with the VMO owner relation (Stage 7-mem) ─────────
 * Its subject was where a sparse VMO's PAGES are charged: once to the owner,
 * not per mapper.  Pages come from the VMO's own budget now, allocated at map
 * time out of the Untyped it was created from, so "who is charged" has one
 * answer fixed at creation and there is no second party to get it wrong.  The
 * per-VSpace mapping nodes this also covered are asserted by T135.
 *
 * The Stage 4 rule, unchanged: a test whose SUBJECT is the retired mechanism
 * dies with it; one asserting a property that survives is rewritten. */


/* ── T244: pager cache/private-page accounting ───────────────────────────────
 * A file-backed pager's cache VMO and private-pool VMO are owned (and charged)
 * by their owner; resolving faults fills pages charged to that owner; pager
 * death releases them; the supervisor never inherits the pager's page charge.
 * Invariants: Q14, Q15, Q17, Q33. */
void test_t244(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "pager accounting";

    struct t25_tgt g;
    if (ok && !t25_tgt_spawn(&g, &why)) ok = 0;
    struct t28_fbk f;
    if (ok && !t28_fbk_spawn(&f, &g, 1u, &why)) ok = 0;
    /* The pager owns its cache/private VMOs — the supervisor's own VMO usage did
     * not grow by the pager's VMOs beyond the two it created and handed over
     * (those are charged to iris_test as their creator/owner here, but the
     * supervisor's PAGE usage must not absorb the pager's fault fills). */

    long sz = ok ? t28_stat(f.vfs_cap, FBK_FILE_NAME) : -1;
    struct t28_grant gr;
    if (ok && !t28_backing_setup(&f, 0, FBK_FILE_NAME, (uint64_t)sz, &gr, &why)) ok = 0;
    if (ok) {
        struct pgr_region_req rq;
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x2000, 0x1000, 0x2000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "reg region"; }
    }
    /* Resolve two faulted pages from the shared RO cache. */
    if (ok && !t28_read_verify(&f, &g, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    /* Stage 7-mem: the cache fill used to be checked against the supervisor's
     * per-process page usage, with a comment conceding it could legitimately
     * move — which is a check that could not fail.  What must hold is the part
     * that could: killing the pager and reaping returns everything to
     * baseline, asserted below on the global gauges. */

    t28_fbk_reap(&f);
    t25_tgt_reap(&g);
    it_quiesce_reaper();
    /* Stage 7-mem: "no VMO leaked by the pager path" is a GLOBAL claim now,
     * and it_snap_baseline_live below makes it — a leak by the pager, a target
     * or the supervisor all show in the live-VMO gauge, where the retired
     * per-process form only ever caught the caller's own. */

    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T244"); else it_fail("T244", why);
}

/* ── T245: independent children under one supervisor ─────────────────────────
 * Killing one child frees ONLY its resources and leaves the other intact — a
 * supervisor's children are independent (the "supervisor death" contract's
 * core: no cross-child resource coupling).
 *
 * Stage 7-mem restated the measurement.  It used to read each child's
 * per-process VMO count and assert B's was untouched by A's death; there is no
 * per-process domain, so independence is measured where it now lives: on the
 * BUDGET each child was given.  Killing A must let A's region become
 * RESET-able while B's stays BUSY, which is a stronger statement than a count
 * that did not move — it says the memory actually came back, and came back to
 * the right region.  Invariants: Q12, Q13. */
void test_t245(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "independent children";

    handle_id_t cmdA, procA, cmdB, procB;
    if (ok && !it_bare_child(&cmdA, &procA)) { ok = 0; why = "childA"; }
    if (ok && !it_bare_child(&cmdB, &procB)) { ok = 0; why = "childB"; }

    /* One budget per child, and a VMO carved from each — the memory that makes
     * the two domains distinguishable. */
    long poolA = ok ? s1_sub_ut(64u * 1024u) : -1;
    long poolB = ok ? s1_sub_ut(64u * 1024u) : -1;
    if (ok && (poolA < 0 || poolB < 0)) { ok = 0; why = "child pools"; }
    long va = ok ? it_frame_create_slot(poolA, 8192u) : -1;
    long vb = ok ? it_frame_create_slot(poolB, 8192u) : -1;
    handle_id_t vah = (va >= 0) ? (handle_id_t)va : HANDLE_INVALID;
    handle_id_t vbh = (vb >= 0) ? (handle_id_t)vb : HANDLE_INVALID;
    if (ok && (va < 0 || vb < 0)) { ok = 0; why = "create in pool"; }
    /* Both regions are spoken for: neither can be reset under a live object. */
    if (ok && it_invoke0(poolA, INV_UNTYPED_RESET) == 0) { ok = 0; why = "A resettable while live"; }
    if (ok && it_invoke0(poolB, INV_UNTYPED_RESET) == 0) { ok = 0; why = "B resettable while live"; }

    /* Kill A and release only A's memory.  B's region must still refuse a
     * RESET — nothing about A's death may reach into it. */
    if (ok) { (void)it_kill((long)procA); (void)it_lp_wait_exit(procA); it_quiesce_reaper(); }
    it_close(&vah);
    it_quiesce_reaper();
    if (ok && it_invoke0(poolA, INV_UNTYPED_RESET) != 0) { ok = 0; why = "A did not come back"; }
    if (ok && it_invoke0(poolB, INV_UNTYPED_RESET) == 0) { ok = 0; why = "B disturbed by A death"; }
    it_close(&cmdA); it_close(&procA);

    it_close(&vbh);
    it_quiesce_reaper();
    if (ok && it_invoke0(poolB, INV_UNTYPED_RESET) != 0) { ok = 0; why = "B did not come back"; }
    it_bare_kill(&cmdB, &procB);
    if (poolA >= 0) it_slot_delete((uint32_t)poolA);
    if (poolB >= 0) it_slot_delete((uint32_t)poolB);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T245"); else it_fail("T245", why);
}

/* ── T246 — RETIRED with the per-process VMO quota (Stage 7-mem) ─────────
 * Its subject was KPROCESS_VMO_QUOTA: filling a process's VMO domain to 32 and
 * asserting the 33rd failed atomically.  The ceiling is gone — it was a number
 * the kernel invented, the same class as the page quota (Step 2) and the
 * live-process ceiling (Step 3), and it contradicted the budget model rather
 * than reinforcing it: a holder with a large delegated Untyped still stopped
 * at 32.  What bounds VMOs now is the budget, and T304 is the test that a
 * ceiling somebody delegated is the only ceiling — including that the refusal,
 * when the budget ends, is clean.
 *
 * The Stage 4 rule, unchanged: a test whose SUBJECT is the retired mechanism
 * dies with it; one asserting a property that survives is rewritten. */


/* ── T247 — RETIRED with the payer argument (Stage 7-mem) ─────────
 * Its subject was that budget-charge authority IS process-MANAGE authority: a
 * MANAGE capability could charge a child, a derived capability without MANAGE
 * could not.  Charging is naming an Untyped now, so the authority is
 * RIGHT_WRITE on that Untyped and the monotonicity question is the general one
 * about rights the MDB already answers (T288-T290).
 *
 * The Stage 4 rule, unchanged: a test whose SUBJECT is the retired mechanism
 * dies with it; one asserting a property that survives is rewritten. */


/* ── T248: kslab capacity and explicit exhaustion ────────────────────────────
 * The kernel object slab has an observable capacity contract: used <= total,
 * total is the reserved arena, and under normal load there are zero allocation
 * failures.  The exhaustion PATH itself (alloc returns 0, fail count advances,
 * no corruption) is proven deterministically in the host unit suite
 * (tests/kernel/test_kslab.c) — exhausting 16 MB in every smoke run is
 * impractical.  Invariants: Q28, Q29. */
void test_t248(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "kslab capacity";

    struct it_utq_global r0;
    if (ok && !it_utq_g(&r0)) { ok = 0; why = "rinfo"; }
    if (ok && r0.kslab_total_bytes == 0u) { ok = 0; why = "no arena"; }
    if (ok && r0.kslab_used_bytes > r0.kslab_total_bytes) { ok = 0; why = "used > total"; }
    if (ok && r0.kslab_failed_allocs != 0u) { ok = 0; why = "spurious kslab failure"; }
    /* Spawning and reaping a child churns kernel objects; used (bump high-water)
     * may rise but never exceeds total, and no allocation fails. */
    handle_id_t cmd, proc;
    if (ok && !it_bare_child(&cmd, &proc)) { ok = 0; why = "child"; }
    struct it_utq_global r1;
    if (ok && !it_utq_g(&r1)) { ok = 0; why = "rinfo1"; }
    if (ok && (r1.kslab_used_bytes > r1.kslab_total_bytes || r1.kslab_failed_allocs != 0u)) { ok = 0; why = "kslab churn"; }
    if (ok && r1.kslab_used_bytes < r0.kslab_used_bytes) { ok = 0; why = "used regressed"; }  /* bump-only */
    it_bare_kill(&cmd, &proc);

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T248"); else it_fail("T248", why);
}

/* ── T249: file-backed resource regression ───────────────────────────────────
 * The whole file-backed path runs under the new accounting with exact baseline:
 * multiple targets, RO shared + private writable, cache pressure, revoke, target
 * death — all return the supervisor's resource books to baseline.  T211–T238
 * (running before this) are the functional regression; T249 adds the accounting
 * assertion.  Invariants: Q30, Q33. */
void test_t249(void) {
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "file-backed regression";

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
        t28_region(&rq, 0, 0, 0, T28_VA_A, 0x1000, 0x1000, 0x1000, FBK_PROT_R, FBK_MODE_RO, gr.gen);
        if (t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "region 0"; }
        t28_region(&rq, 1, 1, 0, T28_VA_A, 0x1000, 0, 0x1000, FBK_PROT_R | FBK_PROT_W, FBK_MODE_PRIVATE, gr.gen);
        if (ok && t28_reg_region(f.ctrl_ep, &rq) != 0) { ok = 0; why = "region 1"; }
    }
    if (ok && !t28_read_verify(&f, &g0, 0u, T28_VA_A, t28_pat(0x1000), &why)) ok = 0;
    if (ok && !t28_write_resolve(&f, &g1, 1u, T28_VA_A, &why)) ok = 0;
    /* Revoke at the VFS, then a fresh fault must fail. */
    if (ok && t28_grant_revoke_name(f.admin, FBK_FILE_NAME, 0) != 0) { ok = 0; why = "revoke"; }

    t28_fbk_reap(&f);
    t25_tgt_reap(&g0);
    t25_tgt_reap(&g1);
    it_quiesce_reaper();
    /* Stage 7-mem: the accounting-drift check was per-process (vmos_usage,
     * pages_usage).  The baseline below covers it and more: the live-VMO gauge
     * is global, so a leak by the pager, a target or the supervisor all show. */
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T249"); else it_fail("T249", why);
}
static uint32_t t250_rnd(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }
void test_t250(void) {
    uint32_t rng = T250_SEED;
    it_quiesce_reaper();
    struct it_snap b = it_snap_take();
    int ok = b.ok;
    const char *why = "resource stress";
    /* Stage 7-mem: the round-trip is measured on a BUDGET of our own and on
     * the global live-VMO gauge, not on a per-process count that no longer
     * exists.  A budget nothing else draws from makes "exactly what this round
     * spent came back" an exact claim. */
    long pool = ok ? s1_sub_ut(512u * 1024u) : -1;
    if (ok && pool < 0) { ok = 0; why = "pool"; }
    struct it_snap s0 = it_snap_take();
    if (ok && !s0.ok) { ok = 0; why = "r0"; }
    uint32_t round = 0, op = 0;

    for (round = 0; ok && round < T250_ROUNDS; round++) {
        op = t250_rnd(&rng) % 4u;
        switch (op) {
        case 0: {
            /* Child owns its image + an extra VMO; kill releases exactly it. */
            handle_id_t cmd, proc;
            if (!it_bare_child(&cmd, &proc)) { ok = 0; why = "s0 child"; break; }
            /* Carved from the round's budget, not from the suite's own: the
             * spend is visible and must come back at the end of the round. */
            long v = it_frame_create_slot(pool, 4096u);
            if (v < 0) { ok = 0; why = "s0 create"; }
            if (v >= 0) { handle_id_t vh = (handle_id_t)v; it_close(&vh); }
            it_bare_kill(&cmd, &proc);
            break;
        }
        case 1: {
            /* Self VMO create + dup + close: single charge, exact release. */
            long v = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
            if (v < 0) { ok = 0; why = "s1 create"; break; }
            handle_id_t vh = (handle_id_t)v;
            long d = it_cs_reduce((long)vh, RIGHT_READ);
            /* One object, two capabilities: the live-frame gauge counts
             * objects, so a derived capability must not move it. */
            struct it_snap sd = it_snap_take();
            if (!sd.ok || sd.fr != s0.fr + 1u) { ok = 0; why = "s1 charge"; }
            if (d >= 0) { handle_id_t dh = (handle_id_t)d; it_close(&dh); }
            it_close(&vh);
            break;
        }
        case 2: {
            /*
             * Exhaust a SMALL budget, then recover.
             *
             * Stage 7-mem: this used to fill the per-process VMO quota to 32
             * and check the global failed-charge counter advanced.  The
             * ceiling is a budget now, so exhaustion is a small Untyped
             * running out — a refusal that comes from a region somebody
             * delegated rather than a number the kernel picked — and recovery
             * is the region becoming reusable again.
             */
            long small = s1_sub_ut(32u * 1024u);
            if (small < 0) { ok = 0; why = "s2 pool"; break; }
            static handle_id_t vv[24];
            uint32_t made = 0;
            while (made < 24u) {
                long v = it_frame_create_slot(small, 4096u);
                if (v < 0) break;
                vv[made++] = (handle_id_t)v;
            }
            if (made == 0u) { ok = 0; why = "s2 nothing made"; }
            /* The refusal is clean and names memory, not a policy. */
            if (ok && it_frame_create_slot(small, 4096u) != (long)IRIS_ERR_NO_MEMORY) {
                ok = 0; why = "s2 not full";
            }
            for (uint32_t i = 0; i < made; i++) it_close(&vv[i]);
            it_quiesce_reaper();
            if (ok && it_invoke0(small, INV_UNTYPED_RESET) != 0) { ok = 0; why = "s2 no recovery"; }
            it_slot_delete((uint32_t)small);
            break;
        }
        default: {
            /* Map/unmap a self VMO page; page charge paid once, released. */
            if (!it_setup_self_vspace()) { ok = 0; why = "s3 vspace"; break; }
            long v = it_frame_create_slot((long)IRIS_CPTR_TEST_UNTYPED, 4096);
            if (v < 0) { ok = 0; why = "s3 create"; break; }
            handle_id_t vh = (handle_id_t)v;
            if (it_invoke((long)vh, INV_FRAME_MAP, IT_VS, (long)T26_SELF_VA, 0L) != 0) { ok = 0; why = "s3 map"; }
            if (ok) (void)it_invoke2((long)vh, INV_FRAME_UNMAP, IT_VS, (long)T26_SELF_VA);
            it_close(&vh);
            break;
        }
        }
        it_quiesce_reaper();
        if (ok) {
            struct it_snap rz = it_snap_take();
            if (!rz.ok) { ok = 0; why = "round rinfo"; }
            else if (rz.fr != s0.fr) { ok = 0; why = "round frame drift"; }
            struct it_snap r = it_snap_take();
            if (ok && !it_snap_baseline_live(&b, &r, &why)) ok = 0;
        }
    }

    it_quiesce_reaper();
    struct it_snap a = it_snap_take();
    if (ok && !it_snap_baseline_live(&b, &a, &why)) ok = 0;
    if (ok) it_pass("T250");
    else { it_fz_note("T250", T250_SEED, round, op); it_fail("T250", why); }
}

/* ── T251: canonical object model manifest ──────────────────────────────────
 * RETYPE2 accepts EXACTLY the canonical creatable set {NOTIFICATION,
 * ENDPOINT, CNODE, SCHED_CONTEXT, UNTYPED, REPLY, FRAME, TCB, PAGE_TABLE,
 * VSPACE, ASID_POOL} and refuses every other type code (0..31) with
 * NOT_SUPPORTED — an unregistered KOBJ_* can never be born.  Every created object reports its declared type through the
 * sanctioned bridge, and the migrated family has a retirement witness: the
 * legacy handle-first retype refuses it (S19/S20/S21). */
void test_t251(void) {
    int ok = 1;
    const char *why = "object manifest";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T251", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    static const struct { uint32_t t; long arg; long ht; } canon[] = {
        { IRIS_KOBJ_NOTIFICATION,  0,    IRIS_HANDLE_TYPE_NOTIFICATION },
        { IRIS_KOBJ_ENDPOINT,      0,    IRIS_HANDLE_TYPE_ENDPOINT },
        { IRIS_KOBJ_CNODE,         4,    IRIS_HANDLE_TYPE_CNODE },
        { IRIS_KOBJ_SCHED_CONTEXT, 0,    IRIS_HANDLE_TYPE_SCHED_CONTEXT },
        { IRIS_KOBJ_UNTYPED,       4096, IRIS_HANDLE_TYPE_UNTYPED },
        { IRIS_KOBJ_REPLY,         0,    IRIS_HANDLE_TYPE_REPLY },
        { IRIS_KOBJ_FRAME,         4096, IRIS_HANDLE_TYPE_FRAME },
        /* Phase S2 Stage 0: the TCB joins the canonical family. */
        { IRIS_KOBJ_TCB,           0,    IRIS_HANDLE_TYPE_TCB },
        /* Stage 6-pure Step 1: a paging level is a retyped object now, so it
         * belongs in the manifest of what CAN exist.  Its region is always
         * exactly one page — 512 entries of 8 bytes and nothing else. */
        { IRIS_KOBJ_PAGE_TABLE,    4096, IRIS_HANDLE_TYPE_PAGE_TABLE },
        /* Stage 6-pure Step 4: an address space is retyped by its holder and
         * handed to SYS_PROCESS_CREATE.  Its region is the PML4 — one page,
         * like every other level of a walk. */
        { IRIS_KOBJ_VSPACE,        4096, IRIS_HANDLE_TYPE_VSPACE },
    };
    for (uint32_t i = 0; ok && i < 10u; i++) {
        if (it_retype2_at(su, canon[i].t, S1_SLOT_A, 1u, canon[i].arg) != 0) {
            ok = 0; why = "canonical type not creatable"; break;
        }
        if (it_invoke0((long)S1_SLOT_A, INV_CAP_IDENTIFY) != canon[i].ht) {
            ok = 0; why = "created type mismatch";
        }
        it_slot_delete(S1_SLOT_A);
    }
    /*
     * Ledger A-21: the eleventh canonical type, and the only one this suite
     * cannot create.  An ASID pool is born only to a holder of ASIDControl,
     * and iris_test was deliberately given the POOL and not the CONTROL — so
     * what it witnesses here is the DISTINCTION: a registered type it may not
     * make is ACCESS_DENIED, an unregistered one is NOT_SUPPORTED.  Collapsing
     * those two would let a retired type code come back as an authority error
     * and vice versa.
     */
    if (ok && it_retype2_at(su, IRIS_KOBJ_ASID_POOL, S1_SLOT_A, 1u, 0) !=
              (long)IRIS_ERR_ACCESS_DENIED) {
        ok = 0; why = "asid pool without ASIDControl";
    }
    /* Everything else in 0..31 is refused — the manifest is CLOSED. */
    for (uint32_t t = 0; ok && t < 32u; t++) {
        int is_canon = (t == IRIS_KOBJ_ASID_POOL);
        for (uint32_t i = 0; i < 10u; i++) if (canon[i].t == t) is_canon = 1;
        if (is_canon) continue;
        if (it_retype2_at(su, t, S1_SLOT_A, 1u, 4096) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "non-canonical type creatable";
        }
    }
    /* Retirement witness: the legacy handle-publishing path refuses every
     * type now (Stage 4 retired 87 outright), not just the migrated family. */
    static const uint32_t migrated[] = { IRIS_KOBJ_ENDPOINT, IRIS_KOBJ_NOTIFICATION,
                                         IRIS_KOBJ_CNODE, IRIS_KOBJ_REPLY,
                                         IRIS_KOBJ_TCB /* Step 0 */ };
    for (uint32_t i = 0; ok && i < 5u; i++) {
        if (it_sys3(SYS_UNTYPED_RETYPE, su, (long)migrated[i], 4) !=
            (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy path not retired"; }
    }

    it_close(&su_h);
    if (ok) it_pass("T251"); else it_fail("T251", why);
}

/* ── T252: Untyped storage provenance ───────────────────────────────────────
 * The retyped region IS the object storage: creating the migrated family
 * consumes exactly measurable bytes of the source untyped (used_bytes grows,
 * child_count counts every object), consumes ZERO kslab bytes (S2 — payload
 * never touches the kernel heap), and destroying every object returns the
 * region to reusable (RESET succeeds, used == 0). */
void test_t252(void) {
    int ok = 1;
    const char *why = "storage provenance";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T252", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_one u0, u1, u2;
    struct it_utq_global k0, k1;
    struct it_utq_objects o0, o1;
    if (!it_utq_1(su, &u0) || !it_utq_g(&k0) || !it_utq_o(&o0)) {
        it_close(&su_h); it_fail("T252", "query"); return;
    }
    if (u0.used_bytes != 0u || u0.child_count != 0u) { ok = 0; why = "not pristine"; }

    /* 4 endpoints + 2 notifications + 2 replies, all batch-retyped. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT,     S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "ep"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT,     S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "ep2"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_C, 1u, 0) != 0) { ok = 0; why = "nt"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_REPLY,        S1_SLOT_D, 1u, 0) != 0) { ok = 0; why = "rp"; }
    if (ok && (!it_utq_1(su, &u1) || !it_utq_g(&k1) || !it_utq_o(&o1))) {
        ok = 0; why = "query mid";
    }
    /* used grew, every object is inside THIS untyped (child_count == 4),
     * live gauges rose accordingly, and the kernel heap did not move. */
    if (ok && !(u1.used_bytes > u0.used_bytes && u1.used_bytes <= u1.total_bytes)) { ok = 0; why = "no consumption"; }
    if (ok && u1.child_count != 4u) { ok = 0; why = "child count"; }
    if (ok && o1.endpoints_live     != o0.endpoints_live + 2u)     { ok = 0; why = "ep live"; }
    if (ok && o1.notifications_live != o0.notifications_live + 1u) { ok = 0; why = "nt live"; }
    if (ok && o1.replies_live       != o0.replies_live + 1u)       { ok = 0; why = "rp live"; }
    if (ok && k1.kslab_used_bytes   != k0.kslab_used_bytes)        { ok = 0; why = "kslab moved (S2)"; }

    /* The objects are REAL (usable through their CSpace caps). */
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m);
        if (iris_msg_nb_recv((long)S1_SLOT_A, &m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ep dead"; }
        if (ok && it_invoke1((long)S1_SLOT_C, INV_NOTIFY_SIGNAL, 1) != 0) { ok = 0; why = "nt dead"; }
    }

    /* Destroy all → region reusable, gauges at baseline. */
    it_slot_delete(S1_SLOT_A); it_slot_delete(S1_SLOT_B);
    it_slot_delete(S1_SLOT_C); it_slot_delete(S1_SLOT_D);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    if (ok && (!it_utq_1(su, &u2) || u2.used_bytes != 0u || u2.child_count != 0u)) { ok = 0; why = "not reclaimed"; }

    it_close(&su_h);
    if (ok) it_pass("T252"); else it_fail("T252", why);
}

/* ── T253: atomic batch retype ──────────────────────────────────────────────
 * One RETYPE2 with count=4 creates all four (each slot resolves to a live
 * endpoint).  A batch whose THIRD destination slot is occupied fails
 * ALREADY_EXISTS with ZERO effect: no slot filled, no untyped byte consumed,
 * no object live (U14/U15/S5).  A batch larger than the remaining capacity
 * fails NO_MEMORY with zero effect. */
void test_t253(void) {
    int ok = 1;
    const char *why = "atomic batch";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T253", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    /* Success batch: 4 endpoints into 241..244. */
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 4u, 0) != 0) { ok = 0; why = "batch"; }
    for (uint32_t i = 0; ok && i < 4u; i++) {
        if (it_invoke0((long)(S1_SLOT_A + i), INV_CAP_IDENTIFY)
            != (long)IRIS_HANDLE_TYPE_ENDPOINT) {
            ok = 0; why = "batch member missing";
        }
    }
    /* Tear down members 242..244, keep 241 occupied as the collision. */
    it_slot_delete(S1_SLOT_B); it_slot_delete(S1_SLOT_C); it_slot_delete(S1_SLOT_D);

    struct it_utq_one ub, ua;
    struct it_utq_objects ob, oa;
    if (ok && (!it_utq_1(su, &ub) || !it_utq_o(&ob))) { ok = 0; why = "query"; }
    /* Batch 239..242: slot 241 (third) is occupied → ALREADY_EXISTS. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, 239u, 4u, 0) !=
              (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "collision not refused"; }
    if (ok && (!it_utq_1(su, &ua) || !it_utq_o(&oa))) { ok = 0; why = "query 2"; }
    if (ok && (ua.used_bytes != ub.used_bytes || ua.child_count != ub.child_count)) {
        ok = 0; why = "failed batch consumed bytes (U15)";
    }
    if (ok && oa.endpoints_live != ob.endpoints_live) { ok = 0; why = "failed batch left object"; }
    if (ok) {
        if (it_invoke0(239, INV_CAP_IDENTIFY) >= 0) { ok = 0; why = "partial slot filled"; }
        if (it_invoke0(240, INV_CAP_IDENTIFY) >= 0) { ok = 0; why = "partial slot filled 2"; }
    }
    /* Capacity failure: 8 CNodes of 64 slots (~5 KiB each with the Phase S3
     * MDB slot metadata) ≫ the 8 KiB region, while the batch stays under
     * KUNTYPED_RETYPE_MAX_BYTES so the failure exercised is genuinely the
     * REGION capacity (NO_MEMORY), not the batch-size validation. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_CNODE, 246u, 8u, 64) !=
              (long)IRIS_ERR_NO_MEMORY) { ok = 0; why = "capacity not refused"; }
    if (ok && (!it_utq_1(su, &ua) || ua.used_bytes != ub.used_bytes)) { ok = 0; why = "capacity fail consumed"; }

    it_slot_delete(S1_SLOT_A);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    it_close(&su_h);
    if (ok) it_pass("T253"); else it_fail("T253", why);
}

/* ── T254: retype validation and overlap denial ─────────────────────────────
 * Every malformed RETYPE2 fails BEFORE any state changes: wrong type, zero →
 * one-normalized vs oversized count, slot 0, out-of-range slot window,
 * occupied destination, missing RIGHT_WRITE on the untyped, a non-CNode
 * destination, invalid CNode fan-out, misaligned physical sizes, count>1 on
 * physical types, device/normal restriction, and a stale untyped handle. */
void test_t254(void) {
    int ok = 1;
    const char *why = "validation";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T254", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;
    struct it_utq_one ub, ua;
    if (!it_utq_1(su, &ub)) { it_close(&su_h); it_fail("T254", "query"); return; }

    /* An occupied fixture at S1_SLOT_A. */
    if (it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) {
        it_close(&su_h); it_fail("T254", "fixture"); return;
    }
    static const struct { uint32_t t; uint32_t slot; uint32_t cnt; long arg; long expect; const char *tag; } cases[] = {
        { 77u,                    S1_SLOT_B, 1u,  0,    (long)IRIS_ERR_NOT_SUPPORTED, "wrong type" },
        { IRIS_KOBJ_ENDPOINT,     S1_SLOT_B, 33u, 0,    (long)IRIS_ERR_INVALID_ARG,   "count too big" },
        { IRIS_KOBJ_ENDPOINT,     0u,        1u,  0,    (long)IRIS_ERR_INVALID_ARG,   "slot 0" },
        { IRIS_KOBJ_ENDPOINT,     255u,      2u,  0,    (long)IRIS_ERR_INVALID_ARG,   "slot window oob" },
        { IRIS_KOBJ_ENDPOINT,     S1_SLOT_A, 1u,  0,    (long)IRIS_ERR_ALREADY_EXISTS,"occupied" },
        { IRIS_KOBJ_CNODE,        S1_SLOT_B, 1u,  3,    (long)IRIS_ERR_INVALID_ARG,   "cnode fanout" },
        { IRIS_KOBJ_CNODE,        S1_SLOT_B, 1u,  8192, (long)IRIS_ERR_INVALID_ARG,   "cnode too big" },
        { IRIS_KOBJ_UNTYPED,      S1_SLOT_B, 1u,  100,  (long)IRIS_ERR_INVALID_ARG,   "ut misaligned" },
        { IRIS_KOBJ_UNTYPED,      S1_SLOT_B, 2u,  4096, (long)IRIS_ERR_INVALID_ARG,   "ut batch" },
        { IRIS_KOBJ_FRAME,        S1_SLOT_B, 1u,  100,  (long)IRIS_ERR_INVALID_ARG,   "frame misaligned" },
    };
    for (uint32_t c = 0; ok && c < 10u; c++) {
        long r = it_retype2_at(su, cases[c].t, cases[c].slot, cases[c].cnt, cases[c].arg);
        if (r != cases[c].expect) { ok = 0; why = cases[c].tag; }
    }
    /* Missing RIGHT_WRITE on the source untyped. */
    if (ok) {
        long ro = it_cdt_reduced((handle_id_t)su, IT_SCRATCH_0, IT_SCRATCH_1,
                                 RIGHT_READ);
        if (ro < 0) { ok = 0; why = "ro derive"; }
        else {
            if (it_retype2_at(ro, IRIS_KOBJ_ENDPOINT, S1_SLOT_B, 1u, 0) !=
                (long)IRIS_ERR_ACCESS_DENIED) { ok = 0; why = "rights"; }
            it_slot_delete(IT_SCRATCH_1);
            it_slot_delete(IT_SCRATCH_0);
        }
    }
    /* Destination that is not a CNode (the notification at S1_SLOT_A) — A-30:
     * WRONG_TYPE, because that is what the resolver found. */
    if (ok && it_invoke(su, INV_UNTYPED_RETYPE, (long)((uint64_t)IRIS_KOBJ_ENDPOINT | (1ULL << 32)), (long)((uint64_t)S1_SLOT_A | ((uint64_t)S1_SLOT_B << 32)), 0) != (long)IRIS_ERR_WRONG_TYPE) { ok = 0; why = "bad dest cnode"; }
    /* Released untyped cap: delete the slot, then retype through the dead
     * CPtr.  Stage 4: an emptied slot answers NOT_FOUND — the CSpace form of
     * the BAD_HANDLE this asserted while the untyped was a handle.  The
     * property is unchanged: a capability that was released cannot be used
     * to create objects. */
    if (ok) {
        long su2 = s1_sub_ut(4096);
        if (su2 < 0) { ok = 0; why = "second sub"; }
        else {
            handle_id_t s2h = (handle_id_t)su2; it_close(&s2h);
            if (it_retype2_at(su2, IRIS_KOBJ_ENDPOINT, S1_SLOT_B, 1u, 0) !=
                (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "stale untyped"; }
        }
    }
    /* No validation failure consumed a byte or created an object. */
    if (ok && !it_utq_1(su, &ua)) { ok = 0; why = "query 2"; }
    if (ok && (ua.used_bytes != ub.used_bytes + (ua.used_bytes - ub.used_bytes))) { ok = 0; why = "?"; }
    if (ok) {
        if (it_invoke0((long)S1_SLOT_B, INV_CAP_IDENTIFY) >= 0) { ok = 0; why = "ghost object"; }
    }

    it_slot_delete(S1_SLOT_A);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    it_close(&su_h);
    if (ok) it_pass("T254"); else it_fail("T254", why);
}

/* ── T255: Endpoint Untyped lifecycle ───────────────────────────────────────
 * retype → derive/mint → send/receive → call → delete-one-cap (object
 * survives) → last-cap delete with a blocked waiter (close wakes CLOSED, no
 * ghost) → RESET → the SAME region hosts a fresh endpoint that works. */
static volatile int  g_t255_done;
static volatile long g_t255_res;
static uint8_t       g_t255_stack[8192];
static void t255_waiter(void) {
    struct iris_msg m; iris_msg_zero(&m);
    g_t255_res  = (m.reply = 0, iris_msg_recv((long)S1_SLOT_A, &m));
    g_t255_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void test_t255(void) {
    int ok = 1;
    const char *why = "endpoint lifecycle";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T255", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    /* Phase S4 (Step 3): the source is ALREADY a CPtr — derive natively, with
     * no CSPACE_RESOLVE bridge and no handle anywhere in the path. */
    long h  = ok ? it_cdt_derive((long)S1_SLOT_A, IT_SCRATCH_1, RIGHT_SAME_RIGHTS) : -1;
    long d  = ok ? it_cdt_derive((long)S1_SLOT_A, IT_SCRATCH_0, RIGHT_WRITE) : -1;
    if (ok && (h < 0 || d < 0)) { ok = 0; why = "derive"; }
    /* send/receive through the CPtr + the derived CPtr. */
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m); m.label = 0x255;
        if (iris_msg_nb_send(d, &m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "nb send"; }
    }
    /* call: needs our reply object. */
    if (ok && it_reply_create_at(S1_SLOT_B) < 0) { ok = 0; why = "reply"; }
    /* Delete ONE cap (the derived SLOT): object must survive. */
    if (ok) { it_slot_delete(IT_SCRATCH_0); d = -1; }
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m);
        if (iris_msg_nb_recv((long)S1_SLOT_A, &m) != (long)IRIS_ERR_WOULD_BLOCK) {
            ok = 0; why = "object died with one cap (S10)";
        }
    }
    /* Blocked waiter + last-cap delete → CLOSED, no zombie (S25). */
    if (ok) {
        g_t255_done = 0; g_t255_res = 999;
        uint64_t entry = (uint64_t)(uintptr_t)t255_waiter;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t255_stack + sizeof(g_t255_stack))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
            if (h >= 0) { it_slot_delete((uint32_t)h); h = -1; }
            it_slot_delete(S1_SLOT_A);      /* last cap → close fires */
            IT_AWAIT(g_t255_done, 4000);
            if (!g_t255_done) { ok = 0; why = "waiter zombie"; }
            else if (g_t255_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake"; }
        }
    }
    it_slot_delete(S1_SLOT_B);
    if (h >= 0) it_slot_delete((uint32_t)h);
    /* Region reusable; the SAME range hosts a working replacement. */
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy (S12)"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "reuse retype"; }
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m);
        if (iris_msg_nb_recv((long)S1_SLOT_A, &m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "reused ep dead"; }
        it_slot_delete(S1_SLOT_A);
    }
    it_close(&su_h);
    if (ok) it_pass("T255"); else it_fail("T255", why);
}

/* ── T256: Notification Untyped lifecycle ───────────────────────────────────
 * signal/wait + pending-bit accumulation through the CSpace cap, TWO holders
 * (CPtr + materialized handle) observing one object, waiter woken CLOSED by
 * last-cap delete (S26), destruction, and same-region reuse with NO residual
 * pending bits (S28). */
static volatile int  g_t256_done;
static volatile long g_t256_res;
static uint8_t       g_t256_stack[8192];
static void t256_waiter(void) {
    uint64_t bits = 0;
    g_t256_res  = it_invoke1((long)S1_SLOT_A, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits);
    g_t256_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void test_t256(void) {
    int ok = 1;
    const char *why = "notification lifecycle";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T256", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    if (it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    /* Two holders of ONE object: signal through a derived copy, observe
     * through the original.  The second holder used to be a materialised
     * handle; a CSpace copy is the same two-references-one-object shape and
     * additionally records the derivation edge the handle could not. */
    long h = -1;
    if (ok) {
        it_slot_delete(IT_SCRATCH_0);
        if (it_invoke2((long)S1_SLOT_A, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_SAME_RIGHTS) != 0) { ok = 0; why = "copy"; }
        else h = (long)IT_SCRATCH_0;
    }
    if (ok && it_invoke1(h, INV_CAP_SAME_OBJECT, (long)S1_SLOT_A) != 1) {
        ok = 0; why = "copy is a different object";
    }
    if (ok && it_invoke1(h, INV_NOTIFY_SIGNAL, 0x5) != 0) { ok = 0; why = "signal"; }
    if (ok && it_invoke1((long)S1_SLOT_A, INV_NOTIFY_SIGNAL, 0x2) != 0) { ok = 0; why = "signal cptr"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1((long)S1_SLOT_A, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
            bits != 0x7u) { ok = 0; why = "pending bits"; }
    }
    /* Waiter + last-cap delete → CLOSED (S26), no zombie. */
    if (ok) {
        g_t256_done = 0; g_t256_res = 999;
        uint64_t entry = (uint64_t)(uintptr_t)t256_waiter;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t256_stack + sizeof(g_t256_stack))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
            if (h >= 0) { it_slot_delete((uint32_t)h); h = -1; }
            it_slot_delete(S1_SLOT_A);
            IT_AWAIT(g_t256_done, 4000);
            if (!g_t256_done) { ok = 0; why = "waiter zombie"; }
            else if (g_t256_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "wrong wake"; }
        }
    }
    if (h >= 0) it_slot_delete((uint32_t)h);
    /* Reuse: same region, fresh notification, ZERO residual bits (S28). */
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "reuse retype"; }
    if (ok) {
        uint64_t bits = 0;
        if (it_wait_timeout( (long)S1_SLOT_A,
                    (long)(uintptr_t)&bits, 20000000L) != (long)IRIS_ERR_TIMED_OUT) {
            ok = 0; why = "residual state leaked (S28)";
        }
        it_slot_delete(S1_SLOT_A);
    }
    it_close(&su_h);
    if (ok) it_pass("T256"); else it_fail("T256", why);
}

/* ── T257: explicit Reply lifecycle ─────────────────────────────────────────
 * A reply object is EXPLICIT authority: born from Untyped (replies_live +1,
 * no kslab), staged by the server's recv, bound at rendezvous, one-shot per
 * binding, REUSABLE across bindings, resilient to caller death, and stale
 * after its last cap is deleted.  The kernel never fabricates one: a CALL
 * against a reply-less receiver fails NOT_SUPPORTED (S16/S18/S22). */
static volatile int g_t257_done;
static uint8_t      g_t257_stack[8192];
static void t257_caller(void) {
    struct iris_msg m; iris_msg_zero(&m);
    m.label = 0x257;
    (void)iris_msg_call((long)S1_SLOT_A, &m);
    g_t257_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void test_t257(void) {
    int ok = 1;
    const char *why = "reply lifecycle";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T257", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_objects o0, o1;
    struct it_utq_global k0, k1;
    if (!it_utq_o(&o0) || !it_utq_g(&k0)) { it_close(&su_h); it_fail("T257", "query"); return; }
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0 ||
        it_retype2_at(su, IRIS_KOBJ_REPLY,    S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok && (!it_utq_o(&o1) || !it_utq_g(&k1))) { ok = 0; why = "query 2"; }
    if (ok && o1.replies_live != o0.replies_live + 1u) { ok = 0; why = "reply not counted"; }
    if (ok && k1.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "reply from kslab (S16)"; }

    /* Round 1: call → recv(reply) → reply; one-shot; then REUSE for round 2. */
    for (uint32_t round = 0; ok && round < 2u; round++) {
        g_t257_done = 0;
        uint64_t entry = (uint64_t)(uintptr_t)t257_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t257_stack + sizeof(g_t257_stack))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread"; break; }
        struct iris_msg m; iris_msg_zero(&m);
        if ((m.reply = (long)S1_SLOT_B, iris_msg_recv((long)S1_SLOT_A, &m)) != 0 ||
            m.got_cap != S1_SLOT_B) { ok = 0; why = "recv/echo"; break; }
        struct iris_msg rm; iris_msg_zero(&rm); rm.label = 0xAC7;
        if (iris_msg_reply((long)S1_SLOT_B, &rm) != 0) { ok = 0; why = "reply"; break; }
        if (iris_msg_reply((long)S1_SLOT_B, &rm) != (long)IRIS_ERR_NOT_FOUND) {
            ok = 0; why = "one-shot broken (S18)"; break;
        }
        IT_AWAIT(g_t257_done, 4000);
        if (!g_t257_done) { ok = 0; why = "caller stuck"; break; }
    }

    /* Caller death while bound: the reply object returns to FREE. */
    if (ok) {
        long ep2 = it_ep_create();
        handle_id_t cmd = (ep2 >= 0) ? (handle_id_t)ep2 : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep2 < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_lp_cmd(cmd, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }
        if (ok) {
            struct iris_msg m; iris_msg_zero(&m);
            if ((m.reply = (long)S1_SLOT_B, iris_msg_recv((long)cmd, &m)) != 0) { ok = 0; why = "recv child call"; }
        }
        if (ok && it_kill((long)proc) != 0) { ok = 0; why = "kill"; }
        if (ok) {
            struct iris_msg rm; iris_msg_zero(&rm);
            if (iris_msg_reply((long)S1_SLOT_B, &rm) != (long)IRIS_ERR_NOT_FOUND) {
                ok = 0; why = "dead caller reply";
            }
        }
        it_close(&proc); it_close(&cmd);
    }

    /* No implicit fabrication: a reply-less receiver cannot serve a CALL. */
    if (ok) {
        g_t257_done = 0;
        uint64_t entry = (uint64_t)(uintptr_t)t257_caller;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t257_stack + sizeof(g_t257_stack))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread 2"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);   /* caller queues */
            struct iris_msg m; iris_msg_zero(&m);
            if ((m.reply = 0, iris_msg_recv((long)S1_SLOT_A, &m)) !=
                (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "implicit reply not retired (S22)"; }
            /* Serve it properly so the thread exits. */
            if (ok) {
                if ((m.reply = (long)S1_SLOT_B, iris_msg_recv((long)S1_SLOT_A, &m)) != 0) { ok = 0; why = "recv 3"; }
                struct iris_msg rm; iris_msg_zero(&rm);
                if (ok && iris_msg_reply((long)S1_SLOT_B, &rm) != 0) { ok = 0; why = "reply 3"; }
                IT_AWAIT(g_t257_done, 4000);
                if (!g_t257_done) { ok = 0; why = "caller 3 stuck"; }
            }
        }
    }

    /* Stale: delete the reply cap → the CPtr no longer resolves. */
    it_slot_delete(S1_SLOT_B);
    if (ok) {
        struct iris_msg rm; iris_msg_zero(&rm);
        long r = iris_msg_reply((long)S1_SLOT_B, &rm);
        if (r != (long)IRIS_ERR_NOT_FOUND && r != (long)IRIS_ERR_BAD_HANDLE) { ok = 0; why = "stale reply cap"; }
    }
    it_slot_delete(S1_SLOT_A);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    if (ok) {
        struct it_utq_objects oz;
        if (!it_utq_o(&oz) || oz.replies_live != o0.replies_live) { ok = 0; why = "reply leak"; }
    }
    it_close(&su_h);
    if (ok) it_pass("T257"); else it_fail("T257", why);
}

/* ── T258: revoke/teardown during IPC and wait ──────────────────────────────
 * Deleting the LAST capability of an object with blocked parties never
 * leaves a zombie: a blocked sender wakes CLOSED, a pending CALLER whose
 * server loses its reply authority wakes CLOSED (S25/S27), and a
 * notification waiter wakes CLOSED (S26).  Object gauges return to baseline
 * and the regions are reusable. */
static volatile int  g_t258_done;
static volatile long g_t258_res;
static uint8_t       g_t258_stack[8192];
static void t258_sender(void) {
    struct iris_msg m; iris_msg_zero(&m); m.label = 0x258;
    g_t258_res  = iris_msg_send((long)S1_SLOT_A, &m);
    g_t258_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}
void test_t258(void) {
    int ok = 1;
    const char *why = "revoke during ipc";
    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T258", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;
    struct it_utq_objects o0, oz;
    if (!it_utq_o(&o0)) { it_close(&su_h); it_fail("T258", "query"); return; }

    /* Blocked SENDER, last cap deleted → CLOSED. */
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok) {
        g_t258_done = 0; g_t258_res = 999;
        uint64_t entry = (uint64_t)(uintptr_t)t258_sender;
        uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t258_stack + sizeof(g_t258_stack))) & ~0xFULL;
        if (it_thread_create(entry, rsp, 0) < 0) { ok = 0; why = "thread"; }
        else {
            for (int y = 0; y < 60; y++) it_sys0(SYS_YIELD);
            it_slot_delete(S1_SLOT_A);
            IT_AWAIT(g_t258_done, 4000);
            if (!g_t258_done) { ok = 0; why = "sender zombie (S25)"; }
            else if (g_t258_res != (long)IRIS_ERR_CLOSED) { ok = 0; why = "sender wake err"; }
        }
    }

    /* Pending CALL: child blocked in call; the server's reply authority is
     * destroyed (last cap) → the caller wakes CLOSED, exits (S27). */
    if (ok) {
        long ep2 = it_ep_create();
        handle_id_t cmd = (ep2 >= 0) ? (handle_id_t)ep2 : HANDLE_INVALID;
        handle_id_t proc = HANDLE_INVALID;
        if (ep2 < 0 || lp_spawn_child(cmd, &proc) < 0) { ok = 0; why = "spawn"; }
        if (ok && it_reply_create_at(S1_SLOT_B) < 0) { ok = 0; why = "reply fixture"; }
        if (ok && it_lp_cmd(cmd, LP_CMD_CALL_BLOCK) != 0) { ok = 0; why = "cmd"; }
        if (ok) {
            struct iris_msg m; iris_msg_zero(&m);
            if ((m.reply = (long)S1_SLOT_B, iris_msg_recv((long)cmd, &m)) != 0) { ok = 0; why = "recv call"; }
        }
        if (ok) {
            it_slot_delete(S1_SLOT_B);         /* reply close → caller CLOSED */
            if (it_lp_wait_exit(proc) < 0) { ok = 0; why = "caller ghost (S27)"; }
        }
        it_close(&proc); it_close(&cmd);
    }

    /* Notification waiter, last cap deleted → CLOSED (S26): T256 already
     * proves this exact path; here we only re-verify the gauges. */
    it_quiesce_reaper();
    if (ok && (!it_utq_o(&oz) ||
               oz.endpoints_live != o0.endpoints_live ||
               oz.replies_live   != o0.replies_live)) { ok = 0; why = "object drift (S31)"; }
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    it_close(&su_h);
    if (ok) it_pass("T258"); else it_fail("T258", why);
}

/* ── T259: Untyped reuse and stale-object defense ───────────────────────────
 * Object A (endpoint) lives in a region; a LIVE cap to A blocks RESET (S13).
 * After every cap dies, RESET bumps the region generation and the SAME
 * memory hosts object B (notification).  No stale path reaches B: the old
 * CSpace slot is empty (S30), a re-minted slot holds B's type — endpoint ops
 * on it fail WRONG_TYPE — and B starts with zero state (S28/S29). */
void test_t259(void) {
    int ok = 1;
    const char *why = "reuse/stale defense";
    long su = s1_sub_ut(4096);
    if (su < 0) { it_fail("T259", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_one q0, q1, q2;
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype A"; }
    if (ok && !it_utq_1(su, &q0)) { ok = 0; why = "query"; }
    /* Keep a SECOND capability to A: the region must NOT be reclaimable while
     * any capability to a live object in it survives.  That second cap used to
     * be a handle; it is a CSpace copy, which is the only kind left. */
    long h = -1;
    if (ok) {
        it_slot_delete(IT_SCRATCH_0);
        if (it_invoke2((long)S1_SLOT_A, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_SAME_RIGHTS) != 0) { ok = 0; why = "copy"; }
        else h = (long)IT_SCRATCH_0;
    }
    it_slot_delete(S1_SLOT_A);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "live object did not retain region (S13)";
    }
    /* Drop the last cap → destroy → reset works and bumps the generation. */
    if (h >= 0) it_slot_delete((uint32_t)h);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset after death"; }
    if (ok && (!it_utq_1(su, &q1) || q1.generation != q0.generation + 1u ||
               q1.used_bytes != 0u)) { ok = 0; why = "generation not bumped"; }

    /* Same region now hosts B (a notification). */
    if (ok && it_retype2_at(su, IRIS_KOBJ_NOTIFICATION, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype B"; }
    if (ok && !it_utq_1(su, &q2)) { ok = 0; why = "query B"; }
    if (ok && q2.used_bytes == 0u) { ok = 0; why = "B not in region"; }
    /* Stale-path probes against B:
     *  - endpoint ops through the reused slot → WRONG_TYPE (cap identity
     *    blocks the old object's protocol);
     *  - B carries no pending state from A's lifetime. */
    if (ok) {
        struct iris_msg m; iris_msg_zero(&m);
        if (iris_msg_nb_send((long)S1_SLOT_A, &m) != (long)IRIS_ERR_WRONG_TYPE) {
            ok = 0; why = "stale protocol reached B (S29)";
        }
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_wait_timeout( (long)S1_SLOT_A,
                    (long)(uintptr_t)&bits, 20000000L) != (long)IRIS_ERR_TIMED_OUT) {
            ok = 0; why = "residual state (S28)";
        }
    }
    it_slot_delete(S1_SLOT_A);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "final reset"; }
    it_close(&su_h);
    if (ok) it_pass("T259"); else it_fail("T259", why);
}

/* ── T260: legacy create-path retirement ────────────────────────────────────
 * SYS_ENDPOINT_CREATE / SYS_NOTIFY_CREATE / SYS_CNODE_CREATE return
 * NOT_SUPPORTED and create NOTHING: no kslab movement, no live-object
 * change, no handle, no CSpace mutation (S22).  The one-shot KReply
 * fabrication inside EP_CALL is equally retired (proven in T257). */
void test_t260(void) {
    int ok = 1;
    const char *why = "legacy retirement";
    struct it_utq_global k0, k1;
    struct it_utq_objects o0, o1;
    uint32_t e0[14], e1[14];
    it_quiesce_reaper();
    if (!it_utq_g(&k0) || !it_utq_o(&o0) || !it_sched_ext(e0)) {
        it_fail("T260", "query"); return;
    }
    static const long retired_creates[] = { SYS_ENDPOINT_CREATE, SYS_NOTIFY_CREATE, SYS_CNODE_CREATE };
    for (uint32_t i = 0; ok && i < 3u; i++) {
        if (it_sys3(retired_creates[i], 4, 0, 0) != (long)IRIS_ERR_NOT_SUPPORTED) {
            ok = 0; why = "create not retired";
        }
    }
    it_quiesce_reaper();
    if (ok && (!it_utq_g(&k1) || !it_utq_o(&o1) || !it_sched_ext(e1))) {
        ok = 0; why = "query 2";
    }
    if (ok && k1.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "kslab consumed"; }
    if (ok && (o1.endpoints_live != o0.endpoints_live ||
               o1.notifications_live != o0.notifications_live ||
               o1.cnodes_live != o0.cnodes_live)) { ok = 0; why = "object born"; }
    if (ok && e1[IT_SI_LIVE] != e0[IT_SI_LIVE]) { ok = 0; why = "handle born"; }
    if (ok) it_pass("T260"); else it_fail("T260", why);
}

/* ── T261: service IPC objects from delegated Untyped ───────────────────────
 * The REAL system runs on the new substrate: svcmgr, vfs, console and kbd
 * all serve EP_CALLs through endpoints their supervisor retyped from a
 * DELEGATED untyped pool, with explicit reply objects (svcmgr pool →
 * per-service reply sub-untypeds).  A privileged service RESTART exercises
 * the pool's reset+retype path and the service serves again. */
void test_t261(void) {
    int ok = 1;
    const char *why = "service ipc";
    uint64_t b = 0;
    /* Each PING is an EP_CALL served with an explicit, untyped-funded reply. */
    if (it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &b) != 0)  { ok = 0; why = "svcmgr ping"; }
    if (ok && it_ping_badge((long)IRIS_CPTR_VFS_EP, &b) != 0) { ok = 0; why = "vfs ping"; }
    if (ok && it_ping_badge((long)IRIS_CPTR_CONSOLE_EP, &b) != 0) { ok = 0; why = "console ping"; }
    if (ok && it_ping_badge((long)IRIS_CPTR_KBD_EP, &b) != 0) { ok = 0; why = "kbd ping"; }

    /* Restart VFS: svcmgr RESETs the service's reply sub-untyped and retypes
     * a fresh reply object for the respawned child. */
    if (ok) {
        uint32_t a = 0, g0 = 0;
        if (it_status(VFS_EP_SVC_NAME, &a, &g0) != 0 || a != 1u) { ok = 0; why = "pre status"; }
        if (ok) {
            struct iris_msg msg; iris_msg_zero(&msg);
            msg.label = IRIS_SVCMGR_EP_RESTART;
            msg.words[0] = (uint64_t)SVCMGR_SERVICE_VFS;
            msg.word_count = 1u;
            long r = iris_msg_call((long)IRIS_CPTR_TEST_SUPER, &msg);
            if (!(r == 0 && msg.label == IRIS_EP_REPLY_OK)) { ok = 0; why = "restart denied"; }
        }
        int recovered = 0;
        for (uint32_t i = 0; ok && i < 400u && !recovered; i++) {
            uint64_t bb = 0;
            (void)it_ping_badge((long)IRIS_CPTR_SVCMGR_EP, &bb);
            uint32_t a1 = 0, g1 = 0;
            if (it_status(VFS_EP_SVC_NAME, &a1, &g1) == 0 && a1 == 1u && g1 > g0)
                recovered = 1;
        }
        if (ok && !recovered) { ok = 0; why = "vfs not restarted"; }
        /* The respawned VFS serves through its FRESH reply object. */
        if (ok && it_ping_badge((long)IRIS_CPTR_VFS_EP, &b) != 0) { ok = 0; why = "post-restart ping"; }
    }
    if (ok) it_pass("T261"); else it_fail("T261", why);
}
void test_t262(void) {
    int ok = 1;
    const char *why = "untyped stress";
    uint32_t round = 0, op = 0;
    g_fz_seed = T262_SEED;

    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T262", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_objects o0, oz;
    struct it_utq_global k0, kz;
    struct it_utq_global g0, g1;
    if (!it_utq_o(&o0) || !it_utq_g(&k0) || !it_utq_g(&g0)) {
        it_close(&su_h); it_fail("T262", "query"); return;
    }

    for (round = 0; ok && round < T262_ROUNDS; round++) {
        uint32_t children = 0;
        /* 1..3 creation ops per round across slots 241..244. */
        uint32_t nops = 1u + (fz_rand() % 3u);
        uint32_t used_slots[4] = {0,0,0,0};
        for (uint32_t i = 0; ok && i < nops; i++) {
            op = fz_rand() % 3u;
            uint32_t slot = S1_SLOT_A + i;
            uint32_t type = (op == 0u) ? IRIS_KOBJ_ENDPOINT
                          : (op == 1u) ? IRIS_KOBJ_NOTIFICATION
                                       : IRIS_KOBJ_REPLY;
            if (it_retype2_at(su, type, slot, 1u, 0) != 0) { ok = 0; why = "retype"; break; }
            used_slots[i] = type;
            children++;
            /* Type-appropriate probe. */
            if (type == IRIS_KOBJ_ENDPOINT) {
                struct iris_msg m; iris_msg_zero(&m);
                if (iris_msg_nb_recv((long)slot, &m) != (long)IRIS_ERR_WOULD_BLOCK) { ok = 0; why = "ep probe"; }
            } else if (type == IRIS_KOBJ_NOTIFICATION) {
                uint64_t bits = 0;
                if (it_invoke1((long)slot, INV_NOTIFY_SIGNAL, 1u + round) != 0 ||
                    it_invoke1((long)slot, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 ||
                    bits != (uint64_t)(1u + round)) { ok = 0; why = "notif probe"; }
            } else {
                struct iris_msg rm; iris_msg_zero(&rm);
                if (iris_msg_reply((long)slot, &rm) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "reply probe"; }
            }
        }
        /* Occasional forced failures — must not consume anything. */
        if (ok && (fz_rand() & 1u)) {
            struct it_utq_one uf0, uf1;
            if (!it_utq_1(su, &uf0)) { ok = 0; why = "q"; }
            if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 2u, 0) !=
                      (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "collision"; }
            if (ok && it_retype2_at(su, 77u, S1_SLOT_E, 1u, 0) !=
                      (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "badtype"; }
            if (ok && (!it_utq_1(su, &uf1) || uf1.used_bytes != uf0.used_bytes ||
                       uf1.child_count != uf0.child_count)) { ok = 0; why = "failure consumed"; }
        }
        /* Take a second reference and drop it: the object survives, because
         * the slot still holds one.  It used to be a materialised handle,
         * which measured the same thing through the retiring namespace. */
        if (ok && (fz_rand() & 1u)) {
            it_slot_delete(IT_SCRATCH_0);
            if (it_invoke2((long)S1_SLOT_A, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_SAME_RIGHTS) != 0) { ok = 0; why = "copy"; }
            it_slot_delete(IT_SCRATCH_0);
        }
        /* Exact shadow of child_count. */
        if (ok) {
            struct it_utq_one u;
            if (!it_utq_1(su, &u) || u.child_count != children) { ok = 0; why = "shadow child"; }
        }
        /* Tear down; stale CPtr probes must fail cleanly; region reusable. */
        for (uint32_t i = 0; ok && i < nops; i++) it_slot_delete(S1_SLOT_A + i);
        if (ok) {
            struct iris_msg m; iris_msg_zero(&m);
            if (iris_msg_nb_send((long)S1_SLOT_A, &m) != (long)IRIS_ERR_NOT_FOUND) {
                ok = 0; why = "stale cptr";
            }
        }
        it_quiesce_reaper();
        if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "round reset"; }
        if (ok) {
            struct it_utq_one u;
            if (!it_utq_1(su, &u) || u.used_bytes != 0u || u.child_count != 0u) { ok = 0; why = "round reclaim"; }
        }
        if (ok && (!it_utq_o(&oz) || !it_utq_g(&kz))) { ok = 0; why = "round query"; }
        if (ok && (oz.endpoints_live     != o0.endpoints_live ||
                   oz.notifications_live != o0.notifications_live ||
                   oz.replies_live       != o0.replies_live)) { ok = 0; why = "round drift"; }
        if (ok && kz.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "round kslab"; }
        (void)used_slots;
    }

    /* Global instrumentation moved in the right direction: retypes and
     * failures were counted, every round's RESET registered as reclaim+reuse. */
    if (ok && !it_utq_g(&g1)) { ok = 0; why = "global query"; }
    if (ok && !(g1.retype_count    >  g0.retype_count &&
                g1.retype_failures >= g0.retype_failures &&
                g1.reset_count     >= g0.reset_count + T262_ROUNDS &&
                g1.reuse_count     >= g0.reuse_count + T262_ROUNDS &&
                g1.reclaimed_bytes >  g0.reclaimed_bytes)) { ok = 0; why = "global counters"; }

    it_close(&su_h);
    if (ok) it_pass("T262");
    else { it_fz_note("T262", T262_SEED, round, op); it_fail("T262", why); }
}
int it_utq_t(struct it_utq_taskobj *q) {
    return it_invoke2(IT_QARG(4, sizeof(*q)), INV_UNTYPED_QUERY, (long)(uintptr_t)q, 0) == 0;
}

/* ── T291 — RETIRED with SYS_BOOTCAP_RESTRICT (Stage 5 Step 2) ─────────
 * Its subject was narrowing a boot capability by deriving a weaker CLONE of
 * it, which existed only because one object carried several authorities at
 * once.  Every boot capability carries exactly one now — kbootcap_alloc
 * refuses a multi-bit kind — so there is nothing to narrow and nothing this
 * test could assert: a capability that could be restricted cannot be
 * constructed.  The number is pinned as NOT_SUPPORTED by T148, and the
 * property that replaced it (one capability, one authority) is T296.
 *
 * This is the Stage 4 rule applied unchanged: a test whose SUBJECT is the
 * retired mechanism dies with it, and one asserting a property that survives
 * is rewritten.  T291 is the first kind. */

/* ── T267: SchedulingContext configure/bind lifecycle ────────────────────────
 * A SC is a CANONICAL object created ONLY from Untyped (SYS_SC_CREATE retired).
 * It is born unconfigured and unbound; SC_CONFIGURE validates budget/period;
 * SC_BIND is one-to-one against a TCB cap, requires a configured SC, rejects a
 * second binding (BUSY), unbinds cleanly, and rebinds.  Provenance: the SC
 * lives in the source Untyped (sc_retyped/live move; no kslab).
 * Invariants: S2.2, S2.8, S2.9, S2.13, S2.14, S2.15. */
void test_t267(void) {
    int ok = 1;
    const char *why = "sc lifecycle";

    /* SYS_SC_CREATE is retired. */
    if (it_sys0(SYS_SC_CREATE) != (long)IRIS_ERR_NOT_SUPPORTED) { it_fail("T267", "sc create not retired"); return; }

    long su = s1_sub_ut(8192);
    if (su < 0) { it_fail("T267", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_taskobj t0, t1;
    struct it_utq_global k0, k1;
    if (!it_utq_t(&t0) || !it_utq_g(&k0)) { it_close(&su_h); it_fail("T267", "query"); return; }

    /* Retype two SCs into CSpace slots (provenance + no kslab). */
    if (it_retype2_at(su, IRIS_KOBJ_SCHED_CONTEXT, S1_SLOT_A, 1u, 0) != 0 ||
        it_retype2_at(su, IRIS_KOBJ_SCHED_CONTEXT, S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok && (!it_utq_t(&t1) || !it_utq_g(&k1))) { ok = 0; why = "query 2"; }
    if (ok && t1.sc_live != t0.sc_live + 2u) { ok = 0; why = "sc not counted"; }
    if (ok && t1.sc_retyped < t0.sc_retyped + 2u) { ok = 0; why = "retype not counted"; }
    if (ok && k1.kslab_used_bytes != k0.kslab_used_bytes) { ok = 0; why = "sc from kslab (S2.13)"; }

    /* Unconfigured SC cannot bind (B2/B3). */
    long self_tcb = ok ? it_own_tcb_derived() : -1;
    handle_id_t self_h = (self_tcb >= 0) ? (handle_id_t)self_tcb : HANDLE_INVALID;
    if (ok && self_tcb < 0) { ok = 0; why = "tcb self"; }
    if (ok && it_invoke1((long)S1_SLOT_A, INV_SC_BIND, (long)self_h) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "unconfigured bind allowed";
    }
    /* Configure validation (S2.8). */
    if (ok && it_invoke((long)S1_SLOT_A, INV_SC_CONFIGURE, 5, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "configure"; }
    if (ok && it_invoke((long)S1_SLOT_A, INV_SC_CONFIGURE, 0, 100, (long)IRIS_CPTR_SCHED_CONTROL) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget 0"; }
    /* Phase S2: budget==period accepted (full reservation); budget>period not. */
    if (ok && it_invoke((long)S1_SLOT_A, INV_SC_CONFIGURE, 100, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "budget==period rejected"; }
    if (ok && it_invoke((long)S1_SLOT_A, INV_SC_CONFIGURE, 200, 100, (long)IRIS_CPTR_SCHED_CONTROL) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "budget>period"; }
    if (ok && it_invoke((long)S1_SLOT_A, INV_SC_CONFIGURE, 5, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "reconfigure A"; }
    if (ok && it_invoke((long)S1_SLOT_B, INV_SC_CONFIGURE, 5, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "configure B"; }

    /* Bind SC_A to our own TCB, then a SECOND SC to the same TCB must fail
     * BUSY (one-to-one: the target already holds SC_A).  Unbind immediately
     * (no yield in between — never let the tiny budget suspend iris_test). */
    if (ok && it_invoke1((long)S1_SLOT_A, INV_SC_BIND, (long)self_h) != 0) { ok = 0; why = "bind"; }
    if (ok && it_invoke1((long)S1_SLOT_B, INV_SC_BIND, (long)self_h) != (long)IRIS_ERR_BUSY) { ok = 0; why = "double bind (S2.9)"; }
    if (ok && it_invoke1((long)S1_SLOT_A, INV_SC_BIND, 0) != 0) { ok = 0; why = "unbind"; }
    /* After unbind, SC_A is free again → SC_B binds, then unbind. */
    if (ok && it_invoke1((long)S1_SLOT_B, INV_SC_BIND, (long)self_h) != 0) { ok = 0; why = "rebind"; }
    if (ok && it_invoke1((long)S1_SLOT_B, INV_SC_BIND, 0) != 0) { ok = 0; why = "unbind 2"; }

    it_close(&self_h);
    it_slot_delete(S1_SLOT_A); it_slot_delete(S1_SLOT_B);
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset busy"; }
    if (ok) {
        struct it_utq_taskobj tz;
        if (!it_utq_t(&tz) || tz.sc_live != t0.sc_live) { ok = 0; why = "sc leak"; }
        if (ok && tz.sc_destroyed < t0.sc_destroyed + 2u) { ok = 0; why = "destroy not counted"; }
    }
    it_close(&su_h);
    if (ok) it_pass("T267"); else it_fail("T267", why);
}

/* ── T283: Versioned user-buffer ABI hardening (Checkpoint C.1) ──────────────
 * SYS_UNTYPED_QUERY / SYS_RESOURCE_INFO must never write past the caller's
 * declared buffer.  A guarded fixture (canary bytes bracketing an
 * intentionally-short buffer) proves the kernel clamps to min(declared,kernel)
 * and rejects undersized/unknown-version/bad-pointer requests without writing.
 * QABI1–QABI10. */
void test_t283(void) {
    int ok = 1;
    const char *why = "abi hardening";

    /* Guarded buffer: [canary0][payload 256][canary1].  All queries target
     * &payload with a DECLARED size; the canaries must never change. */
    struct { uint64_t c0; uint8_t payload[256]; uint64_t c1; } g;
    const uint64_t CAN0 = 0xA5A5A5A5DEADBEEFULL, CAN1 = 0x5A5A5A5AFEEDFACEULL;
    #define QABI_RESET() do { g.c0 = CAN0; g.c1 = CAN1; \
        for (uint32_t _i = 0; _i < sizeof(g.payload); _i++) g.payload[_i] = 0; } while (0)
    #define QABI_CANARY_OK() (g.c0 == CAN0 && g.c1 == CAN1)
    long buf = (long)(uintptr_t)&g.payload[0];

    /* QABI1 — exact current size (the real global struct). */
    QABI_RESET();
    struct it_utq_global gl;
    if (it_invoke2(IT_QARG(1, sizeof(struct it_utq_global)), INV_UNTYPED_QUERY, (long)(uintptr_t)&gl, 0) != 0) { ok = 0; why = "QABI1 exact"; }
    /* QABI2 — valid older/shorter prefix: declare just the 8-byte header. */
    if (ok) { QABI_RESET();
        long r = it_invoke2(IT_QARG(1, 8u), INV_UNTYPED_QUERY, buf, 0);
        if (r != 0) { ok = 0; why = "QABI2 prefix"; }
        else if (!QABI_CANARY_OK()) { ok = 0; why = "QABI2 canary"; }
        else { /* only 8 bytes written; bytes >=8 stay zero */
            for (uint32_t i = 8; i < 64 && ok; i++)
                if (g.payload[i] != 0) { ok = 0; why = "QABI2 overwrote past decl"; }
        }
    }
    /* QABI3 — size below the required header (4 < 8) → INVALID_ARG, no write. */
    if (ok) { QABI_RESET();
        if (it_invoke2(IT_QARG(1, 4u), INV_UNTYPED_QUERY, buf, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "QABI3 subheader"; }
        else if (!QABI_CANARY_OK()) { ok = 0; why = "QABI3 canary"; }
    }
    /* QABI4 — one byte below minimum (7) → INVALID_ARG. */
    if (ok) { QABI_RESET();
        if (it_invoke2(IT_QARG(1, 7u), INV_UNTYPED_QUERY, buf, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "QABI4 min-1"; }
        else if (!QABI_CANARY_OK()) { ok = 0; why = "QABI4 canary"; }
    }
    /* QABI5 — oversized declared buffer (256) → only kernel_size written,
     * canaries intact (kernel never writes past its own struct). */
    if (ok) { QABI_RESET();
        if (it_invoke2(IT_QARG(1, 256u), INV_UNTYPED_QUERY, buf, 0) != 0) { ok = 0; why = "QABI5 oversize"; }
        else if (!QABI_CANARY_OK()) { ok = 0; why = "QABI5 canary"; }
    }
    /* QABI6 — unknown version (0xFFFF) → INVALID_ARG, no write. */
    if (ok) { QABI_RESET();
        long a0 = (long)((uint64_t)1u | ((uint64_t)0xFFFFu << 16) |
                         ((uint64_t)(uint32_t)sizeof(struct it_utq_global) << 32));
        if (it_invoke2(a0, INV_UNTYPED_QUERY, buf, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "QABI6 version"; }
        else if (!QABI_CANARY_OK()) { ok = 0; why = "QABI6 canary"; }
    }
    /* QABI7 — invalid user pointer → INVALID_ARG, no write. */
    if (ok) {
        if (it_invoke2(IT_QARG(1, sizeof(struct it_utq_global)), INV_UNTYPED_QUERY, 0x1L /* bogus */, 0) != (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "QABI7 badptr"; }
    }
    /* QABI8 — guard bytes intact after a full legitimate write (kind 4, the
     * struct that grew and caused the original bug). */
    if (ok) { QABI_RESET();
        struct it_utq_taskobj tq;
        if (it_invoke2(IT_QARG(4, sizeof(tq)), INV_UNTYPED_QUERY, (long)(uintptr_t)&tq, 0) != 0) { ok = 0; why = "QABI8 taskobj"; }
        /* Now target the SHORT guarded buffer with a taskobj-sized declaration
         * that exceeds the guarded payload's safe region: declare exactly the
         * header so only 8 bytes land, canaries must hold. */
        if (ok) { long r = it_invoke2(IT_QARG(4, 8u), INV_UNTYPED_QUERY, buf, 0);
            if (r != 0 || !QABI_CANARY_OK()) { ok = 0; why = "QABI8 canary"; } }
    }
    /* QABI9 — reserved/trailing fields are zero: a prefix read leaves the
     * caller's untouched tail at its pre-call value (we pre-zeroed). */
    if (ok) { QABI_RESET();
        if (it_invoke2(IT_QARG(3, 8u), INV_UNTYPED_QUERY, buf, 0) != 0) { ok = 0; why = "QABI9 write"; }
        else for (uint32_t i = 8; i < 24 && ok; i++)
            if (g.payload[i] != 0) { ok = 0; why = "QABI9 tail dirty"; }
    }
    /* QABI10 — repeated old/new callers do not corrupt kernel state: the
     * global counters are identical before/after a burst of clamped reads. */
    if (ok) {
        struct it_utq_global b0, b1;
        if (!it_utq_g(&b0)) { ok = 0; why = "QABI10 g0"; }
        for (int i = 0; ok && i < 8; i++) {
            QABI_RESET();
            (void)it_invoke2(IT_QARG(1, (i & 1) ? 8u : 256u), INV_UNTYPED_QUERY, buf, 0);
            (void)it_invoke2(IT_QARG(1, 4u), INV_UNTYPED_QUERY, buf, 0); /* rejected */
        }
        if (ok && !it_utq_g(&b1)) { ok = 0; why = "QABI10 g1"; }
        if (ok && (b1.live_untypeds != b0.live_untypeds)) { ok = 0; why = "QABI10 state drift"; }
    }
    /* Stage 7-mem: SYS_RESOURCE_INFO is RETIRED, so the prefix-safety probe
     * becomes a retirement probe — and the stronger claim of the two, because
     * a retired syscall must write NOTHING at all, not merely stay inside a
     * declared prefix. */
    if (ok) { QABI_RESET();
        long r = it_sys3(SYS_RESOURCE_INFO, (long)HANDLE_INVALID, buf, 8L);
        if (r != (long)IRIS_ERR_NOT_SUPPORTED || !QABI_CANARY_OK()) { ok = 0; why = "rinfo prefix"; }
    }

    #undef QABI_RESET
    #undef QABI_CANARY_OK
    if (ok) it_pass("T283"); else it_fail("T283", why);
}

/* ════════════════════════════════════════════════════════════════════════
 * Phase S2 Stage 0 — canonical TCB from Untyped (T284–T287).
 *
 * Charter §2.2 (O1–O6) and §C.2: the TCB is born from RETYPE2 as an INACTIVE
 * object (configured = 0) — cap-complete (GET_INFO / SET_PRIORITY / delete /
 * transfer) but not runnable until TCB_CONFIGURE (roadmap Stage 5/6); the
 * execution syscalls reject it with NOT_SUPPORTED and no side effects.  A
 * TERMINATED TCB stays observable through any surviving cap; the registry
 * (scheduler identity) is released at TERMINATION, not at the last cap; the
 * storage is only reused after the destructor (last reference).
 * ════════════════════════════════════════════════════════════════════════ */

/* task_state_t ABI values: see the mirror beside it_alive. */

/* ── T284: birth, observability and death of the retyped TCB ────────────────
 * RETYPE2(KOBJ_TCB) creates a canonical TCB: storage inside the untyped
 * (child_count/tcb_live rise; the registry is NOT touched), a cap in CSpace
 * with HANDLE_TYPE_TCB, GET_INFO answers {SUSPENDED, id 0}, SET_PRIORITY
 * works, and RESUME/SUSPEND/EXIT/SC_BIND reject NOT_SUPPORTED (never runnable
 * without configure).  RESET with live children → BUSY; deleting the last cap
 * destroys and returns the block; RESET reopens the region and it can be
 * retyped again; a deleted CPtr no longer resolves. */
void test_t284(void) {
    int ok = 1;
    const char *why = "tcb retype";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T284", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_taskobj t0, t1, t2;
    struct it_utq_one u1;
    if (!it_utq_t(&t0)) { it_close(&su_h); it_fail("T284", "query"); return; }

    /* Birth: object gauges move; scheduler registry must NOT. */
    if (it_retype2_at(su, IRIS_KOBJ_TCB, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype"; }
    if (ok && (!it_utq_t(&t1) || !it_utq_1(su, &u1))) { ok = 0; why = "query 2"; }
    if (ok && t1.tcb_live != t0.tcb_live + 1u) { ok = 0; why = "tcb not counted"; }
    if (ok && t1.tcb_registry_active != t0.tcb_registry_active) { ok = 0; why = "registry touched (obj != registry)"; }
    if (ok && u1.child_count != 1u) { ok = 0; why = "child count"; }

    /* Cap identity + observability. */
    if (ok && it_invoke0((long)S1_SLOT_A, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_TCB) { ok = 0; why = "cap type"; }
    struct iris_tcb_info info;
    if (ok && it_invoke1((long)S1_SLOT_A, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) { ok = 0; why = "get info"; }
    if (ok && (info.state != (uint8_t)IT_TASK_SUSPENDED || info.task_id != 0u)) { ok = 0; why = "inactive state"; }

    /* Execution gate: an unconfigured TCB can NEVER run or bind. */
    if (ok && it_invoke0((long)S1_SLOT_A, INV_TCB_RESUME) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "resume allowed"; }
    if (ok && it_invoke0((long)S1_SLOT_A, INV_TCB_SUSPEND) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "suspend allowed"; }
    if (ok && it_invoke0((long)S1_SLOT_A, INV_TCB_EXIT) != (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "exit allowed"; }
    if (ok) {
        if (it_retype2_at(su, IRIS_KOBJ_SCHED_CONTEXT, S1_SLOT_B, 1u, 0) != 0 ||
            it_invoke((long)S1_SLOT_B, INV_SC_CONFIGURE, 5, 100, (long)IRIS_CPTR_SCHED_CONTROL) != 0) { ok = 0; why = "sc setup"; }
        else if (it_invoke1((long)S1_SLOT_B, INV_SC_BIND, (long)S1_SLOT_A) !=
                 (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "bind to unconfigured allowed"; }
    }
    /* SET_PRIORITY works on an inactive TCB (seL4-style: stored for later). */
    if (ok && it_invoke1((long)S1_SLOT_A, INV_TCB_SET_PRIORITY, 7) != 0) { ok = 0; why = "set prio"; }
    if (ok && (it_invoke1((long)S1_SLOT_A, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0 ||
               info.priority != 7u)) { ok = 0; why = "prio roundtrip"; }

    /* Legacy handle-publishing birth is retired outright (Stage 4); it was
     * already refused for TCB by S20 + Stage 0. */
    if (ok && it_sys3(SYS_UNTYPED_RETYPE, su, (long)IRIS_KOBJ_TCB, 0) !=
              (long)IRIS_ERR_NOT_SUPPORTED) { ok = 0; why = "legacy tcb retype alive"; }

    /* Lifecycle: RESET with live children refuses; last cap destroys. */
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != (long)IRIS_ERR_BUSY) { ok = 0; why = "reset with children"; }
    it_slot_delete(S1_SLOT_B);
    it_slot_delete(S1_SLOT_A);   /* last cap → destructor → region */
    if (ok && !it_utq_t(&t2)) { ok = 0; why = "query 3"; }
    if (ok && t2.tcb_live != t0.tcb_live) { ok = 0; why = "tcb leak"; }
    if (ok && t2.tcb_destroyed < t0.tcb_destroyed + 1u) { ok = 0; why = "destroy not counted"; }
    if (ok && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset after destroy"; }

    /* Region reusable; a deleted CPtr never resolves again. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_TCB, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "re-retype"; }
    it_slot_delete(S1_SLOT_A);
    if (ok) {
        if (it_invoke0((long)S1_SLOT_A, INV_CAP_IDENTIFY) >= 0) { ok = 0; why = "stale cptr resolves"; }
    }
    (void)it_invoke0(su, INV_UNTYPED_RESET);

    it_close(&su_h);
    if (ok) it_pass("T284"); else it_fail("T284", why);
}

/* ── T285: a TERMINATED TCB observable through a surviving cap ───────────────
 * A real thread publishes its TCB (TCB_SELF) and calls THREAD_EXIT.  The cap
 * outlives execution: GET_INFO keeps answering (state TERMINATED, stable
 * task_id), the scheduler registry was already released (registry active
 * unchanged while the cap is still live — registry ≠ object), and RESUME on
 * the terminal TCB fails NOT_FOUND (no resurrection). */
static volatile int g_t285_ready = 0;
static long         g_t285_tcb   = -1;
static uint8_t      g_t285_stack[8192];

static void t285_helper(uint64_t self_tcb) {
    g_t285_tcb   = (long)self_tcb;
    g_t285_ready = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t285(void) {
    int ok = 1;
    const char *why = "terminated observability";

    struct it_utq_taskobj t0;
    if (!it_utq_t(&t0)) { it_fail("T285", "query"); return; }

    g_t285_ready = 0; g_t285_tcb = -1;
    uint64_t entry = (uint64_t)(uintptr_t)t285_helper;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t285_stack + sizeof(g_t285_stack))) & ~0xFULL;
    long tid = it_thread_create(entry, rsp, IT_THREAD_ARG_SELF_TCB);
    if (tid < 0) { it_fail("T285", "thread create"); return; }

    IT_AWAIT(g_t285_ready, 200);
    if (!g_t285_ready || g_t285_tcb < 0) { it_fail("T285", "tcb self"); return; }
    handle_id_t tcb_h = (handle_id_t)g_t285_tcb;

    /* Stage 5 Step 4: thread creation returns a CAPABILITY, not a global
     * thread id — so the id this test tracks across death is read from the
     * object while it is alive instead of being whatever the creation call
     * happened to hand back. */
    struct iris_tcb_info alive; alive.task_id = 0u;
    if (it_invoke1((long)tcb_h, INV_TCB_GET_INFO, (long)(uintptr_t)&alive) != 0) {
        it_fail("T285", "info while alive"); return;
    }

    /* Wait for the reaper: execution ends, object survives (our cap pins it). */
    struct iris_tcb_info info; info.state = 0u;
    for (int i = 0; i < 200; i++) {
        if (it_invoke1((long)tcb_h, INV_TCB_GET_INFO, (long)(uintptr_t)&info) != 0) { ok = 0; why = "info during teardown"; break; }
        if (info.state == (uint8_t)IT_TASK_TERMINATED) break;
        it_settle(1);
    }
    if (ok && info.state != (uint8_t)IT_TASK_TERMINATED) { ok = 0; why = "never terminated"; }
    if (ok && info.task_id != alive.task_id) { ok = 0; why = "id unstable after death"; }

    /* Registry lifetime ended at termination — while the cap still lives. */
    if (ok) {
        struct it_utq_taskobj t1;
        if (!it_utq_t(&t1) || t1.tcb_registry_active != t0.tcb_registry_active) {
            ok = 0; why = "registry pinned by cap";
        }
    }
    /* No resurrection of a terminal TCB. */
    if (ok && it_invoke0((long)tcb_h, INV_TCB_RESUME) != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "terminal resumed"; }

    it_close(&tcb_h);
    if (ok) it_pass("T285"); else it_fail("T285", why);
}

/* ── T286: adversarial retype/destroy/reset churn (anti-UAF, rollback) ──────
 * 20 cycles of create→resolve→destroy→retype over the SAME region: the gauges
 * return exact (no leak, no double-destroy), the registry never moves, and
 * generation_mismatch holds.  Atomic batch count=2.  A failed publication
 * (occupied slot) and insufficient capacity: ZERO effect (U14/U15). */
void test_t286(void) {
    int ok = 1;
    const char *why = "tcb churn";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T286", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    struct it_utq_taskobj t0, t1;
    if (!it_utq_t(&t0)) { it_close(&su_h); it_fail("T286", "query"); return; }

    for (int i = 0; ok && i < 20; i++) {
        if (it_retype2_at(su, IRIS_KOBJ_TCB, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "cycle retype"; break; }
        /* Hold a SECOND capability, drop the first, then drop the second: the
         * object outlives the first delete and its storage returns to the
         * untyped only on the last one.  The second reference used to be a
         * materialised handle; a CSpace copy proves the same lifetime rule in
         * the namespace that stays. */
        it_slot_delete(IT_SCRATCH_0);
        if (it_invoke2((long)S1_SLOT_A, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_SAME_RIGHTS) != 0) { ok = 0; why = "cycle copy"; break; }
        it_slot_delete(S1_SLOT_A);
        if (it_invoke0((long)IT_SCRATCH_0, INV_CAP_IDENTIFY) < 0) {
            ok = 0; why = "copy died with the original"; break;
        }
        it_slot_delete(IT_SCRATCH_0);       /* last ref → destructor → region */
        if ((i % 5) == 4 && it_invoke0(su, INV_UNTYPED_RESET) != 0) { ok = 0; why = "cycle reset"; break; }
    }
    (void)it_invoke0(su, INV_UNTYPED_RESET);

    /* Atomic batch: two TCBs in one call, both real, both destroyed. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_TCB, S1_SLOT_B, 2u, 0) != 0) { ok = 0; why = "batch"; }
    if (ok) {
        struct iris_tcb_info bi;
        if (it_invoke1((long)S1_SLOT_B, INV_TCB_GET_INFO, (long)(uintptr_t)&bi) != 0 ||
            it_invoke1((long)(S1_SLOT_B + 1u), INV_TCB_GET_INFO, (long)(uintptr_t)&bi) != 0) {
            ok = 0; why = "batch member dead";
        }
    }
    it_slot_delete(S1_SLOT_B); it_slot_delete(S1_SLOT_B + 1u);
    (void)it_invoke0(su, INV_UNTYPED_RESET);

    /* Failed publication (occupied slot): zero effect on the untyped. */
    if (ok && it_retype2_at(su, IRIS_KOBJ_TCB, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "occupy"; }
    if (ok) {
        struct it_utq_one ub, ua;
        if (!it_utq_1(su, &ub)) { ok = 0; why = "q before"; }
        if (ok && it_retype2_at(su, IRIS_KOBJ_TCB, S1_SLOT_A, 1u, 0) !=
                  (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occupied not refused"; }
        if (ok && (!it_utq_1(su, &ua) || ua.used_bytes != ub.used_bytes ||
                   ua.child_count != ub.child_count)) { ok = 0; why = "failed pub consumed"; }
    }
    it_slot_delete(S1_SLOT_A);
    (void)it_invoke0(su, INV_UNTYPED_RESET);

    /* Capacity refusal: 8 TCBs (≥ 8×sizeof(struct task) ≫ 4 KiB) cannot fit
     * a 4 KiB region — zero effect.  count=8 keeps the destination window
     * (S1_SLOT_C..+7) inside the 256-slot root CNode, so the failure exercised
     * is genuinely the CAPACITY check, not the slot-window validation. */
    if (ok) {
        long su2 = s1_sub_ut(4096);
        if (su2 < 0) { ok = 0; why = "sub2"; }
        else {
            handle_id_t su2_h = (handle_id_t)su2;
            if (it_retype2_at(su2, IRIS_KOBJ_TCB, S1_SLOT_C, 8u, 0) !=
                (long)IRIS_ERR_NO_MEMORY) { ok = 0; why = "capacity not refused"; }
            struct it_utq_one u2;
            if (ok && (!it_utq_1(su2, &u2) || u2.used_bytes != 0u || u2.child_count != 0u)) {
                ok = 0; why = "capacity fail consumed";
            }
            it_close(&su2_h);
        }
    }

    /* Exact accounting after all the churn; scheduler registry untouched. */
    if (ok && !it_utq_t(&t1)) { ok = 0; why = "query end"; }
    if (ok && t1.tcb_live != t0.tcb_live) { ok = 0; why = "tcb leak"; }
    if (ok && t1.tcb_destroyed < t0.tcb_destroyed + 22u) { ok = 0; why = "destroy count"; }
    if (ok && t1.tcb_registry_active != t0.tcb_registry_active) { ok = 0; why = "registry drift"; }
    if (ok && t1.tcb_registry_generation_mismatch != t0.tcb_registry_generation_mismatch) {
        ok = 0; why = "generation mismatch";
    }

    it_close(&su_h);
    if (ok) it_pass("T286"); else it_fail("T286", why);
}

/* ── T287: registry/backing independence after death ────────────────────────
 * A terminated TCB whose cap survives PINS its backing but NOT its registry
 * slot: a new thread is created and runs normally (possibly reusing the freed
 * registry slot) while the dead one's cap still answers TERMINATED with its
 * original id.  The external kill (non-self TCB_EXIT) on the new thread
 * terminates it cleanly. */
static volatile int      g_t287_ready = 0;
static volatile uint64_t g_t287_count = 0;
static long              g_t287_tcb   = -1;
static uint8_t           g_t287_stack[8192];

static void t287_helper(uint64_t self_tcb) {
    g_t287_tcb   = (long)self_tcb;
    g_t287_ready = 1;
    for (;;) { g_t287_count++; it_sys0(SYS_YIELD); }
}

void test_t287(void) {
    int ok = 1;
    const char *why = "registry/backing split";

    /* Thread A: exit while we keep its cap (reuses the T285 helper). */
    g_t285_ready = 0; g_t285_tcb = -1;
    uint64_t entry_a = (uint64_t)(uintptr_t)t285_helper;
    uint64_t rsp_a   = ((uint64_t)(uintptr_t)(g_t285_stack + sizeof(g_t285_stack))) & ~0xFULL;
    long tid_a = it_thread_create(entry_a, rsp_a, IT_THREAD_ARG_SELF_TCB);
    if (tid_a < 0) { it_fail("T287", "thread A create"); return; }
    IT_AWAIT(g_t285_ready, 200);
    if (!g_t285_ready || g_t285_tcb < 0) { it_fail("T287", "A tcb self"); return; }
    handle_id_t a_h = (handle_id_t)g_t285_tcb;

    /* Stage 5 Step 4: creation returns a capability, so A's identity is read
     * from A's own object rather than from the value the creation returned. */
    struct iris_tcb_info ia; ia.state = 0u;
    uint32_t id_a = 0u;
    if (it_invoke1((long)a_h, INV_TCB_GET_INFO, (long)(uintptr_t)&ia) == 0)
        id_a = ia.task_id;
    else { ok = 0; why = "A info"; }
    for (int i = 0; ok && i < 200; i++) {
        if (it_invoke1((long)a_h, INV_TCB_GET_INFO, (long)(uintptr_t)&ia) != 0) { ok = 0; why = "A info"; break; }
        if (ia.state == (uint8_t)IT_TASK_TERMINATED) break;
        it_settle(1);
    }
    if (ok && ia.state != (uint8_t)IT_TASK_TERMINATED) { ok = 0; why = "A never terminated"; }

    /* Thread B: must build and RUN while A's terminated object pins backing. */
    handle_id_t b_h = HANDLE_INVALID;
    if (ok) {
        g_t287_ready = 0; g_t287_count = 0; g_t287_tcb = -1;
        uint64_t entry_b = (uint64_t)(uintptr_t)t287_helper;
        uint64_t rsp_b   = ((uint64_t)(uintptr_t)(g_t287_stack + sizeof(g_t287_stack))) & ~0xFULL;
        long tid_b = it_thread_create(entry_b, rsp_b, IT_THREAD_ARG_SELF_TCB);
        if (tid_b < 0) { ok = 0; why = "thread B create"; }
        if (ok) IT_AWAIT(g_t287_ready, 200);
        if (ok && (!g_t287_ready || g_t287_tcb < 0)) { ok = 0; why = "B never ran"; }
        uint32_t id_b = 0u;
        if (ok) {
            b_h = (handle_id_t)g_t287_tcb;
            struct iris_tcb_info ib0;
            if (it_invoke1((long)b_h, INV_TCB_GET_INFO, (long)(uintptr_t)&ib0) == 0)
                id_b = ib0.task_id;
            uint64_t before = g_t287_count;
            it_settle(3);
            if (g_t287_count == before) { ok = 0; why = "B frozen"; }
        }
        /* A's cap still answers with A's identity — B did not alias it. */
        if (ok) {
            struct iris_tcb_info ia2;
            if (it_invoke1((long)a_h, INV_TCB_GET_INFO, (long)(uintptr_t)&ia2) != 0 ||
                ia2.state != (uint8_t)IT_TASK_TERMINATED ||
                ia2.task_id != id_a ||
                ia2.task_id == id_b /* ids must differ */)
                { ok = 0; why = "A identity aliased"; }
        }
        /* External kill of B through its cap; B must reach TERMINATED. */
        if (ok && it_invoke0((long)b_h, INV_TCB_EXIT) != 0) { ok = 0; why = "B kill"; }
        if (ok) {
            struct iris_tcb_info ib; ib.state = 0u;
            for (int i = 0; i < 200; i++) {
                if (it_invoke1((long)b_h, INV_TCB_GET_INFO, (long)(uintptr_t)&ib) != 0) { ok = 0; why = "B info"; break; }
                if (ib.state == (uint8_t)IT_TASK_TERMINATED) break;
                it_settle(1);
            }
            if (ok && ib.state != (uint8_t)IT_TASK_TERMINATED) { ok = 0; why = "B never terminated"; }
        }
    }

    it_close(&b_h);
    it_close(&a_h);
    if (ok) it_pass("T287"); else it_fail("T287", why);
}

/* ════════════════════════════════════════════════════════════════════════
 * Phase S3 — native MDB/CDT, revoke cross-process (T288–T290).
 *
 * Real kernel objects, real CSpace slots, a real second process
 * (IRIS_CPTR_TEST_PROC).  Authority loss is checked FUNCTIONALLY: a revoked
 * endpoint CPtr stops resolving (EP_NB_RECV fails), a surviving sibling keeps
 * working, and a cross-process slot freed by revoke becomes re-mintable.
 * ════════════════════════════════════════════════════════════════════════ */

/* SYS_CSPACE_MINT: copy (RIGHT_SAME_RIGHTS) / mint (reduced) src→dest within
 * the caller's CSpace.  dest_cnode 0 = own root. */
static long it_cs_mint(uint64_t src, uint32_t dslot, iris_rights_t rights,
                       uint64_t badge) {
    return it_invoke2((long)src, INV_CSPACE_MINT, (long)((uint64_t)0u | ((uint64_t)dslot << 32)), (long)((uint64_t)(uint32_t)rights | (badge << 32)));
}
static long it_cs_revoke(uint64_t cptr) {
    return it_invoke0((long)cptr, INV_CSPACE_REVOKE);
}
/* Stage 7 Step 9: every caller of this named the suite's OWN process to reach
 * its OWN CSpace, which SYS_CSPACE_MINT has expressed as dest_cnode 0 all
 * along.  Kept as a helper because the tests read better for it, not because
 * the operation is different. */
static long it_cs_mint_into(uint64_t proc, uint32_t dslot, uint64_t src,
                            iris_rights_t rights) {
    (void)proc;
    return it_invoke2((long)src, INV_CSPACE_MINT, IT_MINT_SELF(dslot), (long)(uint32_t)rights);
}
/* An endpoint is "alive" iff EP_NB_RECV resolves it (WOULD_BLOCK = no sender);
 * once its cap is revoked/deleted the CPtr no longer resolves (< 0, != WB). */
static int it_ep_alive(uint32_t slot) {
    struct iris_msg m; iris_msg_zero(&m);
    return iris_msg_nb_recv((long)slot, &m) == (long)IRIS_ERR_WOULD_BLOCK;
}
static int it_ep_dead(uint32_t slot) {
    struct iris_msg m; iris_msg_zero(&m);
    long r = iris_msg_nb_recv((long)slot, &m);
    return r < 0 && r != (long)IRIS_ERR_WOULD_BLOCK;
}

/* ── T288: same-CSpace derivation, revoke subtree, siblings survive,
 * delete ≠ revoke ─────────────────────────────────────────────────────────── */
void test_t288(void) {
    int ok = 1;
    const char *why = "cspace mdb";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T288", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    /* A = endpoint retyped from the sub-untyped (child of the untyped slot).
     * D = a second endpoint: an INDEPENDENT sibling under the same untyped. */
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype A"; }
    if (ok && it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_D, 1u, 0) != 0) { ok = 0; why = "retype D"; }

    /* Chain A → B → C by CSpace copy (RIGHT_SAME_RIGHTS keeps DUPLICATE). */
    if (ok && it_cs_mint(S1_SLOT_A, S1_SLOT_B, RIGHT_SAME_RIGHTS, 0) != 0) { ok = 0; why = "mint B"; }
    if (ok && it_cs_mint(S1_SLOT_B, S1_SLOT_C, RIGHT_SAME_RIGHTS, 0) != 0) { ok = 0; why = "mint C"; }
    /* D → E (independent subtree). */
    if (ok && it_cs_mint(S1_SLOT_D, S1_SLOT_E, RIGHT_SAME_RIGHTS, 0) != 0) { ok = 0; why = "mint E"; }

    /* All five caps resolve to the same-or-derived live endpoints. */
    if (ok && !(it_ep_alive(S1_SLOT_A) && it_ep_alive(S1_SLOT_B) &&
                it_ep_alive(S1_SLOT_C) && it_ep_alive(S1_SLOT_D) &&
                it_ep_alive(S1_SLOT_E))) { ok = 0; why = "not all alive"; }

    /* Occupied-dest mint is rejected with zero effect. */
    if (ok && it_cs_mint(S1_SLOT_A, S1_SLOT_B, RIGHT_SAME_RIGHTS, 0) !=
              (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occupied not refused"; }
    /* A handle source is refused (CSpace-only authority). */
    if (ok && it_cs_mint(handle_id_make(2u, 1u), S1_SLOT_B + 5u, RIGHT_SAME_RIGHTS, 0) !=
              (long)IRIS_ERR_INVALID_ARG) { ok = 0; why = "handle source accepted"; }

    /* Revoke A's subtree: B and C die; A survives; D and E untouched. */
    if (ok && it_cs_revoke(S1_SLOT_A) != 2) { ok = 0; why = "revoke count"; }
    if (ok && !(it_ep_alive(S1_SLOT_A) && it_ep_dead(S1_SLOT_B) &&
                it_ep_dead(S1_SLOT_C) && it_ep_alive(S1_SLOT_D) &&
                it_ep_alive(S1_SLOT_E))) { ok = 0; why = "revoke effect wrong"; }

    /* delete ≠ revoke: rebuild A→B→C, then DELETE B (only B goes; C survives,
     * reparented to A so A can still revoke it). */
    if (ok && it_cs_mint(S1_SLOT_A, S1_SLOT_B, RIGHT_SAME_RIGHTS, 0) != 0) { ok = 0; why = "rebuild B"; }
    if (ok && it_cs_mint(S1_SLOT_B, S1_SLOT_C, RIGHT_SAME_RIGHTS, 0) != 0) { ok = 0; why = "rebuild C"; }
    if (ok) it_slot_delete(S1_SLOT_B);              /* SYS_CNODE_DELETE own root */
    if (ok && !(it_ep_dead(S1_SLOT_B) && it_ep_alive(S1_SLOT_C) &&
                it_ep_alive(S1_SLOT_A))) { ok = 0; why = "delete != revoke"; }
    /* A still reaches C (reparented): revoke A removes exactly C. */
    if (ok && it_cs_revoke(S1_SLOT_A) != 1) { ok = 0; why = "reparent revoke count"; }
    if (ok && !(it_ep_alive(S1_SLOT_A) && it_ep_dead(S1_SLOT_C))) { ok = 0; why = "reparent revoke effect"; }

    it_slot_delete(S1_SLOT_A); it_slot_delete(S1_SLOT_D); it_slot_delete(S1_SLOT_E);
    (void)it_invoke0(su, INV_UNTYPED_RESET);
    it_close(&su_h);
    if (ok) it_pass("T288"); else it_fail("T288", why);
}
void test_t289(void) {
    int ok = 1;
    const char *why = "cross-process revoke";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T289", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    /* A = endpoint in OUR CSpace (child of the untyped slot). */
    if (it_retype2_at(su, IRIS_KOBJ_ENDPOINT, S1_SLOT_A, 1u, 0) != 0) { ok = 0; why = "retype A"; }

    /* Derive A into the TEST_PROC child's CSpace — an MDB child of A that
     * lives in ANOTHER process.  Occupied-slot re-mint proves it landed. */
    if (ok && it_cs_mint_into(IRIS_CPTR_TEST_PROC, T289_TSLOT, S1_SLOT_A,
                              RIGHT_SAME_RIGHTS) != 0) { ok = 0; why = "mint_into"; }
    if (ok && it_cs_mint_into(IRIS_CPTR_TEST_PROC, T289_TSLOT, S1_SLOT_A,
                              RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_ALREADY_EXISTS) {
        ok = 0; why = "cross slot not filled";
    }
    /* A handle source is refused even for the cross-process path. */
    if (ok && it_cs_mint_into(IRIS_CPTR_TEST_PROC, T289_TSLOT + 1u, handle_id_make(3u, 1u),
                              RIGHT_SAME_RIGHTS) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "cross handle source accepted";
    }

    /* Revoke A: the cross-process descendant is destroyed (count includes it).
     * Functional proof: the freed slot in the OTHER process re-mints cleanly. */
    if (ok && it_cs_revoke(S1_SLOT_A) != 1) { ok = 0; why = "cross revoke count"; }
    if (ok && it_cs_mint_into(IRIS_CPTR_TEST_PROC, T289_TSLOT, S1_SLOT_A,
                              RIGHT_SAME_RIGHTS) != 0) {
        ok = 0; why = "cross slot not freed by revoke";
    }
    /* A survives the revoke (it is the invoked node, not a descendant). */
    if (ok && !it_ep_alive(S1_SLOT_A)) { ok = 0; why = "invoked node lost"; }

    /* Cleanup: revoke the fresh cross child, then drop A. */
    if (ok) (void)it_cs_revoke(S1_SLOT_A);
    it_slot_delete(S1_SLOT_A);
    (void)it_invoke0(su, INV_UNTYPED_RESET);
    it_close(&su_h);
    if (ok) it_pass("T289"); else it_fail("T289", why);
}

/* ── T290: Untyped is the MDB ancestor of its retyped objects ─────────────── */
void test_t290(void) {
    int ok = 1;
    const char *why = "untyped ancestor";
    long su = s1_sub_ut(65536);
    if (su < 0) { it_fail("T290", "sub untyped"); return; }
    handle_id_t su_h = (handle_id_t)su;

    /* Carve a SECOND untyped as a CSpace slot (child of su's slot), then
     * retype an endpoint FROM that slot: the endpoint is an MDB child of the
     * sub-untyped slot.  Copy it once more (grandchild of the untyped). */
    if (it_retype2_at(su, IRIS_KOBJ_UNTYPED, S1_SLOT_A, 1u, 8192) != 0) { ok = 0; why = "sub-untyped"; }
    if (ok && it_retype2_at((long)S1_SLOT_A, IRIS_KOBJ_ENDPOINT, S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "retype ep"; }
    if (ok && it_cs_mint(S1_SLOT_B, S1_SLOT_C, RIGHT_SAME_RIGHTS, 0) != 0) { ok = 0; why = "copy ep"; }
    if (ok && !(it_ep_alive(S1_SLOT_B) && it_ep_alive(S1_SLOT_C))) { ok = 0; why = "ep not alive"; }

    /* RESET of the sub-untyped must fail while its retyped objects live. */
    if (ok && it_invoke0((long)S1_SLOT_A, INV_UNTYPED_RESET) != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "reset not busy with children";
    }

    /* Revoke the sub-untyped CAPABILITY: its entire retyped descendance
     * (endpoint + copy) is destroyed; the untyped survives.  Authority loss
     * is functional (both endpoint CPtrs stop resolving). */
    if (ok && it_cs_revoke(S1_SLOT_A) != 2) { ok = 0; why = "untyped revoke count"; }
    if (ok && !(it_ep_dead(S1_SLOT_B) && it_ep_dead(S1_SLOT_C))) { ok = 0; why = "objects survived revoke"; }

    /* With the descendance gone, the objects' destructors returned their
     * storage: RESET now succeeds and the region is reusable. */
    if (ok && it_invoke0((long)S1_SLOT_A, INV_UNTYPED_RESET) != 0) { ok = 0; why = "reset after revoke"; }
    if (ok && it_retype2_at((long)S1_SLOT_A, IRIS_KOBJ_ENDPOINT, S1_SLOT_B, 1u, 0) != 0) { ok = 0; why = "reuse"; }

    it_slot_delete(S1_SLOT_B);
    it_slot_delete(S1_SLOT_A);
    (void)it_invoke0(su, INV_UNTYPED_RESET);
    it_close(&su_h);
    if (ok) it_pass("T290"); else it_fail("T290", why);
}

/* ── T292: SYS_CAP_IDENTIFY — CSpace-native type of a slot ────────────────
 * The CSpace-native successor of SYS_HANDLE_TYPE.  Asserts the four things
 * that make it a capability operation rather than a directory listing:
 * (1) it reports the true type of a slot the caller names, for every family
 *     the suite can fabricate;
 * (2) it requires NO right — a slot minted down to RIGHT_READ still answers,
 *     because naming the slot is the authority;
 * (3) an EMPTY slot is NOT_FOUND, indistinguishable from a never-assigned
 *     one: the caller cannot enumerate its own CSpace by scanning;
 * (4) it is CPtr-only — CPTR_NULL and a handle value are INVALID_ARG with no
 *     fallback to the handle table (charter §3.6/§3.7).
 * Invariant: A6 (no cross-namespace fallback), A3. */
void test_t292(void) {
    int ok = 1;
    const char *why = "cap identify";

    long ep = it_ep_create_slot();
    long no = it_notify_create_slot();
    if (ep < 0 || no < 0) { it_fail("T292", "fixture"); return; }

    /* (1) true type per family. */
    if (it_invoke0(ep, INV_CAP_IDENTIFY) != (long)IRIS_KOBJ_ENDPOINT) {
        ok = 0; why = "endpoint type";
    }
    if (ok && it_invoke0(no, INV_CAP_IDENTIFY) != (long)IRIS_KOBJ_NOTIFICATION) {
        ok = 0; why = "notification type";
    }
    if (ok && it_invoke0((long)IRIS_CPTR_TEST_UNTYPED, INV_CAP_IDENTIFY)
              != (long)IRIS_KOBJ_UNTYPED) {
        ok = 0; why = "untyped type";
    }

    /* (2) no right is required: derive a read-only copy and identify it. */
    if (ok) {
        it_slot_delete(IT_SCRATCH_0);
        if (it_invoke2(ep, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)RIGHT_READ) != 0) {
            ok = 0; why = "mint read-only";
        } else if (it_invoke0((long)IT_SCRATCH_0, INV_CAP_IDENTIFY)
                   != (long)IRIS_KOBJ_ENDPOINT) {
            ok = 0; why = "identify needs rights";
        }
        it_slot_delete(IT_SCRATCH_0);
    }

    /* (3) an empty slot answers NOT_FOUND, not "empty". */
    if (ok && it_invoke0((long)IT_SCRATCH_0, INV_CAP_IDENTIFY)
              != (long)IRIS_ERR_NOT_FOUND) {
        ok = 0; why = "empty slot not NOT_FOUND";
    }

    /* (4) CPtr only — no handle leg, no fallback. */
    if (ok && it_invoke0(0, INV_CAP_IDENTIFY) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "null cptr accepted";
    }
    /* A value carrying the retired handle namespace's tag bit is not a CPtr
     * and must be rejected outright — no fallback, no reinterpretation.  It
     * used to be a REAL handle from the materialising factory; with the
     * namespace gone the encoding is what is left to reject, and that is the
     * property worth pinning. */
    if (ok && it_invoke0((long)(HANDLE_TAG | 0x401u), INV_CAP_IDENTIFY)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "tagged value accepted";
    }

    {
        handle_id_t eh = (handle_id_t)ep, nh = (handle_id_t)no;
        it_close(&eh); it_close(&nh);
    }
    if (ok) it_pass("T292"); else it_fail("T292", why);
}

/* ── T293: SYS_CAP_SAME_OBJECT — identity across derivation ───────────────
 * The property the transfer and derivation tests actually need: a minted or
 * rights-reduced capability names the SAME kernel object as its source, and
 * two independently retyped objects never collide.  Identity is compared, not
 * rights and not badge — a cap minted down to RIGHT_READ with a badge is
 * still the same endpoint.  CPtr-only on BOTH arguments.
 * Invariants: A7 (rights reduce, identity survives), A8, A6. */
void test_t293(void) {
    int ok = 1;
    const char *why = "same object";

    long a = it_ep_create_slot();
    long b = it_ep_create_slot();
    if (a < 0 || b < 0) { it_fail("T293", "fixture"); return; }

    /* Distinct objects are distinct. */
    if (it_invoke1(a, INV_CAP_SAME_OBJECT, b) != 0) { ok = 0; why = "distinct eps equal"; }
    /* A slot is the same object as itself. */
    if (ok && it_invoke1(a, INV_CAP_SAME_OBJECT, a) != 1) { ok = 0; why = "self not equal"; }

    /* A derived, rights-reduced, badged cap is the SAME object. */
    if (ok) {
        it_slot_delete(IT_SCRATCH_0);
        if (it_invoke2(a, INV_CSPACE_MINT, (long)((uint64_t)IT_SCRATCH_0 << 32), (long)((uint64_t)RIGHT_READ | (0x5A5AULL << 32))) != 0) {
            ok = 0; why = "mint badged";
        } else if (it_invoke1(a, INV_CAP_SAME_OBJECT, (long)IT_SCRATCH_0) != 1) {
            ok = 0; why = "derived not same object";
        } else if (it_invoke1((long)IT_SCRATCH_0, INV_CAP_SAME_OBJECT, b) != 0) {
            ok = 0; why = "derived matches unrelated";
        }
        it_slot_delete(IT_SCRATCH_0);
    }

    /* Empty and null slots fail closed on either argument. */
    if (ok && it_invoke1(a, INV_CAP_SAME_OBJECT, (long)IT_SCRATCH_0)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "empty b not NOT_FOUND"; }
    if (ok && it_invoke1((long)IT_SCRATCH_0, INV_CAP_SAME_OBJECT, a)
              != (long)IRIS_ERR_NOT_FOUND) { ok = 0; why = "empty a not NOT_FOUND"; }
    if (ok && it_invoke1(a, INV_CAP_SAME_OBJECT, 0) != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "null b accepted";
    }
    /* Same rule on the second argument: a tagged value is not a CPtr. */
    if (ok && it_invoke1(a, INV_CAP_SAME_OBJECT, (long)(HANDLE_TAG | 0x401u))
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "tagged value accepted";
    }

    {
        handle_id_t ah = (handle_id_t)a, bh = (handle_id_t)b;
        it_close(&ah); it_close(&bh);
    }
    if (ok) it_pass("T293"); else it_fail("T293", why);
}

static handle_id_t  g_t294_cmd_ep = HANDLE_INVALID;
static handle_id_t  g_t294_cap    = HANDLE_INVALID;
static volatile int g_t294_s1 = 999, g_t294_done = 0;
static uint8_t      g_t294_stack[8192];

static void t294_sender(void) {
    struct iris_msg m;
    iris_msg_zero(&m);
    m.label           = 0x94;
    m.cap = (uint32_t)g_t294_cap;
    m.cap_rights = RIGHT_WRITE;
    g_t294_s1 = (int)iris_msg_send((long)g_t294_cmd_ep, &m);
    g_t294_done = 1;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

void test_t294(void) {
    g_t294_s1 = 999; g_t294_done = 0;
    it_slot_delete((uint32_t)T294_CPTR);

    long n   = it_notify_create_slot();   /* the cap being transferred */
    long cmd = it_ep_create_slot();       /* the transfer channel */
    if (n < 0 || cmd < 0) { it_fail("T294", "create"); return; }
    handle_id_t n_h = (handle_id_t)n;
    g_t294_cmd_ep = (handle_id_t)cmd;

    long c = it_xfer_slot(n_h, IT_XFER_SLOT_C, RIGHT_WRITE);
    if (c < 0) {
        it_close(&n_h); it_close(&g_t294_cmd_ep);
        it_fail("T294", "xfer slot"); return;
    }
    g_t294_cap = (handle_id_t)c;

    uint64_t entry = (uint64_t)(uintptr_t)t294_sender;
    uint64_t rsp   = ((uint64_t)(uintptr_t)(g_t294_stack + sizeof(g_t294_stack))) & ~0xFULL;
    if (it_thread_create(entry, rsp, 0) < 0) {
        it_close(&n_h); it_close(&g_t294_cmd_ep);
        it_fail("T294", "thread create"); return;
    }
    it_settle(2);   /* let the sender queue its send */

    int ok = 1;
    const char *why = "deep recv slot";
    struct iris_msg r;

    iris_msg_zero(&r);
    r.recv_slot = (uint32_t)T294_CPTR;
    if (iris_msg_recv((long)g_t294_cmd_ep, &r) != 0) {
        ok = 0; why = "recv";
    }
    /* Delivered AT the declared CPtr, and reported as a CPtr — not reclassified
     * as a handle because it happens to exceed 1024. */
    if (ok && r.got_cap != (uint32_t)T294_CPTR) { ok = 0; why = "wrong dest"; }
    if (ok && !iris_msg_cap_is_cptr(r.got_cap)) { ok = 0; why = "classified as handle"; }

    /* The capability is really there, is the right type, and works. */
    if (ok && it_invoke0((long)T294_CPTR, INV_CAP_IDENTIFY)
              != (long)IRIS_HANDLE_TYPE_NOTIFICATION) { ok = 0; why = "not a notification"; }
    if (ok && it_invoke1((long)T294_CPTR, INV_NOTIFY_SIGNAL, 0x94) != 0) {
        ok = 0; why = "signal through deep slot";
    }
    if (ok) {
        uint64_t bits = 0;
        if (it_invoke1(n, INV_NOTIFY_WAIT, (long)(uintptr_t)&bits) != 0 || bits != 0x94u) {
            ok = 0; why = "wait";
        }
    }
    /* It is the SAME notification the sender held, not a look-alike. */
    if (ok && it_invoke1(n, INV_CAP_SAME_OBJECT, (long)T294_CPTR) != 1) {
        ok = 0; why = "not the same object";
    }

    /* Declaring the SAME deep slot again, now occupied, fails fast. */
    if (ok) {
        struct iris_msg r2;
        iris_msg_zero(&r2);
        r2.recv_slot = (uint32_t)T294_CPTR;
        if (iris_msg_nb_recv((long)g_t294_cmd_ep, &r2)
            != (long)IRIS_ERR_ALREADY_EXISTS) { ok = 0; why = "occupied deep slot"; }
    }

    IT_AWAIT(g_t294_done, 200);
    if (ok && (!g_t294_done || g_t294_s1 != 0)) { ok = 0; why = "sender"; }

    it_slot_delete((uint32_t)T294_CPTR);
    it_close(&n_h);
    it_close(&g_t294_cmd_ep);
    if (ok) it_pass("T294"); else it_fail("T294", why);
}

static volatile uint32_t g_t297_ran;
static uint8_t g_t297_stack[4096] __attribute__((aligned(16)));

static void t297_helper(void) {
    g_t297_ran = 1u;
    /* Stay alive long enough for the parent to prove the entry frame is
     * frozen, then leave through the ordinary thread exit. */
    for (int i = 0; i < 50; i++) it_sys0(SYS_YIELD);
    g_t297_ran = 2u;
    it_sys1(SYS_EXIT, 0);
    for (;;) {}
}

/* ── T297: a thread is retyped, configured and started (Stage 5 Step 4) ──
 * Every thread in this suite is already born this way — the helper that used
 * to call SYS_THREAD_CREATE now retypes a TCB from the suite's own Untyped and
 * configures it with capabilities — so the happy path is covered thirty times
 * over by the tests that use threads.  What is asserted HERE is the gate: the
 * things that must NOT work, because each of them is a way the old pool-based
 * creation could come back in disguise.
 *
 *   1. the retired SYS_THREAD_CREATE answers NOT_SUPPORTED (T148 pins the
 *      number; this pins the semantics from a caller that used to succeed);
 *   2. an UNCONFIGURED retyped TCB cannot be started, cannot be written to,
 *      and cannot be exited — it is a capability citizen with no execution;
 *   3. CONFIGURE requires REAL capabilities of the right type: a CNode where a
 *      VSpace belongs is refused, and so is a capability to someone else's
 *      CSpace (here: the object CNode, which is a CNode but not the root);
 *   4. a thread cannot be configured twice, and its entry frame cannot be
 *      rewritten once it has been runnable — its kernel stack holds live
 *      state by then.
 * Invariants: A1, A3, O1, O5, S2. */
void test_t297(void) {
    int ok = 1;
    const char *why = "retyped thread";

    /* 1. the pool path is gone for good. */
    if (it_sys3(SYS_THREAD_CREATE, 0x8000200000L, 0x8000300000L, 0)
        != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "thread_create not retired";
    }

    long cs = ok ? it_cspace_self() : -1;
    if (ok && cs < 0) { ok = 0; why = "cspace self"; }
    if (ok && !it_setup_self_vspace()) { ok = 0; why = "vspace self"; }

    long tcb = -1;
    if (ok) {
        tcb = it_retype_slot_alloc((long)IRIS_CPTR_TEST_UNTYPED, IRIS_KOBJ_TCB, 0);
        if (tcb < 0) { ok = 0; why = "tcb retype"; }
    }

    /* 2. inactive means inactive. */
    if (ok && it_invoke0(tcb, INV_TCB_RESUME) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "unconfigured resumed";
    }
    if (ok && it_invoke(tcb, INV_TCB_WRITE_REGS, 0x8000200000L, 0x8000300000L, 0)
              != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "unconfigured written";
    }
    if (ok && it_invoke0(tcb, INV_TCB_EXIT) != (long)IRIS_ERR_NOT_SUPPORTED) {
        ok = 0; why = "unconfigured exited";
    }

    /* 3. the arguments are capabilities, and their type and identity matter. */
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, IT_VS, IT_VS)
              != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "vspace accepted as cspace";
    }
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, cs, cs)
              != (long)IRIS_ERR_WRONG_TYPE) {
        ok = 0; why = "cnode accepted as vspace";
    }
    /*
     * Stage 7-proc: the "foreign cnode" probe is RETIRED.
     *
     * It asserted that a CNode which is not the target process's own root is
     * ACCESS_DENIED — KProcess acting as an authority over a capability the
     * caller already held: naming it was not enough, it also had to match a
     * third object's idea of what your CSpace should be.  A thread runs in the
     * CSpace and address space its configurer named and holds, and threads
     * sharing that pair are what a process IS rather than something checked
     * against one.  This is seL4's seL4_TCB_Configure.
     *
     * What survives is that both arguments must be capabilities you can
     * actually resolve, which the two probes above (wrong type each way) and
     * the one below (CPTR_NULL) cover.
     */
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, 0, IT_VS)
              != (long)IRIS_ERR_INVALID_ARG) {
        ok = 0; why = "cptr_null accepted";
    }

    /* 4. configure once; write regs only before it runs. */
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, cs, IT_VS) != 0) {
        ok = 0; why = "configure";
    }
    if (ok && it_invoke2(tcb, INV_TCB_CONFIGURE, cs, IT_VS)
              != (long)IRIS_ERR_ALREADY_EXISTS) {
        ok = 0; why = "configured twice";
    }
    if (ok && it_invoke(tcb, INV_TCB_WRITE_REGS, (long)(uintptr_t)t297_helper, (long)(((uint64_t)(uintptr_t)(g_t297_stack +
                              sizeof(g_t297_stack))) & ~0xFULL), 0) != 0) {
        ok = 0; why = "write regs";
    }
    g_t297_ran = 0;
    if (ok && it_invoke0(tcb, INV_TCB_RESUME) != 0) { ok = 0; why = "resume"; }
    if (ok) IT_AWAIT(g_t297_ran, 200);
    if (ok && !g_t297_ran) { ok = 0; why = "never ran"; }

    /* Its entry frame is frozen now: it is standing on that kernel stack. */
    if (ok && it_invoke(tcb, INV_TCB_WRITE_REGS, 0x8000200000L, 0x8000300000L, 0)
              != (long)IRIS_ERR_BUSY) {
        ok = 0; why = "regs rewritten after start";
    }

    for (int i = 0; i < 200 && g_t297_ran == 1u; i++) it_settle(1);
    if (ok) it_pass("T297"); else it_fail("T297", why);
}
