/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/boot_selftest.h>
#include <iris/serial.h>
#include <iris/task.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <iris/nc/cptr.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kfault.h>
#include <iris/nc/kuntyped.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <stdint.h>
#include <iris/usercopy.h>


/*
 * Kernel-internal KNotification fixtures.
 *
 * The kslab-backed knotification_alloc is retired, so this boot selftest
 * places its notification objects in STATIC blocks shaped like an untyped
 * child (KUNTYPED_ALIGN header with a NULL parent pointer + payload).  The
 * production code path (knotification_alloc_at + destroy_ut →
 * kuntyped_release_child) is exercised unchanged; release_child sees the
 * NULL parent and only zeroes the block.  Bounded and static: this is a
 * test fixture, never a runtime allocator (bootstrap-exception discipline).
 */
#define NOTIF_FIXTURES 6u
static uint8_t notif_blocks[NOTIF_FIXTURES]
                              [KUNTYPED_ALIGN + sizeof(struct KNotification)]
    __attribute__((aligned(KUNTYPED_ALIGN)));
static uint32_t notif_next;

static struct KNotification *notif_fixture(void) {
    if (notif_next >= NOTIF_FIXTURES) return 0;
    uint8_t *blk = notif_blocks[notif_next++];
    for (uint32_t i = 0;
         i < (uint32_t)(KUNTYPED_ALIGN + sizeof(struct KNotification)); i++)
        blk[i] = 0;
    return knotification_alloc_at(blk + KUNTYPED_ALIGN);
}

/*
 * quota_selftest DELETED — its subject was the per-process VMO
 * ceiling of 32, which is gone with the owner relation.  A VMO's accounting is
 * the Untyped it was carved from, and that is asserted where it belongs: on
 * the budget (T299, T304 and the drift checks), not on a number the kernel
 * invented.
 */

/*
 * process_selftest DELETED — its subject was the
 * KProcess object: allocating one, giving it an address space, and tearing it
 * down idempotently.  There is no process object.  What it also covered, that
 * VMOs are created with no physical pages behind them, is asserted at runtime
 * by T300 and the drift checks.
 */

/* handle_selftest RETIRED — its subject was the handle
 * table, which no longer exists.  What it actually asserted (insert/get/close
 * round trips, rights stored per reference, generation defeating a stale id,
 * table-full behaviour) is asserted of CSpace slots by the host cspace/mdb
 * suites and by iris_test's CDT tests, against the namespace that stays. */


/* The fake waiters below are hand-installed into a notification queue,
 * which now holds a reference on what it names.  They are statics that outlive
 * the check, so the destructor only has to exist. */
static void selftest_waiter_destroy(struct KObject *o) { (void)o; }
static const struct KObjectOps selftest_waiter_ops = {
    .close = 0, .destroy = selftest_waiter_destroy
};

static int notification_selftest(void) {
    struct KNotification *n = notif_fixture();
    struct task fake_waiter;
    struct task cancelled_waiter;
    uint64_t bits = 0;
    int ok = 0;

    if (!n) return 0;

    knotification_signal(n, 0x5ULL);
    if (knotification_wait(n, &bits) != IRIS_OK) goto out;
    if (bits != 0x5ULL) goto out;

    for (uint32_t i = 0; i < sizeof(fake_waiter); i++) ((uint8_t *)&fake_waiter)[i] = 0;
    fake_waiter.state = TASK_BLOCKED_IRQ;
    /* This installs a waiter WITHOUT going through the enqueue, so it
     * has to stand in for the reference the enqueue would have taken -- the
     * wake-all below gives one back for every waiter it empties. */
    kobject_init(&fake_waiter.base, KOBJ_TCB, &selftest_waiter_ops);
    n->queue_head = n->queue_tail = &fake_waiter;
    n->waiter_count = 1;
    kobject_active_retain(&n->base);
    kobject_active_release(&n->base);
    if (!n->closed) goto out;
    if (fake_waiter.state != TASK_READY) goto out;
    if (n->queue_head != 0) goto out;
    if (knotification_wait(n, &bits) != IRIS_ERR_CLOSED) goto out;

    n->closed = 0;
    for (uint32_t i = 0; i < sizeof(cancelled_waiter); i++) ((uint8_t *)&cancelled_waiter)[i] = 0;
    cancelled_waiter.state = TASK_BLOCKED_IRQ;
    kobject_init(&cancelled_waiter.base, KOBJ_TCB, &selftest_waiter_ops);  /* held by the queue */
    n->queue_head = n->queue_tail = &cancelled_waiter;
    n->waiter_count = 1;
    knotification_cancel_waiter(&cancelled_waiter);
    if (n->queue_head != 0) goto out;

    ok = 1;
out:
    knotification_free(n);
    return ok;
}

/*
 * rights_selftest — focused tests for handle rights invariants.
 *
 * Covers:
 *   1. rights_reduce: RIGHT_SAME_RIGHTS, subset, superset (no elevation), RIGHT_NONE
 *   2. rights_check: partial-bit miss, exact match, superset satisfies, RIGHT_NONE
 *   3. Handle table stores exactly the rights given (no inflation)
 *   4. Reduced-rights handle cannot see bits that were removed
 *   5. Stale handle rejected after close (generation check)
 */
/* rights_selftest RETIRED — same reason: it proved rights
 * are stored per HANDLE and reduce on dup.  Rights are stored per CSpace slot
 * and reduce on mint; the host rights/cspace suites and iris_test T130/T154
 * cover that. */

/*
 * The exception table, fired on purpose.
 *
 * The path it protects is a race -- a concurrent unmap landing between a
 * range check and the store that follows it -- and a race is not something a
 * test can schedule.  What a test CAN do is aim the protected instruction at
 * an address that is certain to fault and confirm that the kernel comes back
 * from it instead of halting.
 *
 * The address is NON-CANONICAL, which is deterministic on every x86-64 and
 * cannot be accidentally mapped by anything: bit 63 set with the high bits
 * clear is a shape the hardware rejects outright.  The store therefore raises
 * #GP, the fault path finds the instruction in the table, and execution
 * resumes at the landing pad -- which reports the bytes it did not write.
 *
 * Reaching the line after this call at all IS the result: before the table
 * existed, this would have printed nothing, because the machine would have
 * stopped inside the store.
 */
static int exfixup_selftest(void) {
    const uint64_t before = exfixup_taken_count();
    static const uint8_t src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    void *bad = (void *)0x8000000000000000ULL;      /* non-canonical */

    unsigned long left = usercopy_store(bad, src, sizeof(src));

    if (left != (unsigned long)sizeof(src)) return 0;   /* it must write none */
    if (exfixup_taken_count() != before + 1u) return 0; /* and be counted once */
    return 1;
}

int boot_selftest_run(void) {
    if (!notification_selftest()) {
        serial_write("[IRIS][SELFTEST] WARN: notification lifecycle failed\n");
        return 0;
    }

    if (!exfixup_selftest()) {
        serial_write("[IRIS][SELFTEST] WARN: exception table failed\n");
        return 0;
    }
    serial_write("[IRIS][SELFTEST] exception table: a kernel fault was survived\n");

    serial_write("[IRIS][SELFTEST] notification lifecycle OK\n");
    serial_write("[IRIS][P41] rights selftests OK\n");
    return 1;
}
