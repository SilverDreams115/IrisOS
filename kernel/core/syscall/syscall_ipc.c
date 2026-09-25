/* SPDX-License-Identifier: Apache-2.0 */
#include "syscall_priv.h"



/* Phase 13/Track G: sys_chan_create/send/recv/recv_nb retired — KChannel is
 * no longer a productive IPC mechanism; the syscall numbers return
 * IRIS_ERR_NOT_SUPPORTED via the dispatch default. */


/* ── Notification syscalls ────────────────────────────────────────── */

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


/*
 * SYS_NOTIFY_POLL(notif_cptr, out_bits) — ledger A-24, seL4's `seL4_Poll`.
 *
 * Take whatever is pending and return; never block.  It exists because
 * SYS_NOTIFY_WAIT_TIMEOUT is retired: a caller that used a zero timeout to ask
 * "is anything there" had that question answered by the kernel's timed-block
 * machinery, and the question is legitimate even though the machinery was not.
 *
 * IRIS_ERR_WOULD_BLOCK when nothing is pending — the same answer, in the same
 * words, that SYS_EP_NB_RECV gives for the same situation.
 */
uint64_t sys_notify_poll(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_resolve_only_notification(t->cspace_root,
                            (iris_cptr_t)arg0, RIGHT_WAIT, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);

    uint64_t bits = knotification_take_pending(notif);
    kobject_release(&notif->base);
    if (bits == 0u) return syscall_err(IRIS_ERR_WOULD_BLOCK);
    if (!copy_to_user_checked(arg1, &bits, (uint32_t)sizeof(bits)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
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




