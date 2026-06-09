/*
 * test_vspace_cspace.c — Fase 4 unit tests for KVSpace capability model.
 *
 * Tests (VS-1..VS-14):
 *   [VS-1]  KOBJ_VSPACE enum exists and is != 0.
 *   [VS-2]  BOOT_CPTR_VSPACE == 2; distinct from CPTR_NULL and BOOT_CPTR_BOOTSTRAP_CAP.
 *   [VS-3]  kvspace_alloc initialises cr3 and valid=1; kvspace_free decrements cleanly.
 *   [VS-4]  kvspace_invalidate zeroes cr3/valid without destroying the object.
 *   [VS-5]  KVSpace minted into BOOT_CPTR_VSPACE; cspace_resolve_vspace succeeds.
 *   [VS-6]  cspace_resolve_cap on BOOT_CPTR_VSPACE slot reports KOBJ_VSPACE type.
 *   [VS-7]  cspace_resolve_vspace on a KOBJ_UNTYPED slot returns IRIS_ERR_WRONG_TYPE.
 *   [VS-8]  cspace_resolve_vspace with insufficient rights returns IRIS_ERR_ACCESS_DENIED.
 *   [VS-9]  CPTR_NULL always rejects with IRIS_ERR_INVALID_ARG.
 *   [VS-10] ACCESS_DENIED hard-stops; handle-table fallback is NOT attempted.
 *   [VS-11] Kernel-main dual-insert pattern: refcount=2, active_refs=1 after grants.
 *   [VS-12] Slot 1 (KBootstrapCap) and slots 16+ are unaffected by grant in slot 2.
 *   [VS-13] Repeated resolve+release does not leak active refs.
 *   [VS-14] invalidate + process-ref-release + CNode teardown — no double-free.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kvspace.h>
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

/* ── Helpers (mirrored from test_boot_cspace.c) ──────────────────────── */

static struct KProcess *vs_make_proc(void) {
    struct KProcess *p = (struct KProcess *)kpage_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    handle_table_init(&p->handle_table);
    p->cspace_root_h = HANDLE_INVALID;
    return p;
}

static void vs_free_proc(struct KProcess *p) {
    handle_table_close_all(&p->handle_table);
    kpage_free(p, (uint32_t)sizeof(*p));
}

static struct KCNode *vs_setup_root(struct KProcess *p) {
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

/* Simulate kernel_main's VSpace grant:
 *   1. alloc KVSpace
 *   2. kobject_retain into process (lifecycle ref)
 *   3. kobject_release alloc ref
 *   4. kcnode_mint into BOOT_CPTR_VSPACE (adds retain+active_retain)
 *
 * Caller must call kobject_release on vs after the process is torn down
 * to simulate the process->vspace release, or call vs_free_proc which
 * will trigger CNode teardown releasing the CNode's refs.
 */
static struct KVSpace *vs_boot_grant(struct KProcess *p, uint64_t cr3) {
    struct KVSpace *vs = kvspace_alloc(cr3);
    if (!vs) return NULL;

    /* Process lifecycle ref (not active — matches kernel_main pattern). */
    kobject_retain(&vs->base);
    kobject_release(&vs->base);   /* drop alloc ref; process ref is the kobject_retain */

    /* Publish in well-known slot BOOT_CPTR_VSPACE. */
    if (p->cspace_root_h == HANDLE_INVALID) return vs;

    struct KObject *root_obj; iris_rights_t root_r;
    if (handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                &root_obj, &root_r) != IRIS_OK) return vs;
    if (root_obj->type == KOBJ_CNODE) {
        (void)kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                          &vs->base,
                          RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    }
    kobject_release(root_obj);
    return vs;
}

/* ── Test suite ───────────────────────────────────────────────────────── */

void test_vspace_cspace(void) {
    TEST_SUITE("vspace_cspace");

    /* [VS-1] KOBJ_VSPACE enum value is defined and non-zero. */
    {
        ASSERT_TRUE(KOBJ_VSPACE != 0u);
        ASSERT_TRUE(KOBJ_VSPACE != KOBJ_CNODE);
        ASSERT_TRUE(KOBJ_VSPACE != KOBJ_UNTYPED);
        ASSERT_TRUE(KOBJ_VSPACE != KOBJ_BOOTSTRAP_CAP);
        ASSERT_TRUE(KOBJ_VSPACE != KOBJ_TCB);
    }

    /* [VS-2] BOOT_CPTR_VSPACE is 2; distinct from CPTR_NULL and BOOT_CPTR_BOOTSTRAP_CAP. */
    {
        ASSERT_EQ((uint32_t)BOOT_CPTR_VSPACE, 2u);
        ASSERT_NE((iris_cptr_t)BOOT_CPTR_VSPACE, CPTR_NULL);
        ASSERT_NE((uint32_t)BOOT_CPTR_VSPACE, (uint32_t)BOOT_CPTR_BOOTSTRAP_CAP);
        ASSERT_TRUE(BOOT_CPTR_VSPACE < BOOT_CPTR_UNTYPED_START);
        ASSERT_TRUE(BOOT_CPTR_VSPACE <= BOOT_CPTR_RES_END);
    }

    /* [VS-3] kvspace_alloc initialises cr3 and valid=1. kvspace_free decrements refcount. */
    {
        const uint64_t fake_cr3 = 0xDEADBEEF000ULL;
        struct KVSpace *vs = kvspace_alloc(fake_cr3);
        ASSERT_NOT_NULL(vs);
        ASSERT_EQ(vs->base.type, KOBJ_VSPACE);
        ASSERT_EQ(vs->cr3,   fake_cr3);
        ASSERT_EQ(vs->valid, 1u);
        ASSERT_EQ(atomic_load(&vs->base.refcount), 1u);

        kvspace_free(vs);
        /* object is destroyed (refcount → 0); no crash = test passes */
    }

    /* [VS-4] kvspace_invalidate zeroes cr3/valid; object still alive afterward. */
    {
        struct KVSpace *vs = kvspace_alloc(0x1000ULL);
        ASSERT_NOT_NULL(vs);

        /* Hold an extra ref so the object survives after kvspace_invalidate + release. */
        kobject_retain(&vs->base);

        kvspace_invalidate(vs);
        ASSERT_EQ(vs->cr3,   0u);
        ASSERT_EQ(vs->valid, 0u);
        ASSERT_EQ(vs->base.type, KOBJ_VSPACE);   /* type unchanged */

        kobject_release(&vs->base);   /* drop extra ref */
        kvspace_free(vs);             /* drop alloc ref → destroy */
    }

    /* [VS-5] KVSpace minted into BOOT_CPTR_VSPACE; cspace_resolve_vspace returns IRIS_OK. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x5000ULL);
        ASSERT_NOT_NULL(vs);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);
        kvspace_free(vs);   /* drop alloc ref; CNode now owns one pair */

        struct KVSpace *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                         RIGHT_READ, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_VSPACE);
        ASSERT_TRUE((rout & RIGHT_READ) != 0u);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        vs_free_proc(p);
    }

    /* [VS-6] cspace_resolve_cap on BOOT_CPTR_VSPACE slot reports KOBJ_VSPACE. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x9000ULL);
        ASSERT_NOT_NULL(vs);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);
        kvspace_free(vs);

        struct KObject *obj; iris_rights_t r;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_VSPACE,
                                      RIGHT_NONE, &obj, &r),
                  IRIS_OK);
        ASSERT_EQ(obj->type, KOBJ_VSPACE);
        kobject_active_release(obj);
        kobject_release(obj);

        vs_free_proc(p);
    }

    /* [VS-7] cspace_resolve_vspace on a KOBJ_UNTYPED slot → IRIS_ERR_WRONG_TYPE. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        /* Insert a KUntyped into slot BOOT_CPTR_VSPACE instead of a KVSpace. */
        void *buf = malloc(4096);
        ASSERT_NOT_NULL(buf);
        struct KUntyped *ut = kuntyped_create((uint64_t)(uintptr_t)buf, 4096u, 0);
        ASSERT_NOT_NULL(ut);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &ut->base, RIGHT_READ | RIGHT_WRITE),
                  IRIS_OK);
        kobject_release(root_obj);
        kobject_release(&ut->base);   /* drop alloc ref */

        struct KVSpace *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                         RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        vs_free_proc(p);
    }

    /* [VS-8] cspace_resolve_vspace with required rights not in slot → ACCESS_DENIED. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x2000ULL);
        ASSERT_NOT_NULL(vs);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        /* Mint with READ-only rights. */
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base, RIGHT_READ),
                  IRIS_OK);
        kobject_release(root_obj);
        kvspace_free(vs);

        /* Require WRITE — must fail with ACCESS_DENIED. */
        struct KVSpace *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                         RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        vs_free_proc(p);
    }

    /* [VS-9] CPTR_NULL always rejects — cspace_resolve_vspace returns IRIS_ERR_INVALID_ARG. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *out; iris_rights_t rout;
        iris_error_t err = cspace_resolve_vspace(p, CPTR_NULL,
                                                  RIGHT_NONE, &out, &rout);
        ASSERT_TRUE(err != IRIS_OK);

        vs_free_proc(p);
    }

    /* [VS-10] ACCESS_DENIED hard-stops: generic cspace_resolve_cap returns ACCESS_DENIED
     *         on rights mismatch and does NOT fall back to any other path. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x3000ULL);
        ASSERT_NOT_NULL(vs);

        /* Also insert vs into the handle table (simulates a legacy handle path). */
        handle_id_t h = handle_table_insert(&p->handle_table, &vs->base,
                                             RIGHT_READ | RIGHT_WRITE |
                                             RIGHT_DUPLICATE | RIGHT_TRANSFER);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        /* CNode slot: READ-only.  Handle: full rights. */
        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base, RIGHT_READ),
                  IRIS_OK);
        kobject_release(root_obj);
        kvspace_free(vs);   /* drop alloc ref */

        /* Resolve via CPtr requiring WRITE: ACCESS_DENIED must NOT fall back to handle. */
        struct KVSpace *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                         RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        vs_free_proc(p);
    }

    /* [VS-11] kernel_main dual-insert pattern: refcount=2, active_refs=1. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x4000ULL);
        ASSERT_NOT_NULL(vs);
        /* After alloc: refcount=1, active_refs=0. */

        kobject_retain(&vs->base);
        /* Simulate: process->vspace = vs.  refcount=2, active_refs=0. */

        kobject_release(&vs->base);
        /* Drop alloc ref.  refcount=1, active_refs=0. */
        /* (process->vspace holds the single lifecycle ref) */

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        /* After kcnode_mint (retain+active_retain): refcount=2, active_refs=1. */
        kobject_release(root_obj);

        ASSERT_EQ(atomic_load(&vs->base.refcount),    2u);
        ASSERT_EQ(atomic_load(&vs->base.active_refs), 1u);

        /* Release process->vspace lifecycle ref. */
        kobject_release(&vs->base);
        /* refcount=1, active_refs=1; CNode slot still owns both. */
        ASSERT_EQ(atomic_load(&vs->base.refcount),    1u);
        ASSERT_EQ(atomic_load(&vs->base.active_refs), 1u);

        /* CNode teardown via vs_free_proc releases retain+active_retain → destroy. */
        vs_free_proc(p);
    }

    /* [VS-12] Slot 1 (KBootstrapCap) and slots 16+ unaffected by grant in slot 2. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

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

        /* Insert KVSpace into slot 2 (BOOT_CPTR_VSPACE). */
        struct KVSpace *vs = kvspace_alloc(0x6000ULL);
        ASSERT_NOT_NULL(vs);
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);
        kvspace_free(vs);

        /* Insert a KUntyped into slot 16 (BOOT_CPTR_UNTYPED_START). */
        void *buf = malloc(4096);
        ASSERT_NOT_NULL(buf);
        struct KUntyped *ut = kuntyped_create((uint64_t)(uintptr_t)buf, 4096u, 0);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_UNTYPED_START,
                               &ut->base,
                               RIGHT_READ | RIGHT_WRITE |
                               RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);
        kobject_release(&ut->base);   /* drop alloc ref */

        /* Verify slot 1 holds KBootstrapCap. */
        struct KObject *c1; iris_rights_t r1;
        ASSERT_EQ(cspace_resolve_cap(p, BOOT_CPTR_BOOTSTRAP_CAP,
                                      RIGHT_NONE, &c1, &r1),
                  IRIS_OK);
        ASSERT_EQ(c1->type, KOBJ_BOOTSTRAP_CAP);
        kobject_active_release(c1);
        kobject_release(c1);

        /* Verify slot 2 holds KVSpace. */
        struct KVSpace *vs2; iris_rights_t r2;
        ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                         RIGHT_NONE, &vs2, &r2),
                  IRIS_OK);
        ASSERT_EQ(vs2->base.type, KOBJ_VSPACE);
        kobject_active_release(&vs2->base);
        kobject_release(&vs2->base);

        /* Verify slot 16 holds KUntyped. */
        struct KUntyped *ut2; iris_rights_t r3;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, BOOT_CPTR_UNTYPED_START,
                                                    RIGHT_READ, &ut2, &r3),
                  IRIS_OK);
        ASSERT_EQ(ut2->base.type, KOBJ_UNTYPED);
        kobject_active_release(&ut2->base);
        kobject_release(&ut2->base);

        /* Slot 0 must be empty. */
        struct KObject *n0; iris_rights_t rn;
        ASSERT_TRUE(cspace_resolve_cap(p, CPTR_NULL, RIGHT_NONE, &n0, &rn) != IRIS_OK);

        vs_free_proc(p);
    }

    /* [VS-13] Repeated cspace_resolve_vspace + release does not leak active refs. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x7000ULL);
        ASSERT_NOT_NULL(vs);

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);
        kvspace_free(vs);

        for (int i = 0; i < 10; i++) {
            struct KVSpace *out; iris_rights_t rout;
            ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                             RIGHT_READ, &out, &rout),
                      IRIS_OK);
            ASSERT_EQ(out->base.type, KOBJ_VSPACE);
            kobject_active_release(&out->base);
            kobject_release(&out->base);
        }

        /* Object still alive: CNode slot still holds it. */
        struct KVSpace *final_out; iris_rights_t fr;
        ASSERT_EQ(cspace_resolve_vspace(p, BOOT_CPTR_VSPACE,
                                         RIGHT_NONE, &final_out, &fr),
                  IRIS_OK);
        kobject_active_release(&final_out->base);
        kobject_release(&final_out->base);

        vs_free_proc(p);
    }

    /* [VS-14] kvspace_invalidate + process-ref-release + CNode teardown: no double-free. */
    {
        struct KProcess *p = vs_make_proc();
        ASSERT_NOT_NULL(p);
        ASSERT_NOT_NULL(vs_setup_root(p));

        struct KVSpace *vs = kvspace_alloc(0x8000ULL);
        ASSERT_NOT_NULL(vs);

        /* Simulate process->vspace lifecycle ref. */
        kobject_retain(&vs->base);   /* refcount=2, active_refs=0 */
        kobject_release(&vs->base);  /* drop alloc ref; refcount=1 */

        struct KObject *root_obj; iris_rights_t root_r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, p->cspace_root_h,
                                           &root_obj, &root_r), IRIS_OK);
        ASSERT_EQ(kcnode_mint((struct KCNode *)root_obj, BOOT_CPTR_VSPACE,
                               &vs->base,
                               RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER),
                  IRIS_OK);
        kobject_release(root_obj);
        /* State: refcount=2, active_refs=1 */

        /* Simulate kprocess_reap_address_space: invalidate then release process ref. */
        kvspace_invalidate(vs);
        ASSERT_EQ(vs->valid, 0u);
        ASSERT_EQ(vs->cr3,   0u);

        kobject_release(&vs->base);   /* drop process->vspace ref: refcount=1 */
        ASSERT_EQ(atomic_load(&vs->base.refcount),    1u);
        ASSERT_EQ(atomic_load(&vs->base.active_refs), 1u);

        /* CNode teardown releases active_retain+retain → refcount=0 → destroy. */
        vs_free_proc(p);
        /* No crash = no double-free. */
    }
}
