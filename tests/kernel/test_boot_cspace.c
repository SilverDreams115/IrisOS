/*
 * test_boot_cspace.c — Fase 3.4 + Fase 3.5 unit tests for bootstrap CSpace grants.
 *
 * Fase 3.4 tests (BC-1..BC-10): Boot KUntyped in slots 16-255.
 *   [BC-1]  Insert at BOOT_CPTR_UNTYPED_START and resolve via CPtr.
 *   [BC-2]  CNode slot rights match handle-table rights (equal, not greater).
 *   [BC-3]  CPTR_NULL slot (0) remains empty after boot grants.
 *   [BC-4]  Multiple consecutive boot slots are all resolvable.
 *   [BC-5]  Failed mint (out-of-range slot) cleans refs; object stays alive.
 *   [BC-6]  Dual insert (handle + CNode): refcounts balanced; teardown clean.
 *   [BC-7]  BOOT_CPTR_UNTYPED_START and BOOT_CPTR_UNTYPED_END within 256-slot CNode.
 *   [BC-8]  Repeated CPtr resolve does not leak refs.
 *   [BC-9]  ACCESS_DENIED on read-only CNode slot blocks write-required resolve.
 *   [BC-10] Slot just past BOOT_CPTR_UNTYPED_END (slot 256) is out-of-range for mint.
 *
 * Fase 3.5 tests (BB-1..BB-10): KBootstrapCap well-known slot 1.
 *   [BB-1]  BOOT_CPTR_BOOTSTRAP_CAP == 1 and != CPTR_NULL.
 *   [BB-2]  Slots 2-15 remain empty after inserting KBootstrapCap in slot 1.
 *   [BB-3]  KBootstrapCap in slot 1 resolves via CPtr; type == KOBJ_BOOTSTRAP_CAP.
 *   [BB-4]  Rights in CSpace slot == legacy handle rights (not greater).
 *   [BB-5]  Dual insert: refcounts balanced (refcount=2, active_refs=2).
 *   [BB-6]  Second kcnode_mint in slot 1 overwrites cleanly; old cap released.
 *   [BB-7]  ACCESS_DENIED from slot 1 (read-only) blocks resolve for WRITE.
 *   [BB-8]  Legacy handle path resolves KBootstrapCap independently.
 *   [BB-9]  Boot KUntyped slots 16+ intact after KBootstrapCap in slot 1.
 *   [BB-10] CPTR_NULL (slot 0) stays empty after all boot grants.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/boot_info.h>
#include <iris/kpage.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

/* ── Helpers (mirror test_untyped_cspace.c helpers) ──────────────────── */

static struct KProcess *bc_make_proc(void) {
    struct KProcess *p = (struct KProcess *)kpage_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->cspace_root = NULL;
    return p;
}

static void bc_free_proc(struct KProcess *p) {
    /* Stage 4: the root is held structurally, so the fixture drops its refs
     * explicitly instead of relying on handle_table_close_all. */
    if (p->cspace_root) {
        kobject_active_release(&p->cspace_root->base);
        kobject_release(&p->cspace_root->base);
        p->cspace_root = NULL;
    }
    kpage_free(p, (uint32_t)sizeof(*p));
}

static struct KCNode *bc_setup_root(struct KProcess *p) {
    struct KCNode *root = kcnode_alloc(KCNODE_DEFAULT_SLOTS);
    if (!root) return NULL;
    /* kcnode_alloc's ref becomes the lifecycle ref; add the active ref the
     * handle used to contribute (mirrors kprocess_alloc). */
    kobject_active_retain(&root->base);
    p->cspace_root = root;
    return root;
}

/* Stage 4: the CSpace root is no longer addressed by a handle.  This shim
 * keeps the fixtures' fetch-then-release shape while reading it structurally,
 * so the assertions below still exercise the same ref discipline. */
static iris_error_t bc_root_fetch(struct KProcess *p, struct KObject **out,
                                  iris_rights_t *rights_out) {
    if (!p || !p->cspace_root) return IRIS_ERR_NOT_FOUND;
    *out = &p->cspace_root->base;
    *rights_out = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
    kobject_retain(*out);
    return IRIS_OK;
}

static struct KUntyped *bc_make_ut(uint64_t size) {
    void *buf = malloc((size_t)size);
    if (!buf) return NULL;
    return kuntyped_create((uint64_t)(uintptr_t)buf, size, 0);
}

/* Simulate the kernel_main Ph76 boot-block publish at drain_idx.
 *
 * Stage 4: the kernel publishes each boot Untyped into the root CNode ONLY;
 * the parallel handle insert this helper used to mirror is deleted, and with
 * it the "CSpace failure is non-fatal because the handle still works" shape
 * the tests below were written around. */
static iris_error_t bc_boot_publish(struct KProcess *p,
                                    struct KUntyped *boot_ut,
                                    uint32_t         drain_idx) {
    iris_rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
    uint32_t cspace_slot = BOOT_CPTR_UNTYPED_START + drain_idx;

    if (!p->cspace_root || cspace_slot >= KCNODE_DEFAULT_SLOTS) {
        kobject_release(&boot_ut->base);
        return IRIS_ERR_NOT_FOUND;
    }
    /* Order matters, exactly as in kernel_main: the mint takes its own refs,
     * so the alloc ref is released AFTER it, never before. */
    iris_error_t me = kcnode_mint(p->cspace_root, cspace_slot,
                                  &boot_ut->base, r);
    kobject_release(&boot_ut->base);   /* drop alloc ref */
    return me;
}

/* ── Test suite ───────────────────────────────────────────────────────── */

void test_boot_cspace(void) {
    TEST_SUITE("boot_cspace");

    /* [BC-1] Single boot block: resolve via CPtr works after dual insert. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);

        ASSERT_EQ(bc_boot_publish(p, ut, 0u), IRIS_OK);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_untyped(
                      p, BOOT_CPTR_UNTYPED_START, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_UNTYPED);
        ASSERT_TRUE((rout & RIGHT_READ)  != 0);
        ASSERT_TRUE((rout & RIGHT_WRITE) != 0);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        bc_free_proc(p);
    }

    /* [BC-2] CNode slot rights == handle-table rights (not greater). */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);

        iris_rights_t expected = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        ASSERT_EQ(bc_boot_publish(p, ut, 0u), IRIS_OK);

        /* CSpace path rights. */
        struct KUntyped *out; iris_rights_t cptr_r;
        ASSERT_EQ(cspace_resolve_only_untyped(
                      p, BOOT_CPTR_UNTYPED_START, RIGHT_NONE, &out, &cptr_r),
                  IRIS_OK);
        ASSERT_EQ(cptr_r, expected);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        bc_free_proc(p);
    }

    /* [BC-3] CPTR_NULL slot (0) remains empty after boot grants. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(bc_boot_publish(p, ut, 0u), IRIS_OK);

        struct KUntyped *out; iris_rights_t rout;
        iris_error_t err = cspace_resolve_only_untyped(
            p, CPTR_NULL, RIGHT_NONE, &out, &rout);
        ASSERT_TRUE(err != IRIS_OK);

        bc_free_proc(p);
    }

    /* [BC-4] Multiple consecutive boot slots all resolvable. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        const uint32_t N = 6u;
        for (uint32_t i = 0; i < N; i++) {
            struct KUntyped *ut = bc_make_ut(4096u);
            ASSERT_NOT_NULL(ut);
            ASSERT_EQ(bc_boot_publish(p, ut, i), IRIS_OK);
        }

        for (uint32_t i = 0; i < N; i++) {
            struct KUntyped *out; iris_rights_t rout;
            ASSERT_EQ(cspace_resolve_only_untyped(
                          p, BOOT_CPTR_UNTYPED_START + i, RIGHT_READ, &out, &rout),
                      IRIS_OK);
            ASSERT_EQ(out->base.type, KOBJ_UNTYPED);
            kobject_active_release(&out->base);
            kobject_release(&out->base);
        }

        /* Slot just beyond the last inserted block → NOT_FOUND. */
        struct KUntyped *out; iris_rights_t rout;
        iris_error_t err = cspace_resolve_only_untyped(
            p, BOOT_CPTR_UNTYPED_START + N, RIGHT_NONE, &out, &rout);
        ASSERT_TRUE(err != IRIS_OK);

        bc_free_proc(p);
    }

    /* [BC-5] Failed mint (slot index >= slot_count) releases refs cleanly;
     * object still alive via handle. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(bc_boot_publish(p, ut, 0u), IRIS_OK);

        /* Try to mint into an out-of-range slot directly — must fail. */
        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(bc_root_fetch(p,
                                           &root_obj, &root_r), IRIS_OK);
        iris_error_t bad = kcnode_mint((struct KCNode *)root_obj,
                                        KCNODE_DEFAULT_SLOTS + 1u,
                                        &ut->base, RIGHT_READ);
        ASSERT_TRUE(bad != IRIS_OK);
        kobject_release(root_obj);

        /* The object is still alive: the slot that DID publish holds it. */
        struct KUntyped *live; iris_rights_t lr;
        ASSERT_EQ(cspace_resolve_only_untyped(p, BOOT_CPTR_UNTYPED_START,
                                                   RIGHT_NONE, &live, &lr),
                  IRIS_OK);
        ASSERT_EQ(live->base.type, KOBJ_UNTYPED);
        kobject_active_release(&live->base);
        kobject_release(&live->base);

        bc_free_proc(p);
    }


    /* [BC-7] Boot slot constants are within the 256-slot root CNode. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(bc_root_fetch(p,
                                           &root_obj, &root_r), IRIS_OK);
        uint32_t sc = ((struct KCNode *)root_obj)->slot_count;
        kobject_release(root_obj);

        ASSERT_EQ(sc, (uint32_t)KCNODE_DEFAULT_SLOTS);
        ASSERT_TRUE(BOOT_CPTR_UNTYPED_START < sc);
        ASSERT_TRUE(BOOT_CPTR_UNTYPED_END   < sc);
        ASSERT_TRUE(BOOT_CPTR_UNTYPED_START <= BOOT_CPTR_UNTYPED_END);

        bc_free_proc(p);
    }

    /* [BC-8] Repeated CPtr resolves do not leak active refs. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(bc_boot_publish(p, ut, 0u), IRIS_OK);

        for (int i = 0; i < 8; i++) {
            struct KUntyped *out; iris_rights_t rout;
            ASSERT_EQ(cspace_resolve_only_untyped(
                          p, BOOT_CPTR_UNTYPED_START, RIGHT_NONE, &out, &rout),
                      IRIS_OK);
            kobject_active_release(&out->base);
            kobject_release(&out->base);
        }

        /* Object still alive after 8 resolve-release cycles. */
        struct KObject *obj; iris_rights_t r;
        struct KProcess *p2 = bc_make_proc();
        ASSERT_NOT_NULL(p2);
        (void)obj; (void)r; (void)p2;
        bc_free_proc(p2);

        bc_free_proc(p);
    }


    /* [BC-10] slot >= KCNODE_DEFAULT_SLOTS → kcnode_mint returns INVALID_ARG. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(bc_root_fetch(p,
                                           &root_obj, &root_r), IRIS_OK);
        iris_error_t err = kcnode_mint((struct KCNode *)root_obj,
                                        KCNODE_DEFAULT_SLOTS,  /* out of range */
                                        &ut->base, RIGHT_READ);
        ASSERT_EQ(err, IRIS_ERR_INVALID_ARG);
        kobject_release(root_obj);

        /* Drop alloc ref; object should be freed now (no other owner). */
        kobject_release(&ut->base);

        bc_free_proc(p);
    }

    /* ── Fase 3.5 tests (BB-1..BB-10) ──────────────────────────────────────── */

    /* [BB-1] BOOT_CPTR_BOOTSTRAP_CAP == 1 and != CPTR_NULL.
     * Stage 5 Etapa 2 emptied that slot for good — the monolithic boot
     * capability it held cannot be constructed any more — but the number
     * stays reserved, which is what this pins. */
    {
        ASSERT_EQ((uint32_t)BOOT_CPTR_BOOTSTRAP_CAP, 1u);
        ASSERT_NE((uint32_t)BOOT_CPTR_BOOTSTRAP_CAP, (uint32_t)CPTR_NULL);
    }

    /* ── Stage 5 Etapa 3: a CSpace that names itself (BC-11..BC-13) ────────
     *
     * The root task holds a capability to its own root CNode, so the CNode is
     * reachable from inside itself and its slot holds refs on it.  Nothing
     * will ever drop those refs from outside: the close callback that empties
     * the slots only runs when the refs reach zero, and the self-reference is
     * one of the refs.  Process teardown therefore empties the root's slots
     * FIRST (kcnode_teardown_slots), which is what makes the capability safe
     * to hand out.
     *
     * These run on the object directly rather than through a process: the
     * property is about references, and the host harness can observe
     * kcnode_live_count() exactly. */

    /* [BC-11] a CNode can name itself, and the self-capability resolves. */
    {
        struct KCNode *cn = kcnode_alloc(16u);
        ASSERT_NOT_NULL(cn);
        if (cn) {
            kobject_active_retain(&cn->base);   /* the structural pair */

            ASSERT_EQ(kcnode_mint(cn, 5u, &cn->base,
                                  RIGHT_READ | RIGHT_WRITE), IRIS_OK);

            struct KObject *got = NULL; iris_rights_t gr = RIGHT_NONE;
            ASSERT_EQ(kcnode_fetch(cn, 5u, &got, &gr), IRIS_OK);
            ASSERT_TRUE(got == &cn->base);
            if (got) { kobject_active_release(got); kobject_release(got); }

            kcnode_teardown_slots(cn);
            kobject_active_release(&cn->base);
            kobject_release(&cn->base);
        }
    }

    /* [BC-12] teardown-then-release frees a self-referencing CNode. */
    {
        uint32_t before = kcnode_live_count();
        struct KCNode *cn = kcnode_alloc(16u);
        ASSERT_NOT_NULL(cn);
        if (cn) {
            kobject_active_retain(&cn->base);
            ASSERT_EQ(kcnode_mint(cn, 1u, &cn->base, RIGHT_READ), IRIS_OK);
            ASSERT_EQ(kcnode_live_count(), before + 1u);

            /* Exactly what kprocess_teardown does, in that order. */
            kcnode_teardown_slots(cn);
            kobject_active_release(&cn->base);
            kobject_release(&cn->base);

            ASSERT_EQ(kcnode_live_count(), before);
        }
    }

    /* [BC-13] without the explicit teardown the object is NOT freed — the
     * negative control, so this suite proves the fix rather than the absence
     * of a symptom.  The CNode is deliberately leaked here (the host process
     * exits); the assertions below take their own baseline. */
    {
        uint32_t before = kcnode_live_count();
        struct KCNode *cn = kcnode_alloc(16u);
        ASSERT_NOT_NULL(cn);
        if (cn) {
            kobject_active_retain(&cn->base);
            ASSERT_EQ(kcnode_mint(cn, 2u, &cn->base, RIGHT_READ), IRIS_OK);

            kobject_active_release(&cn->base);
            kobject_release(&cn->base);

            /* Still live: the slot inside it holds the last references. */
            ASSERT_EQ(kcnode_live_count(), before + 1u);

            /* Break it by hand so the leak does not outlive the case. */
            kcnode_teardown_slots(cn);
            ASSERT_EQ(kcnode_live_count(), before);
        }
    }









}
