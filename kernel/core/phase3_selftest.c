#include <iris/phase3_selftest.h>
#include <iris/serial.h>
#include <iris/task.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <iris/nc/handle.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kvmo.h>
#include <iris/paging.h>
#include <stdatomic.h>
#include <stdint.h>

static HandleTable phase3_ht;
static HandleTable phase3_dest_ht;
static HandleTable phase3_full_ht;
static HandleTable phase41_ht;
static handle_id_t phase3_fill_ids[HANDLE_TABLE_MAX];

/* Fase 13/Track G: the channel-quota portion is retired with the KChannel
 * object; this selftest now covers the notification and VMO quotas. */
static int phase3_quota_selftest(void) {
    struct KProcess *proc = 0;
    struct KNotification *notifs[KPROCESS_NOTIFICATION_QUOTA + 1];
    struct KVmo *vmos[KPROCESS_VMO_QUOTA + 1];
    int ok = 0;

    for (uint32_t i = 0; i < KPROCESS_NOTIFICATION_QUOTA + 1; i++) notifs[i] = 0;
    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA + 1; i++) vmos[i] = 0;

    proc = kprocess_alloc();
    if (!proc) goto out;

    for (uint32_t i = 0; i < KPROCESS_NOTIFICATION_QUOTA; i++) {
        notifs[i] = knotification_alloc();
        if (!notifs[i]) goto out;
        if (knotification_bind_owner(notifs[i], proc) != IRIS_OK) goto out;
    }
    notifs[KPROCESS_NOTIFICATION_QUOTA] = knotification_alloc();
    if (!notifs[KPROCESS_NOTIFICATION_QUOTA]) goto out;
    if (knotification_bind_owner(notifs[KPROCESS_NOTIFICATION_QUOTA], proc) != IRIS_ERR_NO_MEMORY) goto out;

    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA; i++) {
        vmos[i] = kvmo_create(0x1000ULL);
        if (!vmos[i]) goto out;
        if (kvmo_bind_owner(vmos[i], proc) != IRIS_OK) goto out;
    }
    vmos[KPROCESS_VMO_QUOTA] = kvmo_create(0x1000ULL);
    if (!vmos[KPROCESS_VMO_QUOTA]) goto out;
    if (kvmo_bind_owner(vmos[KPROCESS_VMO_QUOTA], proc) != IRIS_ERR_NO_MEMORY) goto out;

    if (proc->owned_notifications != KPROCESS_NOTIFICATION_QUOTA) goto out;
    if (proc->owned_vmos != KPROCESS_VMO_QUOTA) goto out;

    ok = 1;

out:
    for (uint32_t i = 0; i < KPROCESS_NOTIFICATION_QUOTA + 1; i++) {
        if (notifs[i]) knotification_free(notifs[i]);
    }
    for (uint32_t i = 0; i < KPROCESS_VMO_QUOTA + 1; i++) {
        if (vmos[i]) kvmo_free(vmos[i]);
    }
    if (proc) kprocess_free(proc);
    return ok;
}

static int phase3_process_selftest(void) {
    struct KProcess *proc = 0;
    struct KNotification *xnotif = 0;
    struct KVmo *vmo = 0;
    struct KVmo *large_vmo = 0;
    uint64_t ref_before;
    uint64_t active_before;
    int ok = 0;

    proc = kprocess_alloc();
    xnotif = knotification_alloc();
    vmo = kvmo_create(0x1000ULL);
    large_vmo = kvmo_create(0x200000ULL);
    if (!proc || !xnotif || !vmo || !large_vmo) goto out;
    if (large_vmo->page_capacity < 512u) goto out;

    /* Exception handler notification: set twice — idempotent (Track I). */
    ref_before = atomic_load_explicit(&xnotif->base.refcount, memory_order_relaxed);
    active_before = atomic_load_explicit(&xnotif->base.active_refs, memory_order_relaxed);
    if (kprocess_set_exception_handler(proc, xnotif, 1u) != IRIS_OK) goto out;
    if (kprocess_set_exception_handler(proc, xnotif, 1u) != IRIS_OK) goto out;
    if (proc->exception_notif != xnotif) goto out;
    if (atomic_load_explicit(&xnotif->base.refcount, memory_order_relaxed) != ref_before + 1u) goto out;
    if (atomic_load_explicit(&xnotif->base.active_refs, memory_order_relaxed) != active_before + 1u) goto out;

    proc->cr3 = paging_create_user_space();
    if (!proc->cr3) goto out;

    /* Verify VMOs have no physical pages allocated (no demand paging). */
    if (vmo->pages[0] != 0) goto out;
    if (large_vmo->pages[511] != 0) goto out;

    /* Teardown must clear exception notification and restore refcounts.
     * KVSpace.mappings (dynamic linked list) is cleaned up by
     * kvspace_invalidate called from kprocess_teardown. */
    kprocess_teardown(proc, 0);
    if (proc->exception_notif != 0) goto out;
    if (atomic_load_explicit(&xnotif->base.refcount, memory_order_relaxed) != ref_before) goto out;
    if (atomic_load_explicit(&xnotif->base.active_refs, memory_order_relaxed) != active_before) goto out;

    ok = 1;
out:
    if (proc) {
        kprocess_teardown(proc, 0);
        kprocess_reap_address_space(proc);
        kprocess_free(proc);
    }
    if (xnotif) knotification_free(xnotif);
    if (vmo) kvmo_free(vmo);
    if (large_vmo) kvmo_free(large_vmo);
    return ok;
}

static int phase3_handle_selftest(void) {
    struct KObject *obj = 0;
    struct KObject *obj2 = 0;
    iris_rights_t rights = RIGHT_NONE;
    handle_id_t h = HANDLE_INVALID;
    handle_id_t h_src = HANDLE_INVALID;
    handle_id_t h_dup = HANDLE_INVALID;
    handle_id_t h_dest = HANDLE_INVALID;
    struct KNotification *notif = 0;
    struct KNotification *fill_notif = 0;
    struct KBootstrapCap *cap = 0;
    struct KBootstrapCap *cap_clone = 0;
    handle_id_t h_cap_src = HANDLE_INVALID;
    handle_id_t h_cap_alias = HANDLE_INVALID;
    int ok = 0;

    handle_table_init(&phase3_ht);
    handle_table_init(&phase3_dest_ht);
    handle_table_init(&phase3_full_ht);

    notif = knotification_alloc();
    if (!notif) goto out;

    h = handle_table_insert(&phase3_ht, &notif->base,
                            RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                            RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID || handle_id_gen(h) == 0u) goto out;
    if (handle_table_get_object(&phase3_ht, 1u, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;

    if (handle_table_close(&phase3_ht, h) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_ht, h, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;

    h_src = handle_table_insert(&phase3_ht, &notif->base,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                                RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h_src == HANDLE_INVALID) goto out;
    if (handle_table_get_object(&phase3_ht, h_src, &obj, &rights) != IRIS_OK) goto out;
    h_dup = handle_table_insert(&phase3_ht, obj, rights_reduce(rights, RIGHT_READ));
    kobject_release(obj);
    obj = 0;
    if (h_dup == HANDLE_INVALID) goto out;
    if (handle_table_get_object(&phase3_ht, h_dup, &obj, &rights) != IRIS_OK) goto out;
    if (rights != RIGHT_READ) goto out;
    kobject_release(obj);
    obj = 0;

    if (handle_table_get_object(&phase3_ht, h_src, &obj, &rights) != IRIS_OK) goto out;
    h_dest = handle_table_insert(&phase3_dest_ht, obj, rights_reduce(rights, RIGHT_WAIT));
    kobject_release(obj);
    obj = 0;
    if (h_dest == HANDLE_INVALID) goto out;
    if (handle_table_close(&phase3_ht, h_src) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_ht, h_src, &obj, &rights) != IRIS_ERR_BAD_HANDLE) goto out;
    if (handle_table_get_object(&phase3_dest_ht, h_dest, &obj, &rights) != IRIS_OK) goto out;
    if (rights != RIGHT_WAIT) goto out;
    kobject_release(obj);
    obj = 0;

    cap = kbootcap_alloc(IRIS_BOOTCAP_SPAWN_SERVICE |
                         IRIS_BOOTCAP_HW_ACCESS |
                         IRIS_BOOTCAP_KDEBUG);
    if (!cap) goto out;
    h_cap_src = handle_table_insert(&phase3_ht, &cap->base, RIGHT_READ);
    if (h_cap_src == HANDLE_INVALID) goto out;
    h_cap_alias = handle_table_insert(&phase3_dest_ht, &cap->base, RIGHT_READ);
    if (h_cap_alias == HANDLE_INVALID) goto out;

    cap_clone = kbootcap_clone_restricted(cap, IRIS_BOOTCAP_SPAWN_SERVICE);
    if (!cap_clone) goto out;
    if (handle_table_replace(&phase3_dest_ht, h_cap_alias, &cap_clone->base) != IRIS_OK) goto out;
    kobject_release(&cap_clone->base);
    cap_clone = 0;

    if (handle_table_get_object(&phase3_ht, h_cap_src, &obj, &rights) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase3_dest_ht, h_cap_alias, &obj2, &rights) != IRIS_OK) goto out;
    if (obj == obj2) goto out;
    if (!kbootcap_allows((struct KBootstrapCap *)obj,
                         IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_HW_ACCESS | IRIS_BOOTCAP_KDEBUG)) goto out;
    if (!kbootcap_allows((struct KBootstrapCap *)obj2, IRIS_BOOTCAP_SPAWN_SERVICE)) goto out;
    if (kbootcap_allows((struct KBootstrapCap *)obj2, IRIS_BOOTCAP_HW_ACCESS)) goto out;
    if (kbootcap_allows((struct KBootstrapCap *)obj2, IRIS_BOOTCAP_KDEBUG)) goto out;
    kobject_release(obj);
    kobject_release(obj2);
    obj = 0;
    obj2 = 0;

    fill_notif = knotification_alloc();
    if (!fill_notif) goto out;
    for (uint32_t i = 0; i < HANDLE_TABLE_MAX; i++) {
        phase3_fill_ids[i] = handle_table_insert(&phase3_full_ht, &fill_notif->base, RIGHT_READ);
        if (phase3_fill_ids[i] == HANDLE_INVALID) goto out;
    }
    if (handle_table_insert(&phase3_full_ht, &fill_notif->base, RIGHT_READ) != HANDLE_INVALID) goto out;
    for (uint32_t i = 0; i < HANDLE_TABLE_MAX; i++) {
        if (handle_table_close(&phase3_full_ht, phase3_fill_ids[i]) != IRIS_OK) goto out;
    }
    if (atomic_load_explicit(&fill_notif->base.active_refs, memory_order_relaxed) != 0u) goto out;
    h = handle_table_insert(&phase3_full_ht, &fill_notif->base, RIGHT_READ);
    if (h == HANDLE_INVALID) goto out;
    if (handle_table_close(&phase3_full_ht, h) != IRIS_OK) goto out;

    ok = 1;

out:
    if (obj) kobject_release(obj);
    if (obj2) kobject_release(obj2);
    handle_table_close_all(&phase3_ht);
    handle_table_close_all(&phase3_dest_ht);
    handle_table_close_all(&phase3_full_ht);
    if (notif) knotification_free(notif);
    if (fill_notif) knotification_free(fill_notif);
    if (cap) kbootcap_free(cap);
    if (cap_clone) kbootcap_free(cap_clone);
    return ok;
}

/* phase3_channel_selftest retired — Fase 13/Track G (KChannel object removed). */

static int phase3_notification_selftest(void) {
    struct KNotification *n = knotification_alloc();
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
static int phase41_rights_selftest(void) {
    struct KNotification *n = 0;
    struct KObject *obj = 0;
    iris_rights_t stored = RIGHT_NONE;
    handle_id_t h_full = HANDLE_INVALID;
    handle_id_t h_reduced = HANDLE_INVALID;
    int ok = 0;

    handle_table_init(&phase41_ht);
    n = knotification_alloc();
    if (!n) goto out;

    /* 1. rights_reduce invariants */
    if (rights_reduce(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE,
                      RIGHT_SAME_RIGHTS) !=
        (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) goto out;

    if (rights_reduce(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE,
                      RIGHT_READ) != RIGHT_READ) goto out;

    /* superset request is capped — no elevation */
    if (rights_reduce(RIGHT_READ, RIGHT_READ | RIGHT_WRITE) != RIGHT_READ) goto out;

    if (rights_reduce(RIGHT_READ | RIGHT_WRITE, RIGHT_NONE) != RIGHT_NONE) goto out;

    /* RIGHT_NONE base stays empty regardless of request */
    if (rights_reduce(RIGHT_NONE, RIGHT_READ | RIGHT_WRITE) != RIGHT_NONE) goto out;

    /* 2. rights_check edge cases */
    if (rights_check(RIGHT_READ, RIGHT_READ | RIGHT_WRITE)) goto out;  /* partial miss */
    if (!rights_check(RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE)) goto out; /* exact */
    if (!rights_check(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE, RIGHT_READ)) goto out; /* superset */
    if (rights_check(RIGHT_NONE, RIGHT_READ)) goto out;
    if (rights_check(RIGHT_NONE, RIGHT_NONE)) goto out; /* RIGHT_NONE never satisfies */

    /* 3. Handle stores exactly the inserted rights — no inflation */
    h_full = handle_table_insert(&phase41_ht, &n->base,
                                 RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (h_full == HANDLE_INVALID) goto out;
    if (handle_table_get_object(&phase41_ht, h_full, &obj, &stored) != IRIS_OK) goto out;
    if (stored != (RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE)) goto out;
    kobject_release(obj); obj = 0;

    /* 4. Reduced handle has only the bits it was given — no bleed from full handle */
    h_reduced = handle_table_insert(&phase41_ht, &n->base,
                                    rights_reduce(RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE,
                                                  RIGHT_READ));
    if (h_reduced == HANDLE_INVALID) goto out;
    if (handle_table_get_object(&phase41_ht, h_reduced, &obj, &stored) != IRIS_OK) goto out;
    if (stored != RIGHT_READ) goto out;
    if (rights_check(stored, RIGHT_WRITE)) goto out;      /* no elevation */
    if (rights_check(stored, RIGHT_DUPLICATE)) goto out;  /* no elevation */
    kobject_release(obj); obj = 0;

    /* 5. Stale handle rejected after close */
    if (handle_table_close(&phase41_ht, h_full) != IRIS_OK) goto out;
    if (handle_table_get_object(&phase41_ht, h_full, &obj, &stored) != IRIS_ERR_BAD_HANDLE) goto out;
    /* Reduced handle still live; full handle (same or different slot) is stale */
    if (handle_table_get_object(&phase41_ht, h_reduced, &obj, &stored) != IRIS_OK) goto out;
    if (stored != RIGHT_READ) goto out;
    kobject_release(obj); obj = 0;

    ok = 1;
out:
    if (obj) kobject_release(obj);
    handle_table_close_all(&phase41_ht);
    if (n) knotification_free(n);
    return ok;
}

int phase3_selftest_run(void) {
    if (!phase3_handle_selftest()) {
        serial_write("[IRIS][P3] WARN: handle selftest failed\n");
        return 0;
    }
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
    if (!phase41_rights_selftest()) {
        serial_write("[IRIS][P41] WARN: rights selftests failed\n");
        return 0;
    }

    serial_write("[IRIS][P3] handle/lifecycle selftests OK\n");
    serial_write("[IRIS][P41] rights selftests OK\n");
    return 1;
}
