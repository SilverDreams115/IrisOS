#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/kpage.h>
#include <string.h>

/* Minimal KProcess for testing: only cspace_root matters. */
static struct KProcess *make_test_proc(void) {
    struct KProcess *p = (struct KProcess *)kpage_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->cspace_root = NULL;
    return p;
}

static void free_test_proc(struct KProcess *p) {
    /* Stage 4: structural root — released here instead of by
     * handle_table_close_all, which no longer owns it. */
    if (p->cspace_root) {
        kobject_active_release(&p->cspace_root->base);
        kobject_release(&p->cspace_root->base);
        p->cspace_root = NULL;
    }
    kpage_free(p, (uint32_t)sizeof(*p));
}

static int g_destroyed = 0;
static void test_destroy(struct KObject *obj) { (void)obj; g_destroyed++; }
static const struct KObjectOps test_ops = { .close = NULL, .destroy = test_destroy };

static struct KObject *make_obj(kobject_type_t type) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (!o) return NULL;
    kobject_init(o, type, &test_ops);
    return o;
}

void test_cspace(void) {
    TEST_SUITE("cspace");

    /* ── CPTR_NULL always rejected ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);
        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, CPTR_NULL, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_INVALID_ARG);
        free_test_proc(p);
    }

    /* ── No CSpace root (HANDLE_INVALID) → NOT_FOUND ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);
        /* cspace_root stays NULL */
        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 1u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_NOT_FOUND);
        free_test_proc(p);
    }

    /* ── Single-level: empty slot → NOT_FOUND ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        /* slot 3 is empty */
        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 3u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_NOT_FOUND);
        free_test_proc(p);
    }

    /* ── Single-level: populated slot → correct object and rights ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        g_destroyed = 0;
        struct KObject *target = make_obj(KOBJ_CHANNEL);
        ASSERT_NOT_NULL(target);
        iris_rights_t stored_rights = RIGHT_READ | RIGHT_WRITE;
        ASSERT_EQ(kcnode_mint(root, 5, target, stored_rights), IRIS_OK);
        kobject_release(target);  /* drop our alloc ref */

        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 5u, RIGHT_NONE, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->type, KOBJ_CHANNEL);
        ASSERT_EQ(rout, stored_rights);

        /* Root CNode still intact — resolve again should succeed */
        struct KObject *out2; iris_rights_t rout2;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 5u, RIGHT_NONE, &out2, &rout2), IRIS_OK);
        ASSERT_EQ(out2->type, KOBJ_CHANNEL);

        kobject_active_release(out2);
        kobject_release(out2);
        kobject_active_release(out);
        kobject_release(out);

        /* target destroyed only after both the cnode slot and our resolve refs drop */
        ASSERT_EQ(g_destroyed, 0); /* still alive in cnode slot */
        free_test_proc(p);
        ASSERT_EQ(g_destroyed, 1); /* freed when table closes root cnode */
    }

    /* ── Rights check: required rights not met → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        struct KObject *target = make_obj(KOBJ_NOTIFICATION);
        ASSERT_NOT_NULL(target);
        ASSERT_EQ(kcnode_mint(root, 2, target, RIGHT_READ), IRIS_OK);
        kobject_release(target);

        struct KObject *out; iris_rights_t rout;
        /* slot has only READ; require WRITE → ACCESS_DENIED */
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 2u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* slot has READ; require READ → OK */
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 2u, RIGHT_READ, &out, &rout), IRIS_OK);
        ASSERT_EQ(rout, RIGHT_READ);
        kobject_active_release(out);
        kobject_release(out);

        free_test_proc(p);
    }

    /* ── Two-level traversal ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        /* root: 8 slots (3 bits), child: 8 slots (3 bits).
         * To reach child slot 6 via root slot 4: cptr = (6 << 3) | 4 = 52. */
        struct KCNode *root  = kcnode_alloc(8);
        struct KCNode *child = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        ASSERT_NOT_NULL(child);

        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        /* Place child CNode into root slot 4. */
        ASSERT_EQ(kcnode_mint(root, 4, &child->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&child->base);

        g_destroyed = 0;
        struct KObject *leaf = make_obj(KOBJ_ENDPOINT);
        ASSERT_NOT_NULL(leaf);
        ASSERT_EQ(kcnode_mint(child, 6, leaf, RIGHT_READ), IRIS_OK);
        kobject_release(leaf);

        iris_cptr_t cptr = (iris_cptr_t)((6u << 3) | 4u);  /* = 52 */
        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, cptr, RIGHT_NONE, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->type, KOBJ_ENDPOINT);
        ASSERT_EQ(rout, RIGHT_READ);
        kobject_active_release(out);
        kobject_release(out);

        ASSERT_EQ(g_destroyed, 0);
        free_test_proc(p);
        ASSERT_EQ(g_destroyed, 1);
    }

    /* ── Stage 4: a CPtr addresses exactly one capability ──
     * Resolution consumes radix bits per level and is terminal when the CPtr
     * is EXHAUSTED.  It used to also be terminal at the first non-CNode slot,
     * which discarded the remaining bits: with an 8-slot root, CPtr 3, 11, 19,
     * … all resolved to slot 3.  Leftover bits with nothing to descend into
     * are a malformed CPtr, not an alias. */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);   /* 3 radix bits */
        ASSERT_NOT_NULL(root);
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        struct KObject *leaf = make_obj(KOBJ_ENDPOINT);
        ASSERT_NOT_NULL(leaf);
        ASSERT_EQ(kcnode_mint(root, 3, leaf, RIGHT_READ), IRIS_OK);
        kobject_release(leaf);

        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 3u, RIGHT_NONE, &out, &rout), IRIS_OK);
        kobject_active_release(out);
        kobject_release(out);

        /* Every alias of slot 3 must be rejected, on every resolver. */
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 3u + 8u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(cspace_resolve_cap(p->cspace_root, 3u + 64u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_INVALID_ARG);

        struct KCNode *cn_out; uint32_t idx_out;
        ASSERT_EQ(cspace_resolve_slot(p->cspace_root, 3u, &cn_out, &idx_out), IRIS_OK);
        ASSERT_EQ(idx_out, 3u);
        kobject_active_release(&cn_out->base);
        kobject_release(&cn_out->base);
        ASSERT_EQ(cspace_resolve_slot(p->cspace_root, 3u + 8u, &cn_out, &idx_out),
                  IRIS_ERR_INVALID_ARG);

        /* An EMPTY root slot is still a valid destination at its own depth,
         * and still not a path to descend through. */
        ASSERT_EQ(cspace_resolve_dest_slot(p->cspace_root, 5u, &cn_out, &idx_out), IRIS_OK);
        ASSERT_EQ(idx_out, 5u);
        kobject_active_release(&cn_out->base);
        kobject_release(&cn_out->base);
        ASSERT_EQ(cspace_resolve_dest_slot(p->cspace_root, 3u + 8u, &cn_out, &idx_out),
                  IRIS_ERR_INVALID_ARG);
        ASSERT_EQ(cspace_resolve_dest_slot(p->cspace_root, 5u + 8u, &cn_out, &idx_out),
                  IRIS_ERR_NOT_FOUND);

        free_test_proc(p);
    }

    /* ── Typed resolve: cspace_resolve_endpoint OK ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        struct KEndpoint *ep = TEST_UT_ALLOC(struct KEndpoint, kendpoint_alloc_at);
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 3, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_endpoint(p->cspace_root, 3u, RIGHT_NONE, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_ENDPOINT);
        ASSERT_EQ(rout, RIGHT_READ | RIGHT_WRITE);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_test_proc(p);
    }

    /* ── Typed resolve: wrong type returns WRONG_TYPE ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        /* Mint a CHANNEL into slot 1; try to resolve it as an Endpoint. */
        struct KObject *ch = make_obj(KOBJ_CHANNEL);
        ASSERT_NOT_NULL(ch);
        ASSERT_EQ(kcnode_mint(root, 1, ch, RIGHT_READ), IRIS_OK);
        kobject_release(ch);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_endpoint(p->cspace_root, 1u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_test_proc(p);
    }

    /* ── cspace_resolve_only_cnode: CSpace path (root set, slot has CNode) ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root  = kcnode_alloc(8);
        struct KCNode *inner = kcnode_alloc(16);
        ASSERT_NOT_NULL(root); ASSERT_NOT_NULL(inner);

        /* Stage 4: structural CSpace root — kcnode_alloc's ref is the
         * lifecycle ref, plus the active ref the handle used to own. */
        kobject_active_retain(&root->base);
        p->cspace_root = root;

        /* Place inner CNode into root slot 2. */
        ASSERT_EQ(kcnode_mint(root, 2, &inner->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&inner->base);

        struct KCNode  *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_only_cnode(p->cspace_root, 2u, RIGHT_WRITE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->slot_count, 16u);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_test_proc(p);
    }



}
