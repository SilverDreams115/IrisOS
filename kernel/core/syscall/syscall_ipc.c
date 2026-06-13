#include "syscall_priv.h"



/* ── Capability IPC (KChannel + HandleTable) ─────────────────────── */

uint64_t sys_chan_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KChannel *ch = kchannel_alloc();
    if (!ch) return syscall_err(IRIS_ERR_NO_MEMORY);
    iris_error_t r = kchannel_bind_owner(ch, t->process);
    if (r != IRIS_OK) {
        kchannel_free(ch);
        return syscall_err(r);
    }
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &ch->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        kchannel_free(ch);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&ch->base); /* table now holds the reference */
    return (uint64_t)h;
}


uint64_t sys_chan_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_kchanmsg_readable(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KChanMsg msg;
    if (!copy_kchanmsg_from_user(&msg, arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    msg.sender_id = t->id; /* kernel stamps verified sender; ring-3 cannot forge */

    if (msg.attached_handle != HANDLE_INVALID) {
        struct KObject *xfer_obj;
        iris_rights_t   xfer_rights;
        iris_error_t xr = handle_table_get_object(&t->process->handle_table,
                                                  msg.attached_handle,
                                                  &xfer_obj, &xfer_rights);
        if (xr != IRIS_OK) { kobject_release(obj); return syscall_err(xr); }
        if (!rights_check(xfer_rights, RIGHT_TRANSFER)) {
            kobject_release(xfer_obj);
            kobject_release(obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }

        iris_rights_t moved_rights = rights_reduce(xfer_rights, msg.attached_rights);
        if (moved_rights == RIGHT_NONE) {
            kobject_release(xfer_obj);
            kobject_release(obj);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }

        kobject_active_retain(xfer_obj);
        r = kchannel_send_attached((struct KChannel *)obj, &msg, xfer_obj, moved_rights);
        if (r != IRIS_OK) {
            kobject_active_release(xfer_obj);
            kobject_release(xfer_obj);
            kobject_release(obj);
            return syscall_err(r);
        }

        r = handle_table_close(&t->process->handle_table, msg.attached_handle);
        if (r != IRIS_OK) { kobject_release(obj); return syscall_err(r); }
    } else {
        r = kchannel_send((struct KChannel *)obj, &msg);
    }
    kobject_release(obj);
    return syscall_err(r);
}


uint64_t sys_chan_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_kchanmsg_writable(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KChanMsg msg;
    r = kchannel_recv_into_process((struct KChannel *)obj, t->process, &msg);
    kobject_release(obj);
    /* Invariant: if msg carries an attached handle, it was already installed in
     * t->process's handle table by kchannel_recv_into_process.  If the write-back
     * below fails, the handle stays in the table but the caller never learns its ID.
     * This is only reachable if the user buffer became unwritable after our initial
     * check above — an exotic condition requiring concurrent SYS_VMO_UNMAP in the
     * same process. */
    if (r == IRIS_OK && !copy_kchanmsg_to_user(arg1, &msg))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}


/*
 * sys_chan_recv_nb — non-blocking channel receive.
 *
 * Identical to sys_chan_recv except it calls kchannel_try_recv_into_process
 * (no blocking).  Returns IRIS_ERR_WOULD_BLOCK immediately if the channel is
 * empty.  Useful for polling loops and supervisor-side drain passes.
 */
uint64_t sys_chan_recv_nb(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_kchanmsg_writable(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KChanMsg msg;
    r = kchannel_try_recv_into_process((struct KChannel *)obj, t->process, &msg);
    kobject_release(obj);
    if (r == IRIS_OK && !copy_kchanmsg_to_user(arg1, &msg))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}


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


/* ── Channel seal ─────────────────────────────────────────────────── */

/*
 * sys_chan_seal(chan_handle) → 0 or iris_error_t
 *
 * Requires RIGHT_WRITE on chan_handle.
 * Explicitly closes the channel (marks it sealed) and wakes all blocked
 * receivers, which return IRIS_ERR_CLOSED.  Future sends also return
 * IRIS_ERR_CLOSED.  Already-buffered messages can still be drained.
 *
 * The handle itself is NOT consumed; close it with SYS_HANDLE_CLOSE afterwards.
 * Idempotent: sealing an already-sealed channel returns 0.
 *
 * Primary use: svcmgr poisons old service channels before a restart so stale
 * client handles fail fast instead of silently queuing to a dead endpoint.
 */
uint64_t sys_chan_seal(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    kchannel_seal((struct KChannel *)obj);
    kobject_release(obj);
    return syscall_ok_u64(0);
}


/* Fase 13 (Track G): sys_chan_call retired — zero callers; syscall
 * number 38 falls through to the dispatch default and stays reserved. */


/*
 * sys_chan_recv_timeout(chan_h, msg_uptr, timeout_ns) → 0 or iris_error_t
 *
 * Identical to SYS_CHAN_RECV but returns IRIS_ERR_TIMED_OUT (-15) if no
 * message arrives within timeout_ns nanoseconds.  Resolution: 10 ms (one
 * scheduler tick at 100 Hz).
 */
uint64_t sys_chan_recv_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_kchanmsg_writable(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    /* Convert timeout_ns to an absolute tick deadline.  Round up by +1 tick so
     * a caller requesting 10 ms gets at least one full tick of wait time. */
    uint64_t deadline_ticks = 0;
    if (!timeout_ns_to_deadline_ticks(arg2, &deadline_ticks)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }

    struct KChanMsg msg;
    r = kchannel_recv_timeout_into_process((struct KChannel *)obj, t->process, &msg, deadline_ticks);
    kobject_release(obj);
    if (r == IRIS_OK && !copy_kchanmsg_to_user(arg1, &msg))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}


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
