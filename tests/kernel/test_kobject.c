#include "framework.h"
#include <iris/nc/kobject.h>

/* ── Minimal concrete object for testing ──────────────────────────────────── */

static int g_close_fired  = 0;
static int g_destroy_fired = 0;

static void test_close(struct KObject *obj)   { (void)obj; g_close_fired++; }
static void test_destroy(struct KObject *obj) { (void)obj; g_destroy_fired++; }

/* Stack-allocated KObject — the destroy callback must not call kpage_free on it. */
static void test_obj_destroy_nofree(struct KObject *obj) { (void)obj; g_destroy_fired++; }
static const struct KObjectOps nofree_ops = { .close = test_close, .destroy = test_obj_destroy_nofree };

void test_kobject(void) {
    TEST_SUITE("kobject");

    /* ── init ── */
    g_close_fired = g_destroy_fired = 0;
    struct KObject obj;
    kobject_init(&obj, KOBJ_CHANNEL, &nofree_ops);
    ASSERT_EQ(obj.type, KOBJ_CHANNEL);
    ASSERT_EQ(atomic_load(&obj.refcount), 1u);
    ASSERT_EQ(atomic_load(&obj.active_refs), 0u);

    /* ── retain increases refcount ── */
    kobject_retain(&obj);
    ASSERT_EQ(atomic_load(&obj.refcount), 2u);

    /* ── release decrements, no destroy yet ── */
    kobject_release(&obj);
    ASSERT_EQ(atomic_load(&obj.refcount), 1u);
    ASSERT_EQ(g_destroy_fired, 0);

    /* ── last release fires destroy ── */
    kobject_release(&obj);
    ASSERT_EQ(g_destroy_fired, 1);

    /* ── active_retain / active_release fire close exactly once ── */
    g_close_fired = g_destroy_fired = 0;
    struct KObject obj2;
    kobject_init(&obj2, KOBJ_NOTIFICATION, &nofree_ops);
    kobject_active_retain(&obj2);
    kobject_active_retain(&obj2);
    ASSERT_EQ(atomic_load(&obj2.active_refs), 2u);
    ASSERT_EQ(g_close_fired, 0);

    kobject_active_release(&obj2);
    ASSERT_EQ(g_close_fired, 0);          /* 2→1, not yet */
    kobject_active_release(&obj2);
    ASSERT_EQ(g_close_fired, 1);          /* 1→0: close fires */
    ASSERT_EQ(atomic_load(&obj2.active_refs), 0u);

    /* release the lifecycle ref — destroy fires */
    kobject_release(&obj2);
    ASSERT_EQ(g_destroy_fired, 1);

    /* ── retain/release symmetry over multiple refs ── */
    g_close_fired = g_destroy_fired = 0;
    struct KObject obj3;
    kobject_init(&obj3, KOBJ_VMO, &nofree_ops);
    for (int i = 0; i < 10; i++) kobject_retain(&obj3);
    ASSERT_EQ(atomic_load(&obj3.refcount), 11u);
    for (int i = 0; i < 10; i++) kobject_release(&obj3);
    ASSERT_EQ(atomic_load(&obj3.refcount), 1u);
    ASSERT_EQ(g_destroy_fired, 0);
    kobject_release(&obj3);
    ASSERT_EQ(g_destroy_fired, 1);

    /* ── type field preserved through init ── */
    struct KObject obj4;
    kobject_init(&obj4, KOBJ_PROCESS, &nofree_ops);
    ASSERT_EQ(obj4.type, KOBJ_PROCESS);
    kobject_release(&obj4);

    /* ── ops pointer preserved ── */
    struct KObject obj5;
    kobject_init(&obj5, KOBJ_ENDPOINT, &nofree_ops);
    ASSERT_EQ(obj5.ops, &nofree_ops);
    kobject_release(&obj5);
}
