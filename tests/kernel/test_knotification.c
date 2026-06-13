#include "framework.h"
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/kpage.h>
#include <iris/task.h>
#include <stdatomic.h>

void test_knotification(void) {
    TEST_SUITE("knotification");

    /* ── alloc / release lifecycle ── */
    uint32_t before = knotification_live_count();
    struct KNotification *n = knotification_alloc();
    ASSERT_NOT_NULL(n);
    ASSERT_EQ(atomic_load(&n->base.refcount),    1u);
    ASSERT_EQ(atomic_load(&n->base.active_refs), 0u);
    ASSERT_EQ(knotification_live_count(), before + 1u);

    /* ── signal accumulates bits (OR semantics) ── */
    knotification_signal(n, 0x3);
    knotification_signal(n, 0xC);
    /* poll is a destructive atomic-exchange read */
    ASSERT_EQ(knotification_poll(n), (uint64_t)0xF);
    ASSERT_EQ(knotification_poll(n), (uint64_t)0);  /* consumed */

    /* ── wait fast-path: bits already set → immediate return ── */
    knotification_signal(n, 0xAB);
    uint64_t got = 0;
    ASSERT_EQ(knotification_wait(n, &got), IRIS_OK);
    ASSERT_EQ(got, (uint64_t)0xAB);
    ASSERT_EQ(knotification_poll(n), (uint64_t)0);  /* wait cleared the bits */

    /* ── multi-signal accumulation before consume ── */
    knotification_signal(n, 0x1);
    knotification_signal(n, 0x2);
    knotification_signal(n, 0x4);
    got = 0;
    ASSERT_EQ(knotification_wait(n, &got), IRIS_OK);
    ASSERT_EQ(got, (uint64_t)0x7);  /* 0x1|0x2|0x4 */

    kobject_release(&n->base);
    ASSERT_EQ(knotification_live_count(), before);

    /* ── close → wait returns IRIS_ERR_CLOSED (no bits) ── */
    struct KNotification *n2 = knotification_alloc();
    ASSERT_NOT_NULL(n2);
    /* cycle active_refs to 0 to fire the close op */
    kobject_active_retain(&n2->base);   /* active_refs = 1 */
    kobject_active_release(&n2->base);  /* active_refs = 0 → close → n2->closed = 1 */
    uint64_t got2 = 99;
    ASSERT_EQ(knotification_wait(n2, &got2), IRIS_ERR_CLOSED);
    ASSERT_EQ(got2, (uint64_t)99);  /* out_bits not written on error */

    /* ── signal after close is silently discarded ── */
    knotification_signal(n2, 0xFF);
    ASSERT_EQ(knotification_poll(n2), (uint64_t)0);  /* still 0 */

    kobject_release(&n2->base);

    /* ── wait_timeout with null task and no bits returns IRIS_ERR_INTERNAL ── */
    struct KNotification *n3 = knotification_alloc();
    ASSERT_NOT_NULL(n3);
    uint64_t got3 = 0;
    ASSERT_EQ(knotification_wait_timeout(n3, &got3, 0), IRIS_ERR_INTERNAL);
    kobject_release(&n3->base);

    /* ── Fase 10: close WHILE a waiter is blocked wakes + clears it ──
     * Closes the S0 gap: a task blocked in knotification_wait must be woken
     * (with a defined error) when the notification is closed, with no leak
     * and no deadlock — not just the close-before-wait case above. */
    struct KNotification *n4 = knotification_alloc();
    ASSERT_NOT_NULL(n4);
    struct task waiter;
    for (uint32_t i = 0; i < sizeof(waiter); i++) ((uint8_t *)&waiter)[i] = 0;
    waiter.state = TASK_BLOCKED_IRQ;          /* simulate a blocked waiter */
    n4->waiters[0]   = &waiter;
    n4->waiter_count = 1u;
    /* Fire the close op (active_refs → 0): wake-all must clear the array. */
    kobject_active_retain(&n4->base);
    kobject_active_release(&n4->base);
    ASSERT_EQ(n4->closed, 1u);
    ASSERT_EQ(n4->waiter_count, 0u);
    ASSERT_NULL(n4->waiters[0]);
    /* Idempotent: a post-close wait returns CLOSED, never hangs. */
    uint64_t got4 = 7;
    ASSERT_EQ(knotification_wait(n4, &got4), IRIS_ERR_CLOSED);
    ASSERT_EQ(got4, (uint64_t)7);
    kobject_release(&n4->base);

    /* ── cancel_waiter(NULL) is safe ── */
    knotification_cancel_waiter(NULL);
}
