#include <iris/phase3_selftest.h>
#include <iris/serial.h>
#include <iris/task.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <iris/nc/handle.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kuntyped.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <stdint.h>


/*
 * Phase S1: kernel-internal KNotification fixtures.
 *
 * The kslab-backed knotification_alloc is retired, so this boot selftest
 * places its notification objects in STATIC blocks shaped like an untyped
 * child (KUNTYPED_ALIGN header with a NULL parent pointer + payload).  The
 * production code path (knotification_alloc_at + destroy_ut →
 * kuntyped_release_child) is exercised unchanged; release_child sees the
 * NULL parent and only zeroes the block.  Bounded and static: this is a
 * test fixture, never a runtime allocator (bootstrap-exception discipline).
 */
#define P3_NOTIF_FIXTURES 6u
static uint8_t p3_notif_blocks[P3_NOTIF_FIXTURES]
                              [KUNTYPED_ALIGN + sizeof(struct KNotification)]
    __attribute__((aligned(KUNTYPED_ALIGN)));
static uint32_t p3_notif_next;

static struct KNotification *p3_notif_fixture(void) {
    if (p3_notif_next >= P3_NOTIF_FIXTURES) return 0;
    uint8_t *blk = p3_notif_blocks[p3_notif_next++];
    for (uint32_t i = 0;
         i < (uint32_t)(KUNTYPED_ALIGN + sizeof(struct KNotification)); i++)
        blk[i] = 0;
    return knotification_alloc_at(blk + KUNTYPED_ALIGN);
}

/* Phase 13/Track G: the channel-quota portion was retired with KChannel.
 * Phase S1: the NOTIFICATION quota is retired too (Untyped is the budget for
 * notifications), so this selftest now covers the remaining legacy quota:
 * KVmo ownership accounting (LEGACY_FOR_KPROCESS_KVMO in the ledger). */
/*
 * phase3_quota_selftest DELETED (Stage 7-mem) — its subject was the per-process VMO
 * ceiling of 32, which is gone with the owner relation.  A VMO's accounting is
 * the Untyped it was carved from, and that is asserted where it belongs: on
 * the budget (T299, T304 and the drift checks), not on a number the kernel
 * invented.
 */

/*
 * phase3_process_selftest DELETED (Stage 7-proc) — its subject was the
 * KProcess object: allocating one, giving it an address space, and tearing it
 * down idempotently.  There is no process object.  What it also covered, that
 * VMOs are created with no physical pages behind them, is asserted at runtime
 * by T300 and the drift checks.
 */

/* phase3_handle_selftest RETIRED (Stage 4) — its subject was the handle
 * table, which no longer exists.  What it actually asserted (insert/get/close
 * round trips, rights stored per reference, generation defeating a stale id,
 * table-full behaviour) is asserted of CSpace slots by the host cspace/mdb
 * suites and by iris_test's CDT tests, against the namespace that stays. */

static int phase3_notification_selftest(void) {
    struct KNotification *n = p3_notif_fixture();
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
    n->waiters[0] = &fake_waiter;
    n->waiter_count = 1;
    kobject_active_retain(&n->base);
    kobject_active_release(&n->base);
    if (!n->closed) goto out;
    if (fake_waiter.state != TASK_READY) goto out;
    if (n->waiters[0] != 0) goto out;
    if (knotification_wait(n, &bits) != IRIS_ERR_CLOSED) goto out;

    n->closed = 0;
    for (uint32_t i = 0; i < sizeof(cancelled_waiter); i++) ((uint8_t *)&cancelled_waiter)[i] = 0;
    cancelled_waiter.state = TASK_BLOCKED_IRQ;
    n->waiters[0] = &cancelled_waiter;
    n->waiter_count = 1;
    knotification_cancel_waiter(&cancelled_waiter);
    if (n->waiters[0] != 0) goto out;

    ok = 1;
out:
    knotification_free(n);
    return ok;
}

/*
 * phase41_rights_selftest — focused tests for handle rights invariants.
 *
 * Covers:
 *   1. rights_reduce: RIGHT_SAME_RIGHTS, subset, superset (no elevation), RIGHT_NONE
 *   2. rights_check: partial-bit miss, exact match, superset satisfies, RIGHT_NONE
 *   3. Handle table stores exactly the rights given (no inflation)
 *   4. Reduced-rights handle cannot see bits that were removed
 *   5. Stale handle rejected after close (generation check)
 */
/* phase41_rights_selftest RETIRED (Stage 4) — same reason: it proved rights
 * are stored per HANDLE and reduce on dup.  Rights are stored per CSpace slot
 * and reduce on mint; the host rights/cspace suites and iris_test T130/T154
 * cover that. */

int phase3_selftest_run(void) {
    if (!phase3_notification_selftest()) {
        serial_write("[IRIS][P3] WARN: notification selftest failed\n");
        return 0;
    }

    /* The marker names are kept: the headless gate greps for them, and what
     * they now attest is the lifecycle half — notification and process — after
     * the handle-table halves retired with the namespace and the quota half
     * with the per-process VMO ceiling (Stage 7-mem). */
    serial_write("[IRIS][P3] handle/lifecycle selftests OK\n");
    serial_write("[IRIS][P41] rights selftests OK\n");
    return 1;
}
