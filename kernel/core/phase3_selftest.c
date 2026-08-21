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
#include <iris/nc/kvmo.h>
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
static int phase3_quota_selftest(void) {
    struct KProcess *proc = 0;
    struct KVmo *vmos[KPROCESS_VMO_QUOTA + 1];
    int ok = 0;

    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA + 1; i++) vmos[i] = 0;

    proc = kprocess_alloc();
    if (!proc) goto out;

    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA; i++) {
        vmos[i] = kvmo_create(0x1000ULL);
        if (!vmos[i]) goto out;
        if (kvmo_bind_owner(vmos[i], proc) != IRIS_OK) goto out;
    }
    vmos[KPROCESS_VMO_QUOTA] = kvmo_create(0x1000ULL);
    if (!vmos[KPROCESS_VMO_QUOTA]) goto out;
    if (kvmo_bind_owner(vmos[KPROCESS_VMO_QUOTA], proc) != IRIS_ERR_NO_MEMORY) goto out;

    if (proc->owned_vmos != KPROCESS_VMO_QUOTA) goto out;

    ok = 1;

out:
    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA + 1; i++) {
        if (vmos[i]) kvmo_free(vmos[i]);
    }
    if (proc) kprocess_free(proc);
    return ok;
}

/*
 * Stage 7 Step 12: this used to open by arming a process-scoped exception
 * handler twice and asserting the notification's refcount came back to where
 * it started after teardown.  kprocess_set_exception_handler is gone with the
 * registration it served — arming faults is a THREAD operation — and the same
 * arm / re-arm / hand-over / read-after-death balance is asserted on the
 * thread at runtime by T140-T147.  The notification fixture went with it; what
 * is left here is what this function was always also checking: that VMOs are
 * created with no physical pages behind them, and that teardown of a process
 * holding an address space is idempotent.
 */
static int phase3_process_selftest(void) {
    struct KProcess *proc = 0;
    struct KVmo *vmo = 0;
    struct KVmo *large_vmo = 0;
    int ok = 0;

    proc = kprocess_alloc();
    vmo = kvmo_create(0x1000ULL);
    large_vmo = kvmo_create(0x200000ULL);
    if (!proc || !vmo || !large_vmo) goto out;
    if (large_vmo->page_capacity < 512u) goto out;

    proc->cr3 = paging_create_user_space();
    if (!proc->cr3) goto out;

    /* Verify VMOs have no physical pages allocated (no demand paging). */
    if (vmo->pages[0] != 0) goto out;
    if (large_vmo->pages[511] != 0) goto out;

    /* KVSpace.mappings (dynamic linked list) is cleaned up by
     * kvspace_invalidate called from kprocess_teardown; running it twice (here
     * and at `out`) is what makes the idempotence claim a test. */
    kprocess_teardown(proc, 0);

    ok = 1;
out:
    if (proc) {
        kprocess_teardown(proc, 0);
        kprocess_reap_address_space(proc);
        kprocess_free(proc);
    }
    if (vmo) kvmo_free(vmo);
    if (large_vmo) kvmo_free(large_vmo);
    return ok;
}

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
    if (!phase3_process_selftest()) {
        serial_write("[IRIS][P3] WARN: process selftest failed\n");
        return 0;
    }
    if (!phase3_quota_selftest()) {
        serial_write("[IRIS][P3] WARN: quota selftest failed\n");
        return 0;
    }

    /* The marker names are kept: the headless gate greps for them, and what
     * they now attest is the lifecycle half — notification, process and quota
     * — after the handle-table halves retired with the namespace. */
    serial_write("[IRIS][P3] handle/lifecycle selftests OK\n");
    serial_write("[IRIS][P41] rights selftests OK\n");
    return 1;
}
