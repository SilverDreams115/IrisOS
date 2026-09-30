/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_final_cap.c — D-7's predicate, and the exact shape of what it cannot
 * see yet.
 *
 * `kcnode_slot_is_final_cap` is seL4's `isFinalCapability`: no MDB neighbour
 * names this slot's object, so deleting the slot finalises the object.  It is
 * what has to replace `active_refs` before D-7 can be retired, and
 * `spec-exhaustive` has checked it against the reference count on 199177
 * finality decisions with no disagreement.
 *
 * Every one of those decisions was a CSPACE operation, because `spec_driver.c`
 * drives `kcnode`/`cspace` and never touches the TCB paths.  The ledger says
 * so in the same paragraph as the number.  This file makes the limitation an
 * EXECUTABLE fact instead of a sentence, because a caveat in prose is a caveat
 * somebody deletes.
 *
 * The gap: IRIS lets a THREAD hold a reference to an object that no slot
 * names — its CSpace root, its VSpace, its fault-handler endpoint, its bound
 * notification, its IPC buffer frame.  Five relationships, nine `active_refs`
 * acquisition sites.  seL4 has none of them, because all five are capabilities
 * in the TCB's own slots (`tcbCTable`, `tcbVTable`, `tcbFaultHandler`,
 * `tcbBoundNotification`, `tcbIPCBufferFrame`) and the MDB therefore sees
 * every reference that exists.
 *
 * ── When the TCB gets capability slots, THIS TEST MUST FAIL ────────────────
 *
 * It asserts a disagreement.  The disagreement is the bug D-7's retirement
 * removes, so the day it stops reproducing is the day the retirement works,
 * and this file is then rewritten to assert the equality instead.  A test that
 * pins a defect has to say that about itself or it becomes a test that defends
 * one.
 */
#include "framework.h"
#include <iris/nc/kcnode.h>
#include <iris/nc/kobject.h>
#include <iris/nc/knotification.h>
#include <iris/kpage.h>
#include <iris/nc/rights.h>
#include <stdatomic.h>

static int g_destroyed;
static void fc_destroy(struct KObject *o) { (void)o; g_destroyed++; }
static const struct KObjectOps fc_ops = { .close = NULL, .destroy = fc_destroy };

void test_final_cap(void) {
    TEST_SUITE("isFinalCapability, and the references it cannot see (D-7)");

    struct KCNode *cn = kcnode_alloc(8);
    ASSERT_NOT_NULL(cn);
    if (!cn) return;

    struct KObject *o =
        (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    ASSERT_NOT_NULL(o);
    if (!o) return;
    kobject_init_test(o, KOBJ_NOTIFICATION, &fc_ops);

    /* One slot names it, and the slot's reference is the only one. */
    ASSERT_EQ(kcnode_mint(cn, 1, o, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
    kobject_release(o);                 /* drop the birth reference */

    /*
     * Agreement, which is the case spec-exhaustive covers: the MDB says this
     * is the last capability, and the count says this is the last reference.
     */
    ASSERT_EQ(kcnode_slot_is_final_cap(&cn->slots[1]), 1);
    ASSERT_EQ(atomic_load(&o->active_refs), 1u);

    /*
     * Now a THREAD takes a reference, the way SYS_TCB_BIND_NOTIFICATION does
     * for `target->exit_notif` — from outside the CSpace, on an object no
     * extra slot names.  BOTH retains, because that is what the kernel takes
     * there and taking only the active one models a relationship that does
     * not exist: the object would still be destroyed when the slot went, and
     * the test would be asserting something about my simplification rather
     * than about IRIS.
     */
    kobject_retain(o);
    kobject_active_retain(o);

    /*
     * THE GAP.  The MDB is unchanged — still one slot, still no neighbour —
     * so the predicate still says "last capability".  The count knows better.
     *
     * Believing the predicate here would finalise a notification a thread is
     * still pointing at.  That is why D-7 cannot be retired by swapping the
     * mechanism in: it has to be retired by making these five relationships
     * capabilities first, so that there is nothing left for the MDB not to
     * see.
     */
    ASSERT_EQ(kcnode_slot_is_final_cap(&cn->slots[1]), 1);   /* says final */
    ASSERT_EQ(atomic_load(&o->active_refs), 2u);             /* is not */

    /* And the object survives the slot going, because the count is right. */
    g_destroyed = 0;
    ASSERT_EQ(kcnode_slot_delete(cn, 1), IRIS_OK);
    ASSERT_EQ(g_destroyed, 0);

    kobject_active_release(o);          /* the thread lets go... */
    kobject_release(o);                 /* ...both of them, and now it dies */
    ASSERT_EQ(g_destroyed, 1);

    kobject_release(&cn->base);
}
