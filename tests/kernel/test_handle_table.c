#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/handle_table.h>
#include <iris/kpage.h>

static int g_ht_destroyed = 0;
static void ht_destroy(struct KObject *obj) { (void)obj; g_ht_destroyed++; }
static const struct KObjectOps ht_ops = { .close = NULL, .destroy = ht_destroy };

static struct KObject *make_ht_obj(kobject_type_t type) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (!o) return NULL;
    kobject_init(o, type, &ht_ops);
    return o;
}

void test_handle_table(void) {
    TEST_SUITE("handle_table");

    HandleTable ht;
    handle_table_init(&ht);

    /* ── insert + get_object retains ── */
    struct KObject *obj = make_ht_obj(KOBJ_CHANNEL);
    ASSERT_NOT_NULL(obj);
    iris_rights_t rights = RIGHT_READ | RIGHT_WRITE;
    handle_id_t h = handle_table_insert(&ht, obj, rights);
    ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

    /* handle_entry_init: lifecycle retain + active retain */
    ASSERT_EQ(atomic_load(&obj->refcount),   2u);
    ASSERT_EQ(atomic_load(&obj->active_refs), 1u);

    struct KObject *out; iris_rights_t rout;
    ASSERT_EQ(handle_table_get_object(&ht, h, &out, &rout), IRIS_OK);
    ASSERT_EQ(out, obj);
    ASSERT_EQ(rout, rights);
    ASSERT_EQ(atomic_load(&obj->refcount), 3u);  /* get adds lifecycle retain */
    kobject_release(out);  /* drop get ref */

    /* ── close: releases table refs, active_refs hits zero ── */
    g_ht_destroyed = 0;
    ASSERT_EQ(handle_table_close(&ht, h), IRIS_OK);
    ASSERT_EQ(atomic_load(&obj->active_refs), 0u);
    ASSERT_EQ(atomic_load(&obj->refcount),    1u);  /* our alloc ref still held */
    ASSERT_EQ(g_ht_destroyed, 0);

    /* ── stale handle rejected ── */
    ASSERT_EQ(handle_table_get_object(&ht, h, &out, &rout), IRIS_ERR_BAD_HANDLE);

    kobject_release(obj);
    ASSERT_EQ(g_ht_destroyed, 1);

    /* ── insert_derived + revoke_children (transitive BFS) ── */
    g_ht_destroyed = 0;
    struct KObject *root_obj  = make_ht_obj(KOBJ_ENDPOINT);
    struct KObject *child1    = make_ht_obj(KOBJ_CHANNEL);
    struct KObject *child2    = make_ht_obj(KOBJ_NOTIFICATION);
    struct KObject *gc        = make_ht_obj(KOBJ_VMO);
    ASSERT_NOT_NULL(root_obj);
    ASSERT_NOT_NULL(child1);
    ASSERT_NOT_NULL(child2);
    ASSERT_NOT_NULL(gc);

    handle_id_t h_root  = handle_table_insert(&ht, root_obj, RIGHT_READ);
    handle_id_t h_c1    = handle_table_insert_derived(&ht, child1, RIGHT_READ, h_root);
    handle_id_t h_c2    = handle_table_insert_derived(&ht, child2, RIGHT_READ, h_root);
    handle_id_t h_gc    = handle_table_insert_derived(&ht, gc,     RIGHT_READ, h_c1);

    ASSERT_NE(h_root, (handle_id_t)HANDLE_INVALID);
    ASSERT_NE(h_c1,   (handle_id_t)HANDLE_INVALID);
    ASSERT_NE(h_c2,   (handle_id_t)HANDLE_INVALID);
    ASSERT_NE(h_gc,   (handle_id_t)HANDLE_INVALID);

    handle_table_revoke_children(&ht, h_root);

    /* root still valid */
    ASSERT_EQ(handle_table_get_object(&ht, h_root, &out, &rout), IRIS_OK);
    kobject_release(out);

    /* children + grandchild all revoked */
    ASSERT_EQ(handle_table_get_object(&ht, h_c1, &out, &rout), IRIS_ERR_BAD_HANDLE);
    ASSERT_EQ(handle_table_get_object(&ht, h_c2, &out, &rout), IRIS_ERR_BAD_HANDLE);
    ASSERT_EQ(handle_table_get_object(&ht, h_gc, &out, &rout), IRIS_ERR_BAD_HANDLE);

    /* ── close_all releases remaining (root) ── */
    handle_table_close_all(&ht);
    ASSERT_EQ(handle_table_get_object(&ht, h_root, &out, &rout), IRIS_ERR_BAD_HANDLE);

    /* drop our alloc refs — all four objects destroyed */
    kobject_release(root_obj);
    kobject_release(child1);
    kobject_release(child2);
    kobject_release(gc);
    ASSERT_EQ(g_ht_destroyed, 4);

    /* ── deep revocation chain: O(N) BFS regression ── */
    /* Build a linear chain: root → d1 → d2 → … → d15.
     * The old O(N×depth) loop required 16 passes; the BFS does one. */
    {
        HandleTable  ht2;
        handle_table_init(&ht2);
        g_ht_destroyed = 0;

        struct KObject *objs[16];
        handle_id_t     hids[16];
        for (int i = 0; i < 16; i++) {
            objs[i] = make_ht_obj(KOBJ_CHANNEL);
            ASSERT_NOT_NULL(objs[i]);
        }

        /* Insert root (no parent) */
        hids[0] = handle_table_insert(&ht2, objs[0], RIGHT_READ);
        ASSERT_NE(hids[0], (handle_id_t)HANDLE_INVALID);

        /* Build chain: each node derived from the previous */
        for (int i = 1; i < 16; i++) {
            hids[i] = handle_table_insert_derived(&ht2, objs[i], RIGHT_READ, hids[i-1]);
            ASSERT_NE(hids[i], (handle_id_t)HANDLE_INVALID);
        }

        /* Revoke from root: all 15 derived handles must disappear */
        handle_table_revoke_children(&ht2, hids[0]);

        struct KObject *out2;
        iris_rights_t   rout2;
        ASSERT_EQ(handle_table_get_object(&ht2, hids[0], &out2, &rout2), IRIS_OK);
        kobject_release(out2);  /* release lookup ref */
        for (int i = 1; i < 16; i++) {
            ASSERT_EQ(handle_table_get_object(&ht2, hids[i], &out2, &rout2),
                      IRIS_ERR_BAD_HANDLE);
        }

        handle_table_close_all(&ht2);
        for (int i = 0; i < 16; i++) kobject_release(objs[i]);
    }
}
