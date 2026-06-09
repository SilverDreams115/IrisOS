#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/rights.h>
#include <iris/kpage.h>

/* Destroy callback that increments a counter — no kpage_free needed since
 * the CNode itself is freed by kcnode_obj_destroy which calls kpage_free. */
static int g_obj_destroyed = 0;
static void tracked_destroy(struct KObject *obj) {
    (void)obj; g_obj_destroyed++;
}
static const struct KObjectOps tracked_ops = { .close = NULL, .destroy = tracked_destroy };

/* Create a heap-backed test object (kpage_alloc → malloc in stubs). */
static struct KObject *make_obj(kobject_type_t type) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (!o) return NULL;
    kobject_init(o, type, &tracked_ops);
    return o;
}

void test_kcnode(void) {
    TEST_SUITE("kcnode");

    /* ── alloc / close ── */
    struct KCNode *cn = kcnode_alloc(8);
    ASSERT_NOT_NULL(cn);
    ASSERT_EQ(cn->slot_count, 8u);
    ASSERT_EQ(atomic_load(&cn->base.refcount), 1u);

    /* ── empty slot returns NOT_FOUND ── */
    struct KObject *out; iris_rights_t rout;
    ASSERT_EQ(kcnode_fetch(cn, 0, &out, &rout), IRIS_ERR_NOT_FOUND);

    /* ── mint puts an object in a slot ── */
    g_obj_destroyed = 0;
    struct KObject *obj = make_obj(KOBJ_CHANNEL);
    ASSERT_NOT_NULL(obj);
    iris_rights_t rights = RIGHT_READ | RIGHT_WRITE;
    ASSERT_EQ(kcnode_mint(cn, 0, obj, rights), IRIS_OK);
    /* kcnode_mint retains the object (active+lifecycle) */
    ASSERT_EQ(atomic_load(&obj->refcount), 2u);  /* alloc ref + mint ref */
    kobject_release(obj);  /* drop our alloc ref; cnode holds its own */

    /* ── fetch returns the same object and rights ── */
    ASSERT_EQ(kcnode_fetch(cn, 0, &out, &rout), IRIS_OK);
    ASSERT_EQ(out->type, KOBJ_CHANNEL);
    ASSERT_EQ(rout, rights);
    kobject_active_release(out); /* drop active ref from fetch */
    kobject_release(out);        /* drop lifecycle ref from fetch */

    /* ── out-of-bounds slot ── */
    ASSERT_EQ(kcnode_fetch(cn, 8, &out, &rout), IRIS_ERR_INVALID_ARG);
    ASSERT_EQ(kcnode_mint(cn, 8, make_obj(KOBJ_CHANNEL), rights), IRIS_ERR_INVALID_ARG);

    /* ── delete clears the slot ── */
    ASSERT_EQ(kcnode_delete(cn, 0), IRIS_OK);
    ASSERT_EQ(g_obj_destroyed, 1);  /* released by delete */
    ASSERT_EQ(kcnode_fetch(cn, 0, &out, &rout), IRIS_ERR_NOT_FOUND);

    /* ── swap exchanges two slots ── */
    struct KObject *a = make_obj(KOBJ_VMO);
    struct KObject *b = make_obj(KOBJ_NOTIFICATION);
    ASSERT_EQ(kcnode_mint(cn, 1, a, RIGHT_READ), IRIS_OK);
    ASSERT_EQ(kcnode_mint(cn, 2, b, RIGHT_WRITE), IRIS_OK);
    kobject_release(a);
    kobject_release(b);

    ASSERT_EQ(kcnode_swap(cn, 1, 2), IRIS_OK);

    iris_rights_t r1, r2;
    struct KObject *o1, *o2;
    ASSERT_EQ(kcnode_fetch(cn, 1, &o1, &r1), IRIS_OK);
    ASSERT_EQ(kcnode_fetch(cn, 2, &o2, &r2), IRIS_OK);
    ASSERT_EQ(o1->type, KOBJ_NOTIFICATION);  /* was b */
    ASSERT_EQ(r1, RIGHT_WRITE);
    ASSERT_EQ(o2->type, KOBJ_VMO);           /* was a */
    ASSERT_EQ(r2, RIGHT_READ);
    kobject_active_release(o1); kobject_release(o1);
    kobject_active_release(o2); kobject_release(o2);

    /* ── same slot swap is invalid ── */
    ASSERT_EQ(kcnode_swap(cn, 1, 1), IRIS_ERR_INVALID_ARG);

    /* ── invalid mint: zero rights rejected ── */
    struct KObject *c = make_obj(KOBJ_CHANNEL);
    ASSERT_EQ(kcnode_mint(cn, 3, c, RIGHT_NONE), IRIS_ERR_INVALID_ARG);
    kobject_release(c);

    /* ── close releases all slots ── */
    g_obj_destroyed = 0;
    kcnode_close(cn); /* refcount 1→0 → destroy → closes all slots */
    ASSERT_EQ(g_obj_destroyed, 2); /* slots 1 and 2 held objects */
}
