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

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_NOTIFICATION) {
        kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    knotification_signal((struct KNotification *)obj, arg1);
    kobject_release(obj);
    return syscall_ok_u64(IRIS_OK);
}


uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_NOTIFICATION) {
        kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WAIT)) {
        kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    uint64_t bits = 0;
    r = knotification_wait((struct KNotification *)obj, &bits);
    kobject_release(obj);
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


/* ── Synchronous channel call ─────────────────────────────────────── */

/*
 * sys_chan_call(req_chan, msg_uptr, reply_chan) → 0 or iris_error_t
 *
 * Requires RIGHT_WRITE on req_chan and RIGHT_READ on reply_chan.
 * Sends *msg_uptr on req_chan, then blocks on reply_chan until a reply
 * arrives; the reply overwrites *msg_uptr in place.
 *
 * The outbound request may NOT carry an attached handle (msg->attached_handle
 * is forced to HANDLE_INVALID before sending).  The inbound reply MAY carry
 * an attached handle, which is installed in the caller's handle table and
 * written into msg->attached_handle on return.
 *
 * req_chan and reply_chan are NOT consumed — both handles remain valid after
 * the call.  This is a convenience wrapper equivalent to:
 *   SYS_CHAN_SEND(req_chan, msg) + SYS_CHAN_RECV(reply_chan, msg)
 * but in a single syscall to minimize ring transitions.
 */
uint64_t sys_chan_call(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Validate the user message buffer — read+write; writable implies readable on x86-64 */
    if (!user_kchanmsg_writable(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Resolve req_chan — requires RIGHT_WRITE */
    struct KObject  *req_obj;
    iris_rights_t    req_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &req_obj, &req_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (req_obj->type != KOBJ_CHANNEL) {
        kobject_release(req_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(req_rights, RIGHT_WRITE)) {
        kobject_release(req_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Resolve reply_chan — requires RIGHT_READ */
    struct KObject  *rep_obj;
    iris_rights_t    rep_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg2, &rep_obj, &rep_rights);
    if (r != IRIS_OK) {
        kobject_release(req_obj);
        return syscall_err(r);
    }
    if (rep_obj->type != KOBJ_CHANNEL) {
        kobject_release(req_obj);
        kobject_release(rep_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rep_rights, RIGHT_READ)) {
        kobject_release(req_obj);
        kobject_release(rep_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Copy request from user, clear attached handle (no transfer on request path) */
    struct KChanMsg msg;
    if (!copy_kchanmsg_from_user(&msg, arg1)) {
        kobject_release(req_obj);
        kobject_release(rep_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    msg.sender_id = t->id; /* kernel stamps verified sender; ring-3 cannot forge */
    msg.attached_handle = HANDLE_INVALID;
    msg.attached_rights = RIGHT_NONE;

    /* Send the request — no handle transfer */
    r = kchannel_send((struct KChannel *)req_obj, &msg);
    kobject_release(req_obj);
    if (r != IRIS_OK) {
        kobject_release(rep_obj);
        return syscall_err(r);
    }

    /* Block until a reply arrives, installing any attached handle into our table */
    r = kchannel_recv_into_process((struct KChannel *)rep_obj, t->process, &msg);
    kobject_release(rep_obj);
    if (r != IRIS_OK) return syscall_err(r);

    /* Write the reply back to the user buffer in place */
    if (!copy_kchanmsg_to_user(arg1, &msg))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    return syscall_ok_u64(0);
}


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

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_NOTIFICATION) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_WAIT)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    uint64_t deadline_ticks = 0;
    if (!timeout_ns_to_deadline_ticks(arg2, &deadline_ticks)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }

    struct KNotification *notif = (struct KNotification *)obj;
    uint64_t bits = 0;
    r = knotification_wait_timeout(notif, &bits, deadline_ticks);
    kobject_release(obj);
    if (r == IRIS_OK) {
        if (!copy_u64_to_user_checked(arg1, bits))
            return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    return syscall_err(r);
}


/*
 * sys_wait_any(handles_uptr, count, out_index_uptr) → 0 or iris_error_t
 *
 * Blocks until at least one of the watched KChannels has a pending message,
 * then writes its 0-based index to *out_index_uptr and returns 0.
 * Does NOT consume the message — caller follows up with CHAN_RECV / CHAN_RECV_NB.
 */
uint64_t sys_wait_any(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint32_t count = (uint32_t)arg1;
    if (count == 0 || count > WAIT_ANY_MAX_CHANNELS)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg0, (uint32_t)(count * sizeof(handle_id_t))))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg2, (uint32_t)sizeof(uint32_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    handle_id_t handles[WAIT_ANY_MAX_CHANNELS];
    if (!copy_from_user_checked(handles, arg0, (uint32_t)(count * sizeof(handle_id_t))))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KChannel *chans[WAIT_ANY_MAX_CHANNELS];
    for (uint32_t i = 0; i < count; i++) chans[i] = 0;

    for (uint32_t i = 0; i < count; i++) {
        struct KObject *obj;
        iris_rights_t   rights;
        iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                                 handles[i], &obj, &rights);
        if (r != IRIS_OK) {
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(r);
        }
        if (obj->type != KOBJ_CHANNEL) {
            kobject_release(obj);
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_WRONG_TYPE);
        }
        if (!rights_check(rights, RIGHT_READ)) {
            kobject_release(obj);
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        chans[i] = (struct KChannel *)obj;
    }

    /* Phase 1: non-blocking scan */
    for (uint32_t i = 0; i < count; i++) {
        if (kchannel_is_readable(chans[i])) {
            uint32_t idx = i;
            for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
            if (!copy_u32_to_user_checked(arg2, idx))
                return syscall_err(IRIS_ERR_INVALID_ARG);
            return syscall_ok_u64(0);
        }
    }

    /* Phase 2: enqueue on all channels then yield, looping on spurious wakeups.
     *
     * t->state is set to TASK_BLOCKED_IPC inside kchannel_waiters_add_or_closed_atomic
     * while the channel lock is held.  This closes the preemption window that would
     * exist if state were set before the enqueue: IRQ0 calls task_yield() from its
     * handler, so a preemption between state=BLOCKED_IPC and the first enqueue would
     * leave the task permanently stalled with no waiter holding a reference to it.
     */
    for (;;) {
        for (uint32_t i = 0; i < count; i++) {
            iris_error_t r = kchannel_waiters_add_or_closed_atomic(chans[i], t);
            if (r == IRIS_ERR_CLOSED) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j <= i; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
            if (r != IRIS_OK) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j <= i; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(r);
            }
        }

        /* Re-scan after enqueueing: a message may have arrived during setup. */
        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_readable(chans[i])) {
                uint32_t idx = i;
                t->state = TASK_READY;
                for (uint32_t j = 0; j < count; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                if (!copy_u32_to_user_checked(arg2, idx))
                    return syscall_err(IRIS_ERR_INVALID_ARG);
                return syscall_ok_u64(0);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_closed(chans[i])) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j < count; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }
        task_yield();

        for (uint32_t i = 0; i < count; i++)
            kchannel_waiters_remove_task(chans[i], t);

        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_readable(chans[i])) {
                uint32_t idx = i;
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                if (!copy_u32_to_user_checked(arg2, idx))
                    return syscall_err(IRIS_ERR_INVALID_ARG);
                return syscall_ok_u64(0);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_closed(chans[i])) {
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }
        /* spurious wakeup — retry */
    }
}


/*
 * sys_wait_any_timeout(handles_uptr, count, out_index_uptr, timeout_ns)
 *
 * Like sys_wait_any but returns IRIS_ERR_TIMED_OUT if no channel becomes
 * readable within timeout_ns nanoseconds.  timeout_ns == 0 is a non-blocking
 * scan (returns IRIS_ERR_TIMED_OUT immediately if nothing is ready).
 */
uint64_t sys_wait_any_timeout(uint64_t arg0, uint64_t arg1,
                               uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint32_t count = (uint32_t)arg1;
    if (count == 0 || count > WAIT_ANY_MAX_CHANNELS)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg0, (uint32_t)(count * sizeof(handle_id_t))))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg2, (uint32_t)sizeof(uint32_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    handle_id_t handles[WAIT_ANY_MAX_CHANNELS];
    if (!copy_from_user_checked(handles, arg0, (uint32_t)(count * sizeof(handle_id_t))))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t deadline_ticks = 0;
    int has_deadline = (arg3 != 0);
    if (has_deadline && !timeout_ns_to_deadline_ticks(arg3, &deadline_ticks))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KChannel *chans[WAIT_ANY_MAX_CHANNELS];
    for (uint32_t i = 0; i < count; i++) chans[i] = 0;

    for (uint32_t i = 0; i < count; i++) {
        struct KObject *obj;
        iris_rights_t   rights;
        iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                                 handles[i], &obj, &rights);
        if (r != IRIS_OK) {
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(r);
        }
        if (obj->type != KOBJ_CHANNEL) {
            kobject_release(obj);
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_WRONG_TYPE);
        }
        if (!rights_check(rights, RIGHT_READ)) {
            kobject_release(obj);
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        chans[i] = (struct KChannel *)obj;
    }

    /* Non-blocking scan first */
    for (uint32_t i = 0; i < count; i++) {
        if (kchannel_is_readable(chans[i])) {
            uint32_t idx = i;
            for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
            if (!copy_u32_to_user_checked(arg2, idx))
                return syscall_err(IRIS_ERR_INVALID_ARG);
            return syscall_ok_u64(0);
        }
    }

    /* Zero timeout: nothing ready → timed out */
    if (!has_deadline) {
        for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
        return syscall_err(IRIS_ERR_TIMED_OUT);
    }

    for (;;) {
        for (uint32_t i = 0; i < count; i++) {
            iris_error_t r = kchannel_waiters_add_or_closed_atomic(chans[i], t);
            if (r == IRIS_ERR_CLOSED) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j <= i; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
            if (r != IRIS_OK) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j <= i; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(r);
            }
        }

        /* Re-scan after enqueueing */
        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_readable(chans[i])) {
                uint32_t idx = i;
                t->state = TASK_READY;
                for (uint32_t j = 0; j < count; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                if (!copy_u32_to_user_checked(arg2, idx))
                    return syscall_err(IRIS_ERR_INVALID_ARG);
                return syscall_ok_u64(0);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_closed(chans[i])) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j < count; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }

        t->wake_tick  = deadline_ticks;
        t->timed_out  = 0;
        task_yield();

        for (uint32_t i = 0; i < count; i++)
            kchannel_waiters_remove_task(chans[i], t);

        if (t->timed_out) {
            t->timed_out = 0;
            t->wake_tick = 0;
            for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_TIMED_OUT);
        }

        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_readable(chans[i])) {
                uint32_t idx = i;
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                if (!copy_u32_to_user_checked(arg2, idx))
                    return syscall_err(IRIS_ERR_INVALID_ARG);
                return syscall_ok_u64(0);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_closed(chans[i])) {
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }
        /* spurious wakeup — retry */
    }
}


/* ── Futex (D3) ──────────────────────────────────────────────────── */

uint64_t sys_futex_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    uint64_t uaddr    = arg0;
    uint32_t expected = (uint32_t)arg1;

    if (uaddr < USER_SPACE_BASE || uaddr >= USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (uaddr & 0x3ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);  /* must be 4-byte aligned */

    return syscall_err(futex_wait(uaddr, expected));
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
