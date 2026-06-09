#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/kpage.h>
#include <string.h>

/* Minimal KProcess for testing: only handle_table and cspace_root_h matter. */
static struct KProcess *make_test_proc(void) {
    struct KProcess *p = (struct KProcess *)kpage_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    handle_table_init(&p->handle_table);
    p->cspace_root_h = HANDLE_INVALID;
    return p;
}

static void free_test_proc(struct KProcess *p) {
    handle_table_close_all(&p->handle_table);
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
        ASSERT_EQ(cspace_resolve_cap(p, CPTR_NULL, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_INVALID_ARG);
        free_test_proc(p);
    }

    /* ── No CSpace root (HANDLE_INVALID) → NOT_FOUND ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);
        /* cspace_root_h stays HANDLE_INVALID */
        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p, 1u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_NOT_FOUND);
        free_test_proc(p);
    }

    /* ── Single-level: empty slot → NOT_FOUND ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        ASSERT_NE(rh, (handle_id_t)HANDLE_INVALID);
        p->cspace_root_h = rh;

        /* slot 3 is empty */
        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p, 3u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_NOT_FOUND);
        free_test_proc(p);
    }

    /* ── Single-level: populated slot → correct object and rights ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        p->cspace_root_h = rh;

        g_destroyed = 0;
        struct KObject *target = make_obj(KOBJ_CHANNEL);
        ASSERT_NOT_NULL(target);
        iris_rights_t stored_rights = RIGHT_READ | RIGHT_WRITE;
        ASSERT_EQ(kcnode_mint(root, 5, target, stored_rights), IRIS_OK);
        kobject_release(target);  /* drop our alloc ref */

        struct KObject *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_cap(p, 5u, RIGHT_NONE, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->type, KOBJ_CHANNEL);
        ASSERT_EQ(rout, stored_rights);

        /* Root CNode still intact — resolve again should succeed */
        struct KObject *out2; iris_rights_t rout2;
        ASSERT_EQ(cspace_resolve_cap(p, 5u, RIGHT_NONE, &out2, &rout2), IRIS_OK);
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
        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        p->cspace_root_h = rh;

        struct KObject *target = make_obj(KOBJ_NOTIFICATION);
        ASSERT_NOT_NULL(target);
        ASSERT_EQ(kcnode_mint(root, 2, target, RIGHT_READ), IRIS_OK);
        kobject_release(target);

        struct KObject *out; iris_rights_t rout;
        /* slot has only READ; require WRITE → ACCESS_DENIED */
        ASSERT_EQ(cspace_resolve_cap(p, 2u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* slot has READ; require READ → OK */
        ASSERT_EQ(cspace_resolve_cap(p, 2u, RIGHT_READ, &out, &rout), IRIS_OK);
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

        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        p->cspace_root_h = rh;

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
        ASSERT_EQ(cspace_resolve_cap(p, cptr, RIGHT_NONE, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->type, KOBJ_ENDPOINT);
        ASSERT_EQ(rout, RIGHT_READ);
        kobject_active_release(out);
        kobject_release(out);

        ASSERT_EQ(g_destroyed, 0);
        free_test_proc(p);
        ASSERT_EQ(g_destroyed, 1);
    }

    /* ── Typed resolve: cspace_resolve_endpoint OK ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root = kcnode_alloc(8);
        ASSERT_NOT_NULL(root);
        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        p->cspace_root_h = rh;

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 3, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_endpoint(p, 3u, RIGHT_NONE, &out, &rout), IRIS_OK);
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
        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        p->cspace_root_h = rh;

        /* Mint a CHANNEL into slot 1; try to resolve it as an Endpoint. */
        struct KObject *ch = make_obj(KOBJ_CHANNEL);
        ASSERT_NOT_NULL(ch);
        ASSERT_EQ(kcnode_mint(root, 1, ch, RIGHT_READ), IRIS_OK);
        kobject_release(ch);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_resolve_endpoint(p, 1u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_test_proc(p);
    }

    /* ── cspace_or_handle_resolve_cnode: CSpace path (root set, slot has CNode) ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *root  = kcnode_alloc(8);
        struct KCNode *inner = kcnode_alloc(16);
        ASSERT_NOT_NULL(root); ASSERT_NOT_NULL(inner);

        handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                              RIGHT_READ | RIGHT_WRITE);
        kobject_release(&root->base);
        p->cspace_root_h = rh;

        /* Place inner CNode into root slot 2. */
        ASSERT_EQ(kcnode_mint(root, 2, &inner->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&inner->base);

        struct KCNode  *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_cnode(p, 2u, RIGHT_WRITE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->slot_count, 16u);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_test_proc(p);
    }

    /* ── cspace_or_handle_resolve_cnode: handle fallback (no CSpace root) ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);
        /* cspace_root_h stays HANDLE_INVALID → handle fallback always taken. */

        struct KCNode *cn = kcnode_alloc(8);
        ASSERT_NOT_NULL(cn);
        handle_id_t h = handle_table_insert(&p->handle_table, &cn->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&cn->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KCNode  *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_cnode(p, (iris_cptr_t)h, RIGHT_WRITE,
                                                  &out, &rout), IRIS_OK);
        ASSERT_EQ(out->slot_count, 8u);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_test_proc(p);
    }

    /* ── cspace_or_handle_resolve_cnode: handle fallback rights check ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *cn = kcnode_alloc(8);
        ASSERT_NOT_NULL(cn);
        handle_id_t h = handle_table_insert(&p->handle_table, &cn->base, RIGHT_READ);
        kobject_release(&cn->base);

        struct KCNode  *out; iris_rights_t rout;
        /* Have only READ; require WRITE → ACCESS_DENIED. */
        ASSERT_EQ(cspace_or_handle_resolve_cnode(p, (iris_cptr_t)h, RIGHT_WRITE,
                                                  &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* Have READ; require READ → OK. */
        ASSERT_EQ(cspace_or_handle_resolve_cnode(p, (iris_cptr_t)h, RIGHT_READ,
                                                  &out, &rout), IRIS_OK);
        kobject_active_release(&out->base);
        kobject_release(&out->base);

        free_test_proc(p);
    }

    /* ── Repeated dual-resolve: refcount balance across multiple calls ── */
    {
        struct KProcess *p = make_test_proc();
        ASSERT_NOT_NULL(p);

        struct KCNode *cn = kcnode_alloc(8);
        ASSERT_NOT_NULL(cn);
        handle_id_t h = handle_table_insert(&p->handle_table, &cn->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&cn->base);

        for (int i = 0; i < 4; i++) {
            struct KCNode  *out; iris_rights_t rout;
            ASSERT_EQ(cspace_or_handle_resolve_cnode(p, (iris_cptr_t)h,
                                                      RIGHT_NONE, &out, &rout), IRIS_OK);
            kobject_active_release(&out->base);
            kobject_release(&out->base);
        }
        /* If refcounts are balanced, the CNode is still alive in the table. */
        struct KObject *obj; iris_rights_t r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &obj, &r), IRIS_OK);
        ASSERT_EQ(obj->type, KOBJ_CNODE);
        kobject_release(obj);

        free_test_proc(p);
    }
}
