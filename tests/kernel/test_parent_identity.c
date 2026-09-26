/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Ledger A-40 — a derivation names its parent by identity, not by location.
 *
 * A slot is a reusable place.  Every caller that installs a capability as the
 * child of another one reads the parent slot first, decides what it is allowed
 * to do, and installs afterwards — and on SMP a sibling thread of the same
 * process can empty that slot and mint something unrelated into it in between.
 * `kcnode_slot_install_linked` used to test only that the parent slot was
 * OCCUPIED, so the new capability would be linked under an ancestor that never
 * authorised it: revoking the true ancestor would not reach it, and revoking
 * the impostor would destroy a capability unrelated to it.  Charter A9, in
 * both directions.
 *
 * The caller now says what it expects to find, and the check runs under the
 * same mdb_lock hold that installs the link.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/rights.h>
#include <iris/kpage.h>

static void pi_destroy(struct KObject *obj) { (void)obj; }
static const struct KObjectOps pi_ops = { .close = NULL, .destroy = pi_destroy };

static struct KObject *pi_obj(kobject_type_t type) {
    struct KObject *o = (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    if (o) kobject_init(o, type, &pi_ops);
    return o;
}

void test_mdb_parent_identity(void) {
    TEST_SUITE("MDB parent identity (ledger A-40)");

    struct KCNode *cn = kcnode_alloc(8);
    ASSERT_NOT_NULL(cn);
    struct KObject *x = pi_obj(KOBJ_ENDPOINT);   /* the real parent */
    struct KObject *y = pi_obj(KOBJ_CHANNEL);    /* what gets derived */
    struct KObject *z = pi_obj(KOBJ_CHANNEL);    /* an impostor */
    ASSERT_NOT_NULL(x); ASSERT_NOT_NULL(y); ASSERT_NOT_NULL(z);

    /* the parent capability */
    ASSERT_EQ(kcnode_mint(cn, 1, x, RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE),
              IRIS_OK);

    /* ── the expectation holds: the link is made ── */
    ASSERT_EQ(kcnode_slot_install_linked(cn, 2, y, RIGHT_READ, 0,
                                         cn, 1, /*parent_expect=*/x,
                                         /*exclusive=*/1, /*legacy=*/0),
              IRIS_OK);

    /*
     * ── the expectation does not hold: refused ──
     * This is the race, frozen: the caller read `z` out of slot 1 and by the
     * time it installs, slot 1 holds `x`.  Before A-40 this succeeded and the
     * capability was parented to `x`.
     */
    uint32_t y_refs   = atomic_load(&y->refcount);
    uint32_t y_active = atomic_load(&y->active_refs);
    ASSERT_EQ(kcnode_slot_install_linked(cn, 3, y, RIGHT_READ, 0,
                                         cn, 1, /*parent_expect=*/z,
                                         /*exclusive=*/1, /*legacy=*/0),
              IRIS_ERR_INVALID_ARG);

    /* refused means nothing changed: no capability, and no reference kept */
    struct KObject *out; iris_rights_t rr;
    ASSERT_EQ(kcnode_fetch(cn, 3, &out, &rr), IRIS_ERR_NOT_FOUND);
    ASSERT_EQ(atomic_load(&y->refcount), y_refs);
    ASSERT_EQ(atomic_load(&y->active_refs), y_active);

    /* ── no expectation: occupancy only, for the callers that have none ── */
    ASSERT_EQ(kcnode_slot_install_linked(cn, 4, y, RIGHT_READ, 0,
                                         cn, 1, /*parent_expect=*/0,
                                         /*exclusive=*/1, /*legacy=*/0),
              IRIS_OK);

    /* ── an EMPTY parent slot is still refused, expectation or not ── */
    ASSERT_EQ(kcnode_slot_install_linked(cn, 5, y, RIGHT_READ, 0,
                                         cn, 6, /*parent_expect=*/0,
                                         /*exclusive=*/1, /*legacy=*/0),
              IRIS_ERR_INVALID_ARG);

    /*
     * ── the derive path asks for identity on its own behalf ──
     * A copy names the same object as its source, so its expected parent is
     * that object; this is the ordinary success case going through it.
     */
    ASSERT_EQ(kcnode_slot_derive(cn, 1, cn, 7, RIGHT_SAME_RIGHTS, 0), IRIS_OK);
    ASSERT_EQ(kcnode_fetch(cn, 7, &out, &rr), IRIS_OK);
    ASSERT_TRUE(out == x);
    kobject_active_release(out); kobject_release(out);

    kobject_release(x); kobject_release(y); kobject_release(z);
    kcnode_close(cn);
}
