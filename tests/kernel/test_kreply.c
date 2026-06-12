#include "framework.h"
#include <iris/nc/kreply.h>
#include <iris/nc/kobject.h>
#include <iris/task.h>
#include <iris/paging.h>
#include <stdatomic.h>

void test_kreply(void) {
    TEST_SUITE("kreply");

    /* ── alloc with NULL caller ── */
    struct KReply *r = kreply_alloc(NULL);
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
    struct KReply *r2 = kreply_alloc(&caller);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ(r2->caller, &caller);

    /* cancel_caller clears caller and sets ipc_ep_closed */
    kreply_cancel_caller(r2);
    ASSERT_NULL(r2->caller);
    ASSERT_EQ(caller.ipc_ep_closed, (uint8_t)1);

    /* second cancel is idempotent */
    caller.ipc_ep_closed = 0;
    kreply_cancel_caller(r2);
    ASSERT_NULL(r2->caller);
    ASSERT_EQ(caller.ipc_ep_closed, (uint8_t)0);  /* not set again */
    kobject_release(&r2->base);

    /* ── close op fires cancel when handle is dropped ── */
    struct task caller3 = { 0 };
    struct KReply *r3 = kreply_alloc(&caller3);
    ASSERT_NOT_NULL(r3);
    ASSERT_EQ(r3->caller, &caller3);
    /* fire close by cycling active_refs */
    kobject_active_retain(&r3->base);   /* active_refs = 1 */
    kobject_active_release(&r3->base);  /* active_refs = 0 → close → cancels caller3 */
    ASSERT_EQ(caller3.ipc_ep_closed, (uint8_t)1);
    ASSERT_NULL(r3->caller);
    kobject_release(&r3->base);

    /* ── kreply_cancel_caller(NULL) is safe ── */
    kreply_cancel_caller(NULL);

    /* ── alloc failure injection ── */
    kslab_fail_after(0);
    struct KReply *rf = kreply_alloc(NULL);
    ASSERT_NULL(rf);
    kslab_clear_fail();
}
