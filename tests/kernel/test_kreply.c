/* SPDX-License-Identifier: Apache-2.0 */
#include "framework.h"
#include <iris/nc/kreply.h>
#include <iris/nc/kobject.h>
#include <iris/task.h>
#include <iris/paging.h>
#include <stdatomic.h>


/* Phase S1: kreply_alloc(caller) is retired — reproduce the old semantics for
 * these tests: placement-init in an untyped-child block, then stage+bind the
 * caller (the production rendezvous sequence). */
/*
 * A-43: the binding holds a reference on the caller, so a test caller has to
 * BE a KObject.  These are stack structs that outlive nothing; the destructor
 * only has to exist, and the test asserts the reference lands and leaves.
 */
static void test_task_destroy(struct KObject *o) { (void)o; }
static const struct KObjectOps test_task_ops = {
    .close = NULL, .destroy = test_task_destroy
};
static void test_task_init(struct task *t) {
    kobject_init(&t->base, KOBJ_TCB, &test_task_ops);   /* refcount 1 */
}

static struct KReply *test_kreply_alloc(struct task *caller) {
    struct KReply *r = TEST_UT_ALLOC(struct KReply, kreply_alloc_at);
    if (r && caller) {
        (void)kreply_stage(r);
        (void)kreply_bind_caller(r, caller);
    }
    return r;
}

void test_kreply(void) {
    TEST_SUITE("kreply");

    /* ── alloc with NULL caller ── */
    struct KReply *r = test_kreply_alloc(NULL);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(atomic_load(&r->base.refcount),    1u);
    ASSERT_EQ(atomic_load(&r->base.active_refs), 0u);
    ASSERT_NULL(r->caller);

    /* cancel with NULL caller → no-op */
    kreply_cancel_caller(r);
    ASSERT_NULL(r->caller);
    kobject_release(&r->base);

    /* ── alloc with non-NULL caller ── */
    struct task caller = { 0 };
    test_task_init(&caller);
    struct KReply *r2 = test_kreply_alloc(&caller);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ(r2->caller, &caller);
    /* A-43: bound means held — this is what makes it safe to walk the task
     * after the reply object's lock is dropped. */
    ASSERT_EQ(atomic_load(&caller.base.refcount), 2u);

    /* cancel_caller clears caller and sets ipc_ep_closed */
    kreply_cancel_caller(r2);
    ASSERT_NULL(r2->caller);
    ASSERT_EQ(caller.ipc_ep_closed, (uint8_t)1);
    ASSERT_EQ(atomic_load(&caller.base.refcount), 1u);   /* and given back */

    /* second cancel is idempotent */
    caller.ipc_ep_closed = 0;
    kreply_cancel_caller(r2);
    ASSERT_NULL(r2->caller);
    ASSERT_EQ(caller.ipc_ep_closed, (uint8_t)0);  /* not set again */
    kobject_release(&r2->base);

    /* ── close op fires cancel when handle is dropped ── */
    struct task caller3 = { 0 };
    test_task_init(&caller3);
    struct KReply *r3 = test_kreply_alloc(&caller3);
    ASSERT_NOT_NULL(r3);
    ASSERT_EQ(r3->caller, &caller3);
    /* fire close by cycling active_refs */
    kobject_active_retain(&r3->base);   /* active_refs = 1 */
    kobject_active_release(&r3->base);  /* active_refs = 0 → close → cancels caller3 */
    ASSERT_EQ(caller3.ipc_ep_closed, (uint8_t)1);
    ASSERT_NULL(r3->caller);
    ASSERT_EQ(atomic_load(&caller3.base.refcount), 1u);  /* close gave it back */
    kobject_release(&r3->base);

    /* ── kreply_cancel_caller(NULL) is safe ── */
    kreply_cancel_caller(NULL);

    /* ── alloc failure path (Phase S1: no kslab — placement on NULL block,
     * i.e. the untyped carve failed upstream) ── */
    struct KReply *rf = kreply_alloc_at(NULL);
    ASSERT_NULL(rf);
}
