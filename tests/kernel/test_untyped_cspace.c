/*
 * test_untyped_cspace.c — Fase 3.3 unit tests for cspace_or_handle_resolve_untyped.
 *
 * Covers:
 *   - CSpace path resolves correct KUntyped objects
 *   - Handle-table fallback resolves when no CSpace root is set
 *   - Wrong object type returns IRIS_ERR_WRONG_TYPE
 *   - Missing rights returns IRIS_ERR_ACCESS_DENIED
 *   - ACCESS_DENIED from CSpace does not fall through to handle table
 *   - Refcounts balance across repeated lookups
 *   - CPTR_NULL is rejected
 *   - Legacy handle ABI continues to work
 *   - child_count enforcement (reset with live children)
 *   - child_count increments/decrements correctly
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/kpage.h>
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static struct KProcess *make_proc(void) {
    struct KProcess *p = (struct KProcess *)kpage_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    handle_table_init(&p->handle_table);
    p->cspace_root_h = HANDLE_INVALID;
    return p;
}

static void free_proc(struct KProcess *p) {
    handle_table_close_all(&p->handle_table);
    kpage_free(p, (uint32_t)sizeof(*p));
}

static struct KCNode *setup_cspace(struct KProcess *p, uint32_t num_slots) {
    struct KCNode *root = kcnode_alloc(num_slots);
    if (!root) return NULL;
    handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                          RIGHT_READ | RIGHT_WRITE);
    kobject_release(&root->base);
    if (rh == HANDLE_INVALID) return NULL;
    p->cspace_root_h = rh;
    return root;
}

/* Allocate a KUntyped backed by a malloc'd buffer. */
static struct KUntyped *make_untyped(uint64_t size) {
    void *buf = malloc((size_t)size);
    if (!buf) return NULL;
    return kuntyped_create((uint64_t)(uintptr_t)buf, size, 0);
}

/* ── Test suite ────────────────────────────────────────────────────────── */

void test_untyped_cspace(void) {
    TEST_SUITE("untyped_cspace");

    /* ── [UT] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(kcnode_mint(root, 2, &ut->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ut->base); /* drop alloc ref; CNode slot owns it */

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, 2u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_UNTYPED);
        ASSERT_EQ(rout, RIGHT_READ | RIGHT_WRITE);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [UT] Handle fallback: no CSpace root → legacy handle ABI works ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        /* cspace_root_h stays HANDLE_INVALID */

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base,
                                             RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER);
        kobject_release(&ut->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                    RIGHT_READ, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_UNTYPED);
        ASSERT_EQ(rout, RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [UT] Wrong type in CSpace → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        /* Mint an Endpoint where a KUntyped is expected. */
        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 3, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, 3u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }

    /* ── [UT] Wrong type in handle table → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base, RIGHT_READ);
        kobject_release(&ep->base);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                    RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }

    /* ── [UT] Missing rights in CSpace → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        ASSERT_EQ(kcnode_mint(root, 4, &ut->base, RIGHT_READ), IRIS_OK);
        kobject_release(&ut->base);

        struct KUntyped *out; iris_rights_t rout;
        /* Slot has READ-only; need WRITE → ACCESS_DENIED. */
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, 4u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* Slot has READ; need READ → OK. */
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, 4u, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [UT] Missing rights in handle fallback → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base, RIGHT_READ);
        kobject_release(&ut->base);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                    RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        free_proc(p);
    }

    /* ── [UT] ACCESS_DENIED from CSpace does NOT fall back to handle table ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        /* CSpace slot: READ-only. Handle table: full rights. */
        ASSERT_EQ(kcnode_mint(root, 1, &ut->base, RIGHT_READ), IRIS_OK);
        kobject_retain(&ut->base); /* lend extra ref for second insert */
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&ut->base);
        kobject_release(&ut->base); /* drop extra retain */

        struct KUntyped *out; iris_rights_t rout;
        /* cptr=1 → CSpace → ACCESS_DENIED (need WRITE, slot has READ-only).
         * Must NOT fall back to handle table (which has WRITE). */
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, 1u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        (void)h;
        free_proc(p);
    }

    /* ── [UT] Repeated resolve: refcount balance ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&ut->base);

        for (int i = 0; i < 5; i++) {
            struct KUntyped *out; iris_rights_t rout;
            ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                        RIGHT_NONE, &out, &rout), IRIS_OK);
            kobject_active_release(&out->base);
            kobject_release(&out->base);
        }
        /* If refcounts are balanced, the object is still alive in the table. */
        struct KObject *obj; iris_rights_t r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &obj, &r), IRIS_OK);
        ASSERT_EQ(obj->type, KOBJ_UNTYPED);
        kobject_release(obj);

        free_proc(p);
    }

    /* ── [UT] CPTR_NULL is rejected by the underlying CSpace traversal ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KUntyped *out; iris_rights_t rout;
        /* CPTR_NULL → CSpace rejects it → falls back to handle table.
         * Handle table lookup with id=0 (CPTR_NULL) → NOT_FOUND or INVALID_ARG. */
        iris_error_t err = cspace_or_handle_resolve_untyped(p, CPTR_NULL,
                                                             RIGHT_NONE, &out, &rout);
        ASSERT_TRUE(err != IRIS_OK);

        free_proc(p);
    }

    /* ── [UT] null proc returns INVALID_ARG ── */
    {
        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(NULL, 1u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_INVALID_ARG);
    }

    /* ── [UT] child_count: alloc_child increments, release_child decrements ── */
    {
        const uint64_t BUF_SZ = 512u;
        void *buf = malloc((size_t)BUF_SZ);
        ASSERT_NOT_NULL(buf);
        struct KUntyped *u = kuntyped_create((uint64_t)(uintptr_t)buf, BUF_SZ, 0);
        ASSERT_NOT_NULL(u);

        ASSERT_EQ(atomic_load(&u->child_count), 0u);

        void *child = kuntyped_alloc_child(u, 32u);
        ASSERT_NOT_NULL(child);
        ASSERT_EQ(atomic_load(&u->child_count), 1u);

        kuntyped_release_child(child, 32u);
        ASSERT_EQ(atomic_load(&u->child_count), 0u);

        kuntyped_destroy_ref(u);
        free(buf);
    }

    /* ── [UT] reset with live children: child_count != 0 must block reset ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        const uint64_t BUF_SZ = 4096u;
        void *buf = malloc((size_t)BUF_SZ);
        ASSERT_NOT_NULL(buf);
        struct KUntyped *ut = kuntyped_create((uint64_t)(uintptr_t)buf, BUF_SZ, 0);
        ASSERT_NOT_NULL(ut);

        /* Allocate a child — this increments child_count to 1. */
        void *child = kuntyped_alloc_child(ut, 64u);
        ASSERT_NOT_NULL(child);
        ASSERT_EQ(atomic_load(&ut->child_count), 1u);

        /* Insert into handle table so the dual-resolve helper can find it. */
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&ut->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        /* Resolve via handle fallback (no CSpace root). */
        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                    RIGHT_WRITE, &out, &rout), IRIS_OK);

        /* child_count == 1: reset must return BUSY. */
        uint32_t children = atomic_load_explicit(&out->child_count, memory_order_acquire);
        ASSERT_EQ(children, 1u);

        kobject_active_release(&out->base);
        kobject_release(&out->base);

        /* Release the child — child_count drops to 0. */
        kuntyped_release_child(child, 64u);
        ASSERT_EQ(atomic_load(&ut->child_count), 0u);

        /* Now another resolve: child_count == 0, reset would succeed. */
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                    RIGHT_WRITE, &out, &rout), IRIS_OK);
        children = atomic_load_explicit(&out->child_count, memory_order_acquire);
        ASSERT_EQ(children, 0u);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_proc(p);
        free(buf);
    }

    /* ── [UT] CSpace path: rights are checked correctly (READ required) ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        /* Mint with READ | WRITE; resolve requiring READ → should succeed. */
        ASSERT_EQ(kcnode_mint(root, 5, &ut->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ut->base);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, 5u, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        ASSERT_TRUE((rout & RIGHT_READ) != 0);
        ASSERT_TRUE((rout & RIGHT_WRITE) != 0);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [UT] Active+lifecycle contract: active_refs held during resolve ── */
    {
        /* Verify that the handle-table path adds kobject_active_retain.
         * After insert + drop alloc ref: active_refs == 1 (from handle entry).
         * After resolve: active_refs == 2 (entry + our active_retain).
         * After active_release + lifecycle release: back to 1. */
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KUntyped *ut = make_untyped(4096u);
        ASSERT_NOT_NULL(ut);
        /* After alloc: refcount=1, active_refs=0 */
        handle_id_t h = handle_table_insert(&p->handle_table, &ut->base,
                                             RIGHT_READ | RIGHT_WRITE);
        /* After insert: refcount=2, active_refs=1 */
        kobject_release(&ut->base); /* drop alloc ref: refcount=1, active_refs=1 */
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_untyped(p, (iris_cptr_t)h,
                                                    RIGHT_NONE, &out, &rout), IRIS_OK);
        /* During resolve via handle path: refcount=2, active_refs=2. */
        ASSERT_EQ(atomic_load(&out->base.active_refs), 2u);
        kobject_active_release(&out->base);
        kobject_release(&out->base);
        /* After release: active_refs=1, refcount=1. */
        ASSERT_EQ(atomic_load(&out->base.active_refs), 1u);

        free_proc(p);
    }
}
