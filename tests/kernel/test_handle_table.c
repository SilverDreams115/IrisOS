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

    /* Fase S4 (Etapa 3): the handle table's parallel derivation tree
     * (insert_derived / revoke_children / derivation_parent[]) is DELETED.
     * Derivation and revocation live in the CSpace CDT only — see
     * test_mdb.c for the structural suite and T071/T072/T127/T131 for the
     * runtime coverage.  The handle table is now a flat reference table. */

    /* ── close_all releases every remaining entry ── */
    g_ht_destroyed = 0;
    struct KObject *a1 = make_ht_obj(KOBJ_ENDPOINT);
    struct KObject *a2 = make_ht_obj(KOBJ_NOTIFICATION);
    ASSERT_NOT_NULL(a1);
    ASSERT_NOT_NULL(a2);
    handle_id_t h_a1 = handle_table_insert(&ht, a1, RIGHT_READ);
    handle_id_t h_a2 = handle_table_insert(&ht, a2, RIGHT_READ);
    ASSERT_NE(h_a1, (handle_id_t)HANDLE_INVALID);
    ASSERT_NE(h_a2, (handle_id_t)HANDLE_INVALID);

    handle_table_close_all(&ht);
    ASSERT_EQ(handle_table_get_object(&ht, h_a1, &out, &rout), IRIS_ERR_BAD_HANDLE);
    ASSERT_EQ(handle_table_get_object(&ht, h_a2, &out, &rout), IRIS_ERR_BAD_HANDLE);

    /* drop our alloc refs — both objects destroyed */
    kobject_release(a1);
    kobject_release(a2);
    ASSERT_EQ(g_ht_destroyed, 2);

    /* ── table capacity / lifetime over many entries ── */
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

        /* Fase S4: the derivation CHAIN test moved to the CDT suite; what
         * remains here is plain table capacity/lifetime. */
        for (int i = 0; i < 16; i++) {
            hids[i] = handle_table_insert(&ht2, objs[i], RIGHT_READ);
            ASSERT_NE(hids[i], (handle_id_t)HANDLE_INVALID);
        }
        struct KObject *out2;
        iris_rights_t   rout2;
        for (int i = 0; i < 16; i++) {
            ASSERT_EQ(handle_table_get_object(&ht2, hids[i], &out2, &rout2), IRIS_OK);
            kobject_release(out2);
        }

        handle_table_close_all(&ht2);
        for (int i = 0; i < 16; i++) kobject_release(objs[i]);
    }

    /* ── A1.7 instrumentation counters ── */
    {
        HandleTable ht3;
        handle_table_init(&ht3);
        ASSERT_EQ(ht3.live, 0u);
        ASSERT_EQ(ht3.hwm, 0u);
        ASSERT_EQ(ht3.inserts, 0u);
        ASSERT_EQ(ht3.removes, 0u);

        uint32_t g0  = handle_table_global_hwm;
        uint32_t ti0 = handle_table_type_inserts[KOBJ_NOTIFICATION];
        uint32_t tr0 = handle_table_type_removes[KOBJ_NOTIFICATION];

        struct KObject *o1 = make_ht_obj(KOBJ_NOTIFICATION);
        struct KObject *o2 = make_ht_obj(KOBJ_NOTIFICATION);
        ASSERT_NOT_NULL(o1);
        ASSERT_NOT_NULL(o2);
        handle_id_t a = handle_table_insert(&ht3, o1, RIGHT_READ);
        handle_id_t b = handle_table_insert(&ht3, o2, RIGHT_READ);
        ASSERT_NE(a, (handle_id_t)HANDLE_INVALID);
        ASSERT_NE(b, (handle_id_t)HANDLE_INVALID);
        ASSERT_EQ(ht3.live, 2u);
        ASSERT_EQ(ht3.hwm, 2u);
        ASSERT_EQ(ht3.inserts, 2u);
        ASSERT_EQ(handle_table_type_inserts[KOBJ_NOTIFICATION], ti0 + 2u);
        ASSERT_TRUE(handle_table_global_hwm >= 2u);
        ASSERT_TRUE(handle_table_global_hwm >= g0);

        /* close decrements live but hwm is sticky */
        ASSERT_EQ(handle_table_close(&ht3, b), IRIS_OK);
        ASSERT_EQ(ht3.live, 1u);
        ASSERT_EQ(ht3.hwm, 2u);
        ASSERT_EQ(ht3.removes, 1u);
        ASSERT_EQ(handle_table_type_removes[KOBJ_NOTIFICATION], tr0 + 1u);

        /* re-insert to the old peak: hwm unchanged; one past it: hwm rises */
        handle_id_t c = handle_table_insert(&ht3, o2, RIGHT_READ);
        ASSERT_NE(c, (handle_id_t)HANDLE_INVALID);
        ASSERT_EQ(ht3.live, 2u);
        ASSERT_EQ(ht3.hwm, 2u);
        handle_id_t d = handle_table_insert(&ht3, o1, RIGHT_READ);
        ASSERT_NE(d, (handle_id_t)HANDLE_INVALID);
        ASSERT_EQ(ht3.hwm, 3u);

        /* bulk close balances the books */
        handle_table_close_all(&ht3);
        ASSERT_EQ(ht3.live, 0u);
        ASSERT_EQ(ht3.inserts, 4u);
        ASSERT_EQ(ht3.removes, 4u);
        ASSERT_EQ(handle_table_type_inserts[KOBJ_NOTIFICATION], ti0 + 4u);
        ASSERT_EQ(handle_table_type_removes[KOBJ_NOTIFICATION], tr0 + 4u);

        kobject_release(o1);
        kobject_release(o2);
    }

    /* Generation counter vs encoded field: they must agree at every point.
     *
     * handle_id_make packs the generation into HANDLE_GEN_MAX bits and every
     * lookup compares the STORED counter against handle_id_gen() of the id.
     * If the counter is ever allowed past the field width the two disagree and
     * the slot becomes permanently unusable — occupied, but every handle minted
     * from it decodes to a value the counter never holds, so every lookup
     * returns BAD_HANDLE forever.
     *
     * Seeded at the boundary rather than counted up to it: the failure is a
     * wrap, so the only interesting iterations are the ones around it. */
    {
        HandleTable ht4;
        handle_table_init(&ht4);
        struct KObject *o = make_ht_obj(KOBJ_NOTIFICATION);
        ASSERT_NOT_NULL(o);

        /* Park slot 0's counter one step below the field limit. */
        ht4.gen[0] = HANDLE_GEN_MAX - 1u;

        for (uint32_t i = 0; i < 4u; i++) {
            /* Force slot 0 every time: after a close the allocator's hint has
             * moved on, so without this the loop walks fresh slots and never
             * re-uses the one whose counter is parked at the boundary — which
             * is the entire point. */
            ht4.next_hint = 0u;
            handle_id_t h = handle_table_insert(&ht4, o, RIGHT_READ);
            ASSERT_EQ(handle_id_slot(h), 0u);
            ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);
            uint32_t slot = handle_id_slot(h);
            /* the encoded generation is exactly what the table stored */
            ASSERT_EQ(handle_id_gen(h), ht4.gen[slot]);
            /* 0 is reserved as "never issued" and must never be minted */
            ASSERT_NE(handle_id_gen(h), 0u);
            /* the handle resolves while open, and is stale once closed */
            struct KObject *back; iris_rights_t r;
            ASSERT_EQ(handle_table_get_object(&ht4, h, &back, &r), IRIS_OK);
            kobject_release(back);
            ASSERT_EQ(handle_table_close(&ht4, h), IRIS_OK);
            ASSERT_EQ(handle_table_get_object(&ht4, h, &back, &r),
                      IRIS_ERR_BAD_HANDLE);
        }
        kobject_release(o);
    }
}
