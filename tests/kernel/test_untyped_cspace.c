/*
 * test_untyped_cspace.c — Phase 3.3 unit tests for cspace_resolve_only_untyped.
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
    p->cspace_root = NULL;
    return p;
}

static void free_proc(struct KProcess *p) {
    /* Stage 4: structural root — released here instead of by
     * handle_table_close_all, which no longer owns it. */
    if (p->cspace_root) {
        kobject_active_release(&p->cspace_root->base);
        kobject_release(&p->cspace_root->base);
        p->cspace_root = NULL;
    }
    kpage_free(p, (uint32_t)sizeof(*p));
}

static struct KCNode *setup_cspace(struct KProcess *p, uint32_t num_slots) {
    struct KCNode *root = kcnode_alloc(num_slots);
    if (!root) return NULL;
    /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
     * lifecycle ref, plus the active ref the handle used to own. */
    kobject_active_retain(&root->base);
    p->cspace_root = root;
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
        ASSERT_EQ(cspace_resolve_only_untyped(p->cspace_root, 2u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_UNTYPED);
        ASSERT_EQ(rout, RIGHT_READ | RIGHT_WRITE);
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
        ASSERT_EQ(cspace_resolve_only_untyped(p->cspace_root, 3u, RIGHT_NONE, &out, &rout),
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
        ASSERT_EQ(cspace_resolve_only_untyped(p->cspace_root, 4u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* Slot has READ; need READ → OK. */
        ASSERT_EQ(cspace_resolve_only_untyped(p->cspace_root, 4u, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

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
        iris_error_t err = cspace_resolve_only_untyped(p->cspace_root, CPTR_NULL,
                                                             RIGHT_NONE, &out, &rout);
        ASSERT_TRUE(err != IRIS_OK);

        free_proc(p);
    }

    /* ── [UT] no CSpace returns NOT_FOUND ── */
    {
        /* Stage 7 Step 4: the first argument is the CSpace ROOT, not a
         * process, so NULL now means "this caller has no CSpace" — the same
         * situation a process with an unset root was already in, and it gets
         * the same answer.  It used to be two codes for one state: NOT_FOUND
         * for a process whose root was unset, INVALID_ARG for no process at
         * all, which a caller could not act on differently anyway. */
        struct KUntyped *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_untyped(NULL, 1u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_NOT_FOUND);
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
        ASSERT_EQ(cspace_resolve_only_untyped(p->cspace_root, 5u, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        ASSERT_TRUE((rout & RIGHT_READ) != 0);
        ASSERT_TRUE((rout & RIGHT_WRITE) != 0);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_proc(p);
    }

}
