/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Ledger A-39 — CNode teardown depth does not follow capability depth.
 *
 * Emptying a slot that names another CNode ends that CNode's last active
 * reference and runs its close hook, which empties ITS slots.  Written as
 * recursion the stack depth is whatever depth ring 3 nested its CNodes to,
 * and nothing caps that: CSPACE_MAX_DEPTH bounds walking a CPtr, not nesting,
 * and a chain is built bottom-up with every link at depth 1 in the builder's
 * own CSpace.  A core has one 4 KiB stack with nothing mapped below it.
 *
 * The measurement here is the stack frame the DEEPEST destructor runs in.  A
 * marker object sits in the tail CNode's slot and records its frame address
 * when it dies; the same teardown is then run for a short chain and a long
 * one.  Recursive teardown moves that address by the length of the chain.
 * Iterative teardown does not move it at all.
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/rights.h>
#include <iris/kpage.h>
#include <stdint.h>

static uintptr_t g_deepest_frame;
static int       g_marker_destroyed;

static void marker_destroy(struct KObject *obj) {
    (void)obj;
    g_deepest_frame = (uintptr_t)__builtin_frame_address(0);
    g_marker_destroyed++;
}
static const struct KObjectOps marker_ops = {
    .close = NULL, .destroy = marker_destroy
};

/*
 * Build a chain of `len` nested CNodes with a marker in the tail, then do
 * exactly what ring 3 does to bring it down: delete the ONE capability that
 * names the head.  Returns the frame address the marker's destructor ran in.
 */
static uintptr_t teardown_chain(int len) {
    static struct KCNode *n[4096];

    for (int i = 0; i < len; i++) {
        n[i] = kcnode_alloc(4);
        ASSERT_NOT_NULL(n[i]);
        if (!n[i]) return 0;
    }
    for (int i = 0; i + 1 < len; i++)
        ASSERT_EQ(kcnode_mint(n[i], 1, &n[i + 1]->base,
                              RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE),
                  IRIS_OK);

    struct KObject *marker =
        (struct KObject *)kpage_alloc((uint32_t)sizeof(struct KObject));
    ASSERT_NOT_NULL(marker);
    if (!marker) return 0;
    kobject_init(marker, KOBJ_CHANNEL, &marker_ops);
    ASSERT_EQ(kcnode_mint(n[len - 1], 2, marker, RIGHT_READ), IRIS_OK);
    kobject_release(marker);            /* the tail CNode's slot holds it now */

    /* every link but the head is named only by its parent's slot */
    for (int i = 1; i < len; i++) kobject_release(&n[i]->base);

    /* the one capability ring 3 holds to the head of the chain */
    struct KCNode *holder = kcnode_alloc(4);
    ASSERT_NOT_NULL(holder);
    if (!holder) return 0;
    ASSERT_EQ(kcnode_mint(holder, 1, &n[0]->base,
                          RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE), IRIS_OK);
    kobject_release(&n[0]->base);

    g_deepest_frame = 0;
    g_marker_destroyed = 0;
    ASSERT_EQ(kcnode_slot_delete(holder, 1), IRIS_OK);   /* the whole attack */
    ASSERT_EQ(g_marker_destroyed, 1);                    /* the tail did die */

    kobject_release(&holder->base);
    return g_deepest_frame;
}

void test_cnode_teardown_depth(void) {
    TEST_SUITE("CNode teardown depth (ledger A-39)");

    uint32_t live_before = kcnode_live_count();

    uintptr_t shallow = teardown_chain(4);
    uintptr_t deep    = teardown_chain(200);
    ASSERT_TRUE(shallow != 0u);
    ASSERT_TRUE(deep != 0u);

    /*
     * 196 extra links.  Recursive teardown costs ~177 bytes each, so a broken
     * kernel lands ~34 KB further down — eight times the whole core stack.
     * The bound is deliberately loose: what is being asserted is that the
     * distance does not scale with the chain, not an exact frame layout.
     */
    uintptr_t drift = (shallow > deep) ? (shallow - deep) : (deep - shallow);
    ASSERT_TRUE(drift < 512u);

    /* and the chains are gone, not merely unreachable */
    ASSERT_EQ(kcnode_live_count(), live_before);
}
