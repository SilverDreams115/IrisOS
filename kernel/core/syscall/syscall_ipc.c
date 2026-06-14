#include "syscall_priv.h"



/* Fase 13/Track G: sys_chan_create/send/recv/recv_nb retired — KChannel is
 * no longer a productive IPC mechanism; the syscall numbers return
 * IRIS_ERR_NOT_SUPPORTED via the dispatch default. */


/* ── Notification syscalls ────────────────────────────────────────── */

uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KNotification *n = knotification_alloc();
    if (!n) return syscall_err(IRIS_ERR_NO_MEMORY);
    iris_error_t r = knotification_bind_owner(n, t->process);
    if (r != IRIS_OK) {
        knotification_free(n);
        return syscall_err(r);
    }
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &n->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                                        RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        knotification_free(n);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&n->base); /* table now holds the reference */
    return (uint64_t)h;
}


uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_or_handle_resolve_notification(t->process, (iris_cptr_t)arg0,
                                                            RIGHT_WRITE, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);
    knotification_signal(notif, arg1);
    kobject_release(&notif->base);
    return syscall_ok_u64(IRIS_OK);
}


uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_or_handle_resolve_notification(t->process, (iris_cptr_t)arg0,
                                                            RIGHT_WAIT, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);
    uint64_t bits = 0;
    r = knotification_wait(notif, &bits);
    kobject_release(&notif->base);
    if (r == IRIS_OK && !copy_u64_to_user_checked(arg1, bits))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}


/* Fase 13/Track G: sys_chan_seal / sys_chan_recv_timeout retired (KChannel
 * fully retired); the syscall numbers fall to the dispatch default. */


/*
 * sys_notify_wait_timeout(notify_h, bits_uptr, timeout_ns) → 0 or iris_error_t
 *
 * Identical to SYS_NOTIFY_WAIT but returns IRIS_ERR_TIMED_OUT (-15) if no
 * signal arrives within timeout_ns nanoseconds.
 */
uint64_t sys_notify_wait_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KNotification *notif; iris_rights_t notif_r;
    iris_error_t r = cspace_or_handle_resolve_notification(t->process, (iris_cptr_t)arg0,
                                                            RIGHT_WAIT, &notif, &notif_r);
    if (r != IRIS_OK) return syscall_err(r);

    uint64_t deadline_ticks = 0;
    if (!timeout_ns_to_deadline_ticks(arg2, &deadline_ticks)) {
        kobject_release(&notif->base);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }

    uint64_t bits = 0;
    r = knotification_wait_timeout(notif, &bits, deadline_ticks);
    kobject_release(&notif->base);
    if (r == IRIS_OK) {
        if (!copy_u64_to_user_checked(arg1, bits))
            return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    return syscall_err(r);
}


/* Fase 13 (Track G): sys_wait_any / sys_wait_any_timeout retired —
 * zero callers; the syscall numbers (44 / 72) fall through to the
 * dispatch default (IRIS_ERR_NOT_SUPPORTED) and stay reserved. */


/* ── Futex (D3) ──────────────────────────────────────────────────── */

uint64_t sys_futex_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t uaddr    = arg0;
    uint32_t expected = (uint32_t)arg1;

    if (uaddr < USER_SPACE_BASE || uaddr >= USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (uaddr & 0x3ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);  /* must be 4-byte aligned */

    uint64_t deadline_ticks = 0;
    if (arg2 != 0 && !timeout_ns_to_deadline_ticks(arg2, &deadline_ticks))
        return syscall_err(IRIS_ERR_OVERFLOW);

    return syscall_err(futex_wait(uaddr, expected, deadline_ticks));
}


uint64_t sys_futex_wake(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    uint64_t uaddr = arg0;
    uint32_t count = (uint32_t)arg1;

    if (uaddr & 0x3ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (count == 0) return syscall_ok_u64(0);

    return syscall_ok_u64((uint64_t)futex_wake(uaddr, count));
}
