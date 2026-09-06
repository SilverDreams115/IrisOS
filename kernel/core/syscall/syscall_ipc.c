#include "syscall_priv.h"



/* Phase 13/Track G: sys_chan_create/send/recv/recv_nb retired — KChannel is
 * no longer a productive IPC mechanism; the syscall numbers return
 * IRIS_ERR_NOT_SUPPORTED via the dispatch default. */


/* ── Notification syscalls ────────────────────────────────────────── */

/*
 * Phase S1: SYS_NOTIFY_CREATE (19) is RETIRED — it fabricated a KNotification
 * from kslab, charged a per-process quota and returned a handle: three
 * non-seL4 mechanisms in one path.  Notifications are now created ONLY via
 * SYS_UNTYPED_RETYPE2 (storage inside the source Untyped, capability directly
 * in CSpace).  The syscall number stays reserved and fails without touching
 * any state: no kslab, no quota, no handle, no object.
 */
uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_resolve_only_notification(t->cspace_root, (iris_cptr_t)arg0,
                                                            RIGHT_WRITE, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);
    knotification_signal(notif, arg1);
    kobject_release(&notif->base);
    return syscall_ok_u64(IRIS_OK);
}


uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    /*
     * Woken because the notification CLOSED under us.  Checked before the
     * capability is resolved, because the close is usually the last capability
     * going away — the slot is empty now, and re-resolving would report
     * NOT_FOUND for something that actually closed.  Preserves the contract
     * the parked form had, which could tell the two apart only because it
     * never let go of the object.
     */
    if (t->sc_reentry && t->ipc_ep_closed) {
        t->ipc_ep_closed = 0u;
        return syscall_err(IRIS_ERR_CLOSED);
    }

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_resolve_only_notification(t->cspace_root, (iris_cptr_t)arg0,
                                                            RIGHT_WAIT, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);

    /*
     * Stage 9-evt Step 1 — RESTARTABLE (ledger D-1).
     *
     * One non-blocking attempt; if it would block, the thread is enqueued on
     * the notification and parked, and the DISPATCHER re-executes this syscall
     * when it runs again.  The continuation is the waiter registration, which
     * lives on the notification and the thread — not on a kernel stack, which
     * is the whole point.
     *
     * The capability is re-resolved on every entry rather than held across the
     * block.  That is not overhead, it is the correct semantics: a capability
     * deleted while the thread was parked should not still be waited on, and
     * the parked form could not notice.
     */
    uint64_t bits = 0;
    r = knotification_wait_step(notif, &bits);
    kobject_release(&notif->base);

    if (r == IRIS_ERR_WOULD_BLOCK) {
        syscall_request_restart(t);
        return 0;   /* unused: the dispatcher re-enters instead of returning */
    }
    if (r == IRIS_OK && !copy_u64_to_user_checked(arg1, bits))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}


/* Phase 13/Track G: sys_chan_seal / sys_chan_recv_timeout retired (KChannel
 * fully retired); the syscall numbers fall to the dispatch default. */


/*
 * sys_notify_wait_timeout(notify_h, bits_uptr, timeout_ns) → 0 or iris_error_t
 *
 * Identical to SYS_NOTIFY_WAIT but returns IRIS_ERR_TIMED_OUT (-15) if no
 * signal arrives within timeout_ns nanoseconds.
 */
uint64_t sys_notify_wait_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    if (t->sc_reentry && t->ipc_ep_closed) {
        t->ipc_ep_closed = 0u;
        t->wake_tick = 0u; t->timed_out = 0u;
        return syscall_err(IRIS_ERR_CLOSED);
    }

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_resolve_only_notification(t->cspace_root, (iris_cptr_t)arg0,
                                                            RIGHT_WAIT, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);

    uint64_t deadline_ticks = 0;
    if (!timeout_ns_to_deadline_ticks(arg2, &deadline_ticks)) {
        kobject_release(&notif->base);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }

    /* Stage 9-evt Step 1 — RESTARTABLE, same shape as SYS_NOTIFY_WAIT with a
     * deadline armed on the first attempt only. */
    uint64_t bits = 0;
    r = knotification_wait_timeout_step(notif, &bits, deadline_ticks,
                                        /*first=*/!t->sc_reentry);
    kobject_release(&notif->base);

    if (r == IRIS_ERR_WOULD_BLOCK) {
        syscall_request_restart(t);
        return 0;
    }
    if (r == IRIS_OK) {
        if (!copy_u64_to_user_checked(arg1, bits))
            return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    return syscall_err(r);
}


