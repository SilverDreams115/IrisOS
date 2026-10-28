/*
 * T359, T360 — D-13: which revoke reaches a mapping, and which must not.
 *
 * Until this tree grew `kvspace_finalise_orphans`, removing every capability
 * to a mapped frame left the PTE standing: the page stayed readable and
 * writable through an address space that held no authority naming it.  That is
 * access without authority, which charter A10 forbids, and `blk`, `fs` and
 * `ip` each depended on the opposite of what they intended — they hand a data
 * frame to a client and REVOKE it before the next handout, believing that
 * withdrew access, while the client could still read the page.
 *
 * Closing it needs the relation between the capabilities to be exactly right,
 * and BOTH directions are tested here because getting one of them wrong cost a
 * boot.  A space holds what it mapped as a capability that is a SIBLING of the
 * slot the mapper named:
 *
 *   T359 — revoking the slot MAPPED THROUGH must NOT unmap.  That slot's
 *          descendants are the copies handed out, not the mapper's own page,
 *          and seL4 behaves the same way because there the mapping lives in
 *          the CTE itself.  Making the space's capability a CHILD instead
 *          unmapped the mapper: thirteen pages went in the first eighteen
 *          lines of a boot and init then blocked forever in `FS_OP_STAT`.
 *          Then revoking an ANCESTOR of it must unmap, because the mapping's
 *          authority descends from that ancestor — the case A10 names.
 *   T360 — the same ancestor revoke with the frame mapped at TWO addresses
 *          through ONE capability, because a space holds one capability
 *          however many times it mapped the frame and the sweep has to take
 *          every mapping, not the first one it finds.
 *
 * Neither test reads the mapped address after the unmap: there is no PTE any
 * more, and touching it would be a page fault rather than an assertion.
 *
 * T137 is a third case and still holds unchanged: it revokes a capability on a
 * branch the mapping does not descend from at all.
 *
 * Invariants: A9, A10 (+ V17, V18).
 */
#include "it_priv.h"

/* IT_S4_MAPLIVE counts the KFrameMapping records alive in the kernel, so a
 * mapping that went away is visible without ring 3 touching the address. */
void test_t359(void) {
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T359", "vspace self mint"); return; }
    if (!it_sched_ext4(v0)) { it_fail("T359", "sched ext"); return; }
    int ok = 1;
    const char *why = "ancestor revoke did not unmap";

    iris_cptr_t fr = it_retype_frame();
    if (fr == IRIS_CPTR_NULL) { it_fail("T359", "retype frame"); return; }

    /* The slot mapped THROUGH: a child of `fr`, so `fr` is its ancestor. */
    long frc = it_cdt_root(fr, IT_SCRATCH_0);
    if (frc < 0) { ok = 0; why = "root slot"; }

    if (ok && it_invoke((long)frc, INV_FRAME_MAP, IT_VS,
                        (long)T359_VA, (long)IT_MAP_W) != 0) { ok = 0; why = "map"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 1u) { ok = 0; why = "map not counted"; }

    /* ── Half one: revoking the slot mapped through SPARES the mapping ──── */
    if (ok && it_cdt_revoke(frc) < 0) { ok = 0; why = "revoke frc"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 after frc revoke"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 1u) {
        ok = 0; why = "revoke of the mapping slot unmapped the mapper";
    }
    /* Still usable: this is the page a server keeps after withdrawing copies. */
    if (ok) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)T359_VA;
        *p = 0x359ABCDEu;
        __asm__ volatile ("" ::: "memory");
        if (*p != 0x359ABCDEu) { ok = 0; why = "mapper lost its own page"; }
    }

    /* ── Half two: revoking an ANCESTOR takes the mapping with it ───────── */
    if (ok && it_cdt_revoke((long)fr) < 0) { ok = 0; why = "revoke fr"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 after fr revoke"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "ancestor revoke did not unmap"; }
    /* Gone, not merely uncounted: unmapping nothing must fail. */
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T359_VA) == 0) {
        ok = 0; why = "unmap succeeded after ancestor revoke";
    }
    /* The FRAME survives: `fr` still names it — revoke spares its own root. */
    if (ok && it_invoke0((long)fr, INV_CAP_IDENTIFY) != (long)IT_KOBJ_FRAME) {
        ok = 0; why = "frame died";
    }

    it_slot_delete(IT_SCRATCH_0);
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame still mapped?)"; }
    if (ok && (!it_sched_ext4(v1) || v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE])) {
        ok = 0; why = "mapping leak";
    }

    if (ok) it_pass("T359"); else it_fail("T359", why);
}

/* ONE capability, TWO mappings: the sweep must take both. */
void test_t360(void) {
    uint32_t v0[5], v1[5];
    it_quiesce_reaper();
    if (!it_setup_self_vspace()) { it_fail("T360", "vspace self mint"); return; }
    if (!it_sched_ext4(v0)) { it_fail("T360", "sched ext"); return; }
    int ok = 1;
    const char *why = "revoke swept only one mapping";

    iris_cptr_t fr = it_retype_frame();
    if (fr == IRIS_CPTR_NULL) { it_fail("T360", "retype frame"); return; }

    long frc = it_cdt_root(fr, IT_SCRATCH_0);
    if (frc < 0) { ok = 0; why = "root slot"; }

    if (ok && it_invoke((long)frc, INV_FRAME_MAP, IT_VS,
                        (long)T360_VA_A, (long)IT_MAP_W) != 0) { ok = 0; why = "map A"; }
    if (ok && it_invoke((long)frc, INV_FRAME_MAP, IT_VS,
                        (long)T360_VA_B, (long)IT_MAP_W) != 0) { ok = 0; why = "map B"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 mid"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE] + 2u) { ok = 0; why = "two maps not counted"; }

    /* The ancestor, so both mappings lose their authority at once. */
    if (ok && it_cdt_revoke((long)fr) < 0) { ok = 0; why = "revoke fr"; }
    if (ok && !it_sched_ext4(v1)) { ok = 0; why = "ext4 post"; }
    if (ok && v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE]) { ok = 0; why = "revoke swept only one mapping"; }

    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T360_VA_A) == 0) {
        ok = 0; why = "VA A still mapped";
    }
    if (ok && it_invoke2((long)fr, INV_FRAME_UNMAP, IT_VS, (long)T360_VA_B) == 0) {
        ok = 0; why = "VA B still mapped";
    }

    it_slot_delete(IT_SCRATCH_0);
    it_close(&fr);
    it_quiesce_reaper();
    if (ok && !it_ut_reset()) { ok = 0; why = "reset busy (frame still mapped?)"; }
    if (ok && (!it_sched_ext4(v1) || v1[IT_S4_MAPLIVE] != v0[IT_S4_MAPLIVE])) {
        ok = 0; why = "mapping leak";
    }

    if (ok) it_pass("T360"); else it_fail("T360", why);
}
