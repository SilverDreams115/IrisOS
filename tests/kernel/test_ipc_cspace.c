/*
 * test_ipc_cspace.c — Fase 3.2 unit tests for IPC dual-resolve helpers.
 *
 * Covers cspace_resolve_only_endpoint/reply/notification, verifying:
 *   - CSpace path resolves correct typed objects
 *   - Handle-table fallback resolves when no CSpace root is set
 *   - Wrong object type returns IRIS_ERR_WRONG_TYPE
 *   - Missing rights returns IRIS_ERR_ACCESS_DENIED
 *   - ACCESS_DENIED from CSpace does not fall through to handle table
 *   - Refcounts balance across repeated lookups
 *   - Legacy handle ABI continues to work
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kreply.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/kpage.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

/* Install a CSpace root with num_slots slots and return the root CNode. */
static struct KCNode *setup_cspace(struct KProcess *p, uint32_t num_slots) {
    struct KCNode *root = kcnode_alloc(num_slots);
    if (!root) return NULL;
    /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
     * lifecycle ref, plus the active ref the handle used to own. */
    kobject_active_retain(&root->base);
    p->cspace_root = root;
    return root;
}

/* ── Endpoint helpers ─────────────────────────────────────────────────── */


/* Fase S1: kreply_alloc(caller) is retired — reproduce the old semantics for
 * these tests: placement-init in an untyped-child block, then stage+bind the
 * caller (the production rendezvous sequence). */
static struct KReply *test_kreply_alloc(struct task *caller) {
    struct KReply *r = TEST_UT_ALLOC(struct KReply, kreply_alloc_at);
    if (r && caller) {
        (void)kreply_stage(r);
        (void)kreply_bind_caller(r, caller);
    }
    return r;
}

void test_ipc_cspace(void) {
    TEST_SUITE("ipc_cspace");

    /* ── [EP] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 3, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_endpoint(p, 3u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_ENDPOINT);
        ASSERT_EQ(rout, RIGHT_READ | RIGHT_WRITE);
        kobject_release(&out->base);

        free_proc(p);
    }


    /* ── [EP] Wrong object type in CSpace → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        /* Mint a KNotification where an endpoint is expected. */
        struct KNotification *n = TEST_UT_ALLOC(struct KNotification, knotification_alloc_at);
        ASSERT_NOT_NULL(n);
        ASSERT_EQ(kcnode_mint(root, 2, &n->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&n->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_endpoint(p, 2u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }


    /* ── [EP] Missing rights in CSpace → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 4, &ep->base, RIGHT_READ), IRIS_OK);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_endpoint(p, 4u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* Correct right succeeds. */
        ASSERT_EQ(cspace_resolve_only_endpoint(p, 4u, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        kobject_release(&out->base);

        free_proc(p);
    }




    /* ── [Reply] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KReply *rp = test_kreply_alloc(NULL);
        ASSERT_NOT_NULL(rp);
        ASSERT_EQ(kcnode_mint(root, 5, &rp->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&rp->base);

        struct KReply *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_reply(p, 5u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_REPLY);
        kobject_release(&out->base);

        free_proc(p);
    }


    /* ── [Reply] Wrong type → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 2, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KReply *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_reply(p, 2u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }


    /* ── [Notification] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KNotification *n = TEST_UT_ALLOC(struct KNotification, knotification_alloc_at);
        ASSERT_NOT_NULL(n);
        ASSERT_EQ(kcnode_mint(root, 6, &n->base,
                               RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT), IRIS_OK);
        kobject_release(&n->base);

        struct KNotification *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_notification(p, 6u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_NOTIFICATION);
        kobject_release(&out->base);

        free_proc(p);
    }







    /* ── [Fase 9] badges: per-cap identity ───────────────────────────── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);

        /* Two caps to the SAME endpoint with DIFFERENT badges. */
        ASSERT_EQ(kcnode_mint_excl_badged(root, 1, &ep->base, RIGHT_WRITE,
                                          0xAAu), IRIS_OK);
        ASSERT_EQ(kcnode_mint_excl_badged(root, 2, &ep->base, RIGHT_WRITE,
                                          0xBBu), IRIS_OK);
        /* Unbadged third cap. */
        ASSERT_EQ(kcnode_mint_excl(root, 3, &ep->base, RIGHT_WRITE), IRIS_OK);

        struct KEndpoint *out; iris_rights_t rout; uint64_t badge;
        badge = 99u;
        ASSERT_EQ(cspace_resolve_only_endpoint_badged(p, 1u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0xAAu);
        kobject_release(&out->base);

        badge = 99u;
        ASSERT_EQ(cspace_resolve_only_endpoint_badged(p, 2u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0xBBu);
        kobject_release(&out->base);

        badge = 99u;
        ASSERT_EQ(cspace_resolve_only_endpoint_badged(p, 3u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0u);                 /* unbadged cap delivers 0 */
        kobject_release(&out->base);

        /* swap preserves badges; delete clears only its own slot. */
        ASSERT_EQ(kcnode_swap(root, 1, 2), IRIS_OK);
        uint64_t b1 = 0, b2 = 0; struct KObject *o; iris_rights_t rr;
        ASSERT_EQ(kcnode_fetch_badged(root, 1, &o, &rr, &b1), IRIS_OK);
        kobject_active_release(o); kobject_release(o);
        ASSERT_EQ(kcnode_fetch_badged(root, 2, &o, &rr, &b2), IRIS_OK);
        kobject_active_release(o); kobject_release(o);
        ASSERT_EQ(b1, 0xBBu);
        ASSERT_EQ(b2, 0xAAu);
        ASSERT_EQ(kcnode_delete(root, 1), IRIS_OK);
        ASSERT_EQ(kcnode_fetch_badged(root, 2, &o, &rr, &b2), IRIS_OK);
        kobject_active_release(o); kobject_release(o);
        ASSERT_EQ(b2, 0xAAu);                 /* unaffected by delete */

        /* Stage 4: a non-CPtr value is malformed, and a failed resolve must
         * not write through the badge out-parameter. */
        badge = 99u;
        ASSERT_EQ(cspace_resolve_only_endpoint_badged(p, (iris_cptr_t)0x80000401u,
                                                           RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(badge, 99u);

        /* ACCESS_DENIED on a badged slot stays a hard stop, no badge leak. */
        ASSERT_EQ(kcnode_mint_excl_badged(root, 4, &ep->base, RIGHT_READ,
                                          0xDDu), IRIS_OK);
        badge = 99u;
        ASSERT_EQ(cspace_resolve_only_endpoint_badged(p, 4u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_ERR_ACCESS_DENIED);
        ASSERT_EQ(badge, 99u);                /* untouched on failure */

        kobject_release(&ep->base);           /* drop alloc ref */
        free_proc(p);
    }

}
