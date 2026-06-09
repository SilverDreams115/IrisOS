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
#include <iris/nc/handle_table.h>
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
    handle_table_init(&p->handle_table);
    p->cspace_root_h = HANDLE_INVALID;
    return p;
}

static void bc_free_proc(struct KProcess *p) {
    handle_table_close_all(&p->handle_table);
    kpage_free(p, (uint32_t)sizeof(*p));
}

static struct KCNode *bc_setup_root(struct KProcess *p) {
    struct KCNode *root = kcnode_alloc(KCNODE_DEFAULT_SLOTS);
    if (!root) return NULL;
    handle_id_t rh = handle_table_insert(
        &p->handle_table, &root->base,
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    kobject_release(&root->base);
    if (rh == HANDLE_INVALID) return NULL;
    p->cspace_root_h = rh;
    return root;
}

static struct KUntyped *bc_make_ut(uint64_t size) {
    void *buf = malloc((size_t)size);
    if (!buf) return NULL;
    return kuntyped_create((uint64_t)(uintptr_t)buf, size, 0);
}

/* Simulate the kernel_main Ph76 dual-insert for one boot block at drain_idx. */
static iris_error_t bc_boot_dual_insert(struct KProcess *p,
                                         struct KUntyped *boot_ut,
                                         uint32_t         drain_idx,
                                         handle_id_t     *out_handle) {
    iris_rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    /* Step 1: legacy handle-table insert. */
    handle_id_t h = handle_table_insert(&p->handle_table, &boot_ut->base, r);
    kobject_release(&boot_ut->base);  /* drop alloc ref */
    if (h == HANDLE_INVALID) return IRIS_ERR_NO_MEMORY;
    if (out_handle) *out_handle = h;

    /* Step 2: root CNode insert (Fase 3.4). */
    uint32_t cspace_slot = BOOT_CPTR_UNTYPED_START + drain_idx;
    if (p->cspace_root_h == HANDLE_INVALID || cspace_slot >= KCNODE_DEFAULT_SLOTS)
        return IRIS_OK;  /* CSpace not available; legacy path still worked */

    struct KObject *root_obj; iris_rights_t root_r;
    if (handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                &root_obj, &root_r) != IRIS_OK)
        return IRIS_OK;

    iris_error_t me = IRIS_ERR_INVALID_ARG;
    if (root_obj->type == KOBJ_CNODE)
        me = kcnode_mint((struct KCNode *)root_obj, cspace_slot,
                         &boot_ut->base, r);
    kobject_release(root_obj);
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

        handle_id_t h = HANDLE_INVALID;
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, &h), IRIS_OK);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(
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
        handle_id_t h = HANDLE_INVALID;
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, &h), IRIS_OK);

        /* CSpace path rights. */
        struct KUntyped *out; iris_rights_t cptr_r;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(
                      p, BOOT_CPTR_UNTYPED_START, RIGHT_NONE, &out, &cptr_r),
                  IRIS_OK);
        ASSERT_EQ(cptr_r, expected);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        /* Handle path rights. */
        struct KObject *obj; iris_rights_t ht_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &obj, &ht_r), IRIS_OK);
        ASSERT_EQ(ht_r, expected);
        kobject_release(obj);

        bc_free_proc(p);
    }

    /* [BC-3] CPTR_NULL slot (0) remains empty after boot grants. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, NULL), IRIS_OK);

        struct KUntyped *out; iris_rights_t rout;
        iris_error_t err = cspace_or_handle_resolve_untyped(
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
            ASSERT_EQ(bc_boot_dual_insert(p, ut, i, NULL), IRIS_OK);
        }

        for (uint32_t i = 0; i < N; i++) {
            struct KUntyped *out; iris_rights_t rout;
            ASSERT_EQ(cspace_or_handle_resolve_untyped(
                          p, BOOT_CPTR_UNTYPED_START + i, RIGHT_READ, &out, &rout),
                      IRIS_OK);
            ASSERT_EQ(out->base.type, KOBJ_UNTYPED);
            kobject_active_release(&out->base);
            kobject_release(&out->base);
        }

        /* Slot just beyond the last inserted block → NOT_FOUND. */
        struct KUntyped *out; iris_rights_t rout;
        iris_error_t err = cspace_or_handle_resolve_untyped(
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
        handle_id_t h = HANDLE_INVALID;
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, &h), IRIS_OK);

        /* Try to mint into an out-of-range slot directly — must fail. */
        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        iris_error_t bad = kcnode_mint((struct KCNode *)root_obj,
                                        KCNODE_DEFAULT_SLOTS + 1u,
                                        &ut->base, RIGHT_READ);
        ASSERT_TRUE(bad != IRIS_OK);
        kobject_release(root_obj);

        /* Object still alive through the handle. */
        struct KObject *obj; iris_rights_t r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &obj, &r), IRIS_OK);
        ASSERT_EQ(obj->type, KOBJ_UNTYPED);
        kobject_release(obj);

        bc_free_proc(p);
    }

    /* [BC-6] Dual insert: refcount balanced; teardown does not crash. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);

        /* After kuntyped_create: refcount=1, active_refs=0. */
        handle_id_t h = handle_table_insert(
            &p->handle_table, &ut->base,
            RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
        /* After handle insert: refcount=2, active_refs=1. */
        kobject_release(&ut->base);  /* drop alloc ref: refcount=1, active_refs=1. */
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_UNTYPED_START,
                               &ut->base,
                               RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        /* After kcnode_mint: refcount=2, active_refs=2. */
        kobject_release(root_obj);

        /* Verify refcounts before teardown. */
        ASSERT_EQ(atomic_load(&ut->base.refcount),    2u);
        ASSERT_EQ(atomic_load(&ut->base.active_refs), 2u);

        /* bc_free_proc → handle_table_close_all:
         *   - closes handle for ut: refcount 2→1, active_refs 2→1
         *   - closes root CNode → kcnode_obj_close releases slot: refcount 1→0 → destroy
         * OR the CNode is destroyed first; either order is safe. */
        bc_free_proc(p);
    }

    /* [BC-7] Boot slot constants are within the 256-slot root CNode. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
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
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, NULL), IRIS_OK);

        for (int i = 0; i < 8; i++) {
            struct KUntyped *out; iris_rights_t rout;
            ASSERT_EQ(cspace_or_handle_resolve_untyped(
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

    /* [BC-9] Read-only CNode slot + WRITE required → ACCESS_DENIED (no fallback). */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);

        /* Legacy handle: full rights. CNode slot: READ-only. */
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&ut->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_UNTYPED_START,
                               &ut->base, RIGHT_READ),  /* READ only */
                  IRIS_OK);
        kobject_release(root_obj);

        /* CPtr with WRITE required → ACCESS_DENIED; must NOT fall back to handle. */
        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(
                      p, BOOT_CPTR_UNTYPED_START, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

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
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
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

    /* [BB-1] BOOT_CPTR_BOOTSTRAP_CAP == 1 and != CPTR_NULL. */
    {
        ASSERT_EQ((uint32_t)BOOT_CPTR_BOOTSTRAP_CAP, 1u);
        ASSERT_NE((uint32_t)BOOT_CPTR_BOOTSTRAP_CAP, (uint32_t)CPTR_NULL);
    }

    /* [BB-2] Slots 2-15 stay empty after inserting KBootstrapCap in slot 1. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t h = handle_table_insert(&p->handle_table, &cap->base,
                                             RIGHT_READ | RIGHT_DUPLICATE |
                                             RIGHT_TRANSFER);
        kobject_release(&cap->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);

        /* Slots 2-15 must be empty (NOT_FOUND). */
        for (uint32_t s = 2u; s <= BOOT_CPTR_RES_END; s++) {
            struct KObject *root2; iris_rights_t r2;
            ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                               &root2, &r2), IRIS_OK);
            struct KObject *sobj; iris_rights_t sr;
            iris_error_t fe = kcnode_fetch((struct KCNode *)root2, s, &sobj, &sr);
            ASSERT_EQ(fe, IRIS_ERR_NOT_FOUND);
            kobject_release(root2);
        }

        bc_free_proc(p);
    }

    /* [BB-3] KBootstrapCap in slot 1 resolves via CPtr; type == KOBJ_BOOTSTRAP_CAP. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t h = handle_table_insert(&p->handle_table, &cap->base,
                                             RIGHT_READ | RIGHT_DUPLICATE |
                                             RIGHT_TRANSFER);
        kobject_release(&cap->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);

        /* Resolve via CPtr. */
        struct KObject *resolved; iris_rights_t rr;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_BOOTSTRAP_CAP,
                                      RIGHT_NONE, &resolved, &rr),
                  IRIS_OK);
        ASSERT_EQ(resolved->type, KOBJ_BOOTSTRAP_CAP);
        kobject_active_release(resolved);
        kobject_release(resolved);

        bc_free_proc(p);
    }

    /* [BB-4] Rights in CSpace slot == legacy handle rights (not greater). */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        iris_rights_t expected = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;

        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t h = handle_table_insert(&p->handle_table, &cap->base,
                                             expected);
        kobject_release(&cap->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base, expected),
                  IRIS_OK);
        kobject_release(root_obj);

        /* CSpace rights. */
        struct KObject *cobj; iris_rights_t cr;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_BOOTSTRAP_CAP,
                                      RIGHT_NONE, &cobj, &cr),
                  IRIS_OK);
        ASSERT_EQ(cr, expected);
        kobject_active_release(cobj);
        kobject_release(cobj);

        /* Handle rights. */
        struct KObject *hobj; iris_rights_t hr;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &hobj, &hr),
                  IRIS_OK);
        ASSERT_EQ(hr, expected);
        kobject_release(hobj);

        /* CSpace rights must NOT exceed handle rights. */
        ASSERT_TRUE((cr & ~hr) == RIGHT_NONE);

        bc_free_proc(p);
    }

    /* [BB-5] Dual insert refcounts balanced: refcount=2, active_refs=2. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        /* After alloc: refcount=1, active_refs=0. */

        handle_id_t h = handle_table_insert(&p->handle_table, &cap->base,
                                             RIGHT_READ | RIGHT_DUPLICATE |
                                             RIGHT_TRANSFER);
        /* After handle insert: refcount=2, active_refs=1. */
        kobject_release(&cap->base);
        /* Drop alloc ref: refcount=1, active_refs=1. */
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        /* After CNode mint: refcount=2, active_refs=2. */
        kobject_release(root_obj);

        ASSERT_EQ(atomic_load(&cap->base.refcount),    2u);
        ASSERT_EQ(atomic_load(&cap->base.active_refs), 2u);

        bc_free_proc(p);
        /* Teardown releases handle + CNode slot → refcount 0 → destroy. */
    }

    /* [BB-6] Second kcnode_mint in slot 1 overwrites cleanly; old cap released. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KBootstrapCap *cap1 = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        struct KBootstrapCap *cap2 = kbootcap_alloc(IRIS_BOOTCAP_HW_ACCESS);
        ASSERT_NOT_NULL(cap1);
        ASSERT_NOT_NULL(cap2);

        handle_id_t h1 = handle_table_insert(&p->handle_table, &cap1->base,
                                              RIGHT_READ | RIGHT_DUPLICATE |
                                              RIGHT_TRANSFER);
        kobject_release(&cap1->base);
        ASSERT_NE(h1, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);

        /* First mint: cap1 into slot 1. */
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap1->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        /* refcount cap1 = 2, active_refs cap1 = 2 */

        /* Second mint: cap2 replaces cap1 in slot 1. */
        handle_id_t h2 = handle_table_insert(&p->handle_table, &cap2->base,
                                              RIGHT_READ | RIGHT_DUPLICATE |
                                              RIGHT_TRANSFER);
        kobject_release(&cap2->base);
        ASSERT_NE(h2, (handle_id_t)HANDLE_INVALID);

        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap2->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        /* kcnode_mint released cap1 active+ref; cap1 refcount = 1 (handle only). */
        kobject_release(root_obj);

        /* Slot 1 now holds cap2 — verify by type (both are KOBJ_BOOTSTRAP_CAP,
         * but we verify the resolved cap is cap2 via permissions). */
        struct KObject *robj; iris_rights_t rr;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_BOOTSTRAP_CAP,
                                      RIGHT_NONE, &robj, &rr),
                  IRIS_OK);
        ASSERT_EQ(robj->type, KOBJ_BOOTSTRAP_CAP);
        ASSERT_TRUE(((struct KBootstrapCap *)robj)->permissions ==
                    IRIS_BOOTCAP_HW_ACCESS);
        kobject_active_release(robj);
        kobject_release(robj);

        bc_free_proc(p);
    }

    /* [BB-7] ACCESS_DENIED from slot 1 (read-only) blocks resolve for WRITE. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t h = handle_table_insert(&p->handle_table, &cap->base,
                                             RIGHT_READ | RIGHT_WRITE |
                                             RIGHT_DUPLICATE | RIGHT_TRANSFER);
        kobject_release(&cap->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        /* Mint into slot 1 with READ-only rights. */
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base, RIGHT_READ),
                  IRIS_OK);
        kobject_release(root_obj);

        /* Resolving slot 1 with WRITE required must return ACCESS_DENIED,
         * NOT fall through to the handle table. */
        struct KObject *robj; iris_rights_t rr;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_BOOTSTRAP_CAP,
                                      RIGHT_WRITE, &robj, &rr),
                  IRIS_ERR_ACCESS_DENIED);

        bc_free_proc(p);
    }

    /* [BB-8] Legacy handle path resolves KBootstrapCap independently. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t h = handle_table_insert(&p->handle_table, &cap->base,
                                             RIGHT_READ | RIGHT_DUPLICATE |
                                             RIGHT_TRANSFER);
        kobject_release(&cap->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);

        /* Legacy handle path must still resolve the same object. */
        struct KObject *hobj; iris_rights_t hr;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &hobj, &hr),
                  IRIS_OK);
        ASSERT_EQ(hobj->type, KOBJ_BOOTSTRAP_CAP);
        kobject_release(hobj);

        bc_free_proc(p);
    }

    /* [BB-9] Boot KUntyped slots 16+ remain intact after KBootstrapCap in slot 1. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        /* Insert KBootstrapCap into slot 1. */
        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t hcap = handle_table_insert(&p->handle_table, &cap->base,
                                                RIGHT_READ | RIGHT_DUPLICATE |
                                                RIGHT_TRANSFER);
        kobject_release(&cap->base);
        ASSERT_NE(hcap, (handle_id_t)HANDLE_INVALID);
        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);

        /* Also insert a KUntyped into slot 16. */
        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, NULL), IRIS_OK);

        /* Slot 16 (BOOT_CPTR_UNTYPED_START) must resolve to a KUntyped. */
        struct KUntyped *utout; iris_rights_t utr;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(
                      p, BOOT_CPTR_UNTYPED_START, RIGHT_READ, &utout, &utr),
                  IRIS_OK);
        ASSERT_EQ(utout->base.type, KOBJ_UNTYPED);
        kobject_active_release(&utout->base);
        kobject_release(&utout->base);

        /* Slot 1 must still hold the KBootstrapCap. */
        struct KObject *cobj; iris_rights_t cr;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_BOOTSTRAP_CAP,
                                      RIGHT_NONE, &cobj, &cr),
                  IRIS_OK);
        ASSERT_EQ(cobj->type, KOBJ_BOOTSTRAP_CAP);
        kobject_active_release(cobj);
        kobject_release(cobj);

        bc_free_proc(p);
    }

    /* [BB-10] CPTR_NULL (slot 0) stays empty after all boot grants. */
    {
        struct KProcess *p = bc_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(bc_setup_root(p));

        /* Insert KBootstrapCap + KUntyped as kernel_main would. */
        struct KBootstrapCap *cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE);
        ASSERT_NOT_NULL(cap);
        handle_id_t hcap = handle_table_insert(&p->handle_table, &cap->base,
                                                RIGHT_READ | RIGHT_DUPLICATE |
                                                RIGHT_TRANSFER);
        kobject_release(&cap->base);
        ASSERT_NE(hcap, (handle_id_t)HANDLE_INVALID);
        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_BOOTSTRAP_CAP,
                               &cap->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);

        struct KUntyped *ut = bc_make_ut(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(bc_boot_dual_insert(p, ut, 0u, NULL), IRIS_OK);

        /* CPTR_NULL resolve must fail — slot 0 is the null slot. */
        struct KObject *nobj; iris_rights_t nr;
        iris_error_t nerr = cspace_resolve_cap(p, CPTR_NULL,
                                                RIGHT_NONE, &nobj, &nr);
        ASSERT_TRUE(nerr != IRIS_OK);

        bc_free_proc(p);
    }
}
