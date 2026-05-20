#include "syscall_priv.h"
#include <iris/nc/kendpoint.h>
#include <iris/ipc_msg.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

static inline void copy_irismsg(struct IrisMsg *dst, const struct IrisMsg *src) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < (uint32_t)sizeof(struct IrisMsg); i++)
        d[i] = s[i];
}

static inline void copy_kbuf(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

/*
 * ep_get — look up endpoint handle, validate type and required rights.
 * Returns the KEndpoint* with the kobject lifecycle ref bumped by get_object.
 * Caller must kobject_release() when done.
 */
static struct KEndpoint *ep_get(struct task *t, handle_id_t h,
                                iris_rights_t need, iris_error_t *out_err) {
    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, h, &obj, &rights);
    if (r != IRIS_OK) { *out_err = r; return 0; }
    if (obj->type != KOBJ_ENDPOINT) {
        kobject_release(obj);
        *out_err = IRIS_ERR_WRONG_TYPE;
        return 0;
    }
    if (!rights_check(rights, need)) {
        kobject_release(obj);
        *out_err = IRIS_ERR_ACCESS_DENIED;
        return 0;
    }
    return (struct KEndpoint *)obj;
}

/*
 * stage_send_cap — validate and detach an attached cap from the sender's
 * handle table, storing the kobject + rights for later delivery to a receiver.
 * On success: sender's handle is closed; *out_obj holds the ownership ref.
 * On failure: *out_obj = NULL; caller's handle is untouched; returns error.
 */
static iris_error_t stage_send_cap(struct task *t, uint32_t src_h,
                                   uint32_t requested_rights,
                                   struct KObject **out_obj,
                                   uint32_t *out_rights) {
    struct KObject *xo;
    iris_rights_t   xr;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)src_h, &xo, &xr);
    if (r != IRIS_OK) return r;
    if (!rights_check(xr, RIGHT_TRANSFER)) { kobject_release(xo); return IRIS_ERR_ACCESS_DENIED; }

    iris_rights_t cap_rights = rights_reduce(xr, (iris_rights_t)requested_rights);
    if (cap_rights == RIGHT_NONE) { kobject_release(xo); return IRIS_ERR_INVALID_ARG; }

    /* Close sender's handle; the get_object lifecycle ref becomes the staging ref. */
    handle_table_close(&t->process->handle_table, (handle_id_t)src_h);
    *out_obj    = xo;
    *out_rights = (uint32_t)cap_rights;
    return IRIS_OK;
}

/*
 * deliver_cap — install a staged cap into the receiver's handle table.
 * Releases the staging ref regardless of success.
 * Returns HANDLE_INVALID on failure (table full); caller should treat it as
 * a soft error and deliver the message without the capability.
 */
static uint32_t deliver_cap(struct task *receiver,
                             struct KObject *xo, uint32_t cap_rights) {
    if (!xo) return IRIS_MSG_NO_CAP;
    handle_id_t new_h = handle_table_insert(&receiver->process->handle_table,
                                            xo, (iris_rights_t)cap_rights);
    kobject_release(xo); /* release staging ref; table holds its own ref */
    return (uint32_t)new_h;
}

/* ── SYS_ENDPOINT_CREATE ─────────────────────────────────────────────── */

uint64_t sys_endpoint_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KEndpoint *ep = kendpoint_alloc();
    if (!ep) return syscall_err(IRIS_ERR_NO_MEMORY);

    handle_id_t h = handle_table_insert(&t->process->handle_table, &ep->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    kobject_release(&ep->base);
    if (h == HANDLE_INVALID)
        return syscall_err(IRIS_ERR_TABLE_FULL);

    return (uint64_t)h;
}

/* ── SYS_EP_SEND ─────────────────────────────────────────────────────── */

uint64_t sys_ep_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    iris_error_t err;
    struct KEndpoint *ep = ep_get(t, (handle_id_t)arg0, RIGHT_WRITE, &err);
    if (!ep) return syscall_err(err);

    /* Copy sender's message. */
    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Ph69: stage bulk payload in kernel buffer. */
    t->ipc_kbuf_len = 0;
    if (t->ipc_msg.buf_len > 0 && t->ipc_msg.buf_uptr != 0) {
        uint32_t n = t->ipc_msg.buf_len;
        if (n > IRIS_IPC_BUF_SIZE) n = IRIS_IPC_BUF_SIZE;
        if (!user_range_readable(t->ipc_msg.buf_uptr, n) ||
            !copy_from_user_checked(t->ipc_kbuf, t->ipc_msg.buf_uptr, n)) {
            kobject_release(&ep->base);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
        t->ipc_kbuf_len  = n;
        t->ipc_msg.buf_len = n;
    }

    /* Ph68: validate and stage attached cap before taking the spinlock. */
    struct KObject *xfer_obj    = 0;
    uint32_t        xfer_rights = 0;
    if (t->ipc_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = stage_send_cap(t, t->ipc_msg.attached_handle,
                                         t->ipc_msg.attached_rights,
                                         &xfer_obj, &xfer_rights);
        if (cr != IRIS_OK) {
            kobject_release(&ep->base);
            return syscall_err(cr);
        }
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* cleared in staged msg */
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) kobject_release(xfer_obj);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_RECV) {
        /* Rendezvous: a receiver is already waiting. */
        struct task *receiver = ep->queue_head;
        ep->queue_head = receiver->ep_next;
        if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
        receiver->ep_next     = 0;
        receiver->blocking_ep = 0;

        copy_irismsg(&receiver->ipc_msg, &t->ipc_msg);
        receiver->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* will update after unlock */
        receiver->ipc_msg_ready           = 1;

        /* Ph69: copy kbuf to receiver's staging. */
        if (t->ipc_kbuf_len > 0) {
            copy_kbuf(receiver->ipc_kbuf, t->ipc_kbuf, t->ipc_kbuf_len);
            receiver->ipc_kbuf_len = t->ipc_kbuf_len;
        } else {
            receiver->ipc_kbuf_len = 0;
        }

        irq_spinlock_unlock(&ep->lock, flags);

        /* Ph68: install cap in receiver's handle table (outside lock). */
        if (xfer_obj) {
            uint32_t new_h = deliver_cap(receiver, xfer_obj, xfer_rights);
            receiver->ipc_msg.attached_handle = new_h;
        }

        /* Wake receiver only after all data is consistent. */
        receiver->state = TASK_READY;
        kobject_release(&ep->base);
        return syscall_ok_u64(0);
    }

    /* No receiver: stage cap in task and block. */
    ep->ep_state     = EP_STATE_SEND;
    t->ep_next       = 0;
    t->blocking_ep   = ep;
    t->ipc_msg_ready = 0;
    t->ipc_ep_closed = 0;
    t->ep_cap_obj    = xfer_obj;    /* staging ref; released by receiver or cancel */
    t->ep_cap_rights = xfer_rights;

    if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
    else                { ep->queue_head = t; ep->queue_tail = t; }

    t->state = TASK_BLOCKED_SEND;
    irq_spinlock_unlock(&ep->lock, flags);

    task_yield();

    kobject_release(&ep->base);

    if (t->ipc_ep_closed) { t->ipc_ep_closed = 0; return syscall_err(IRIS_ERR_CLOSED); }
    return syscall_ok_u64(0);
}

/* ── SYS_EP_RECV ─────────────────────────────────────────────────────── */

uint64_t sys_ep_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    iris_error_t err;
    struct KEndpoint *ep = ep_get(t, (handle_id_t)arg0, RIGHT_READ, &err);
    if (!ep) return syscall_err(err);

    /* Ph69: read receiver's hints (buf_uptr = where to put bulk data). */
    t->ep_recv_buf_uptr = 0;
    t->ipc_kbuf_len     = 0;
    {
        struct IrisMsg hints;
        if (user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)) &&
            copy_from_user_checked(&hints, arg1, (uint32_t)sizeof(struct IrisMsg)))
            t->ep_recv_buf_uptr = hints.buf_uptr;
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_SEND) {
        /* Rendezvous: a sender is already waiting. */
        struct task *sender = ep->queue_head;
        ep->queue_head = sender->ep_next;
        if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
        sender->ep_next     = 0;
        sender->blocking_ep = 0;

        copy_irismsg(&t->ipc_msg, &sender->ipc_msg);
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP; /* will update after unlock */

        /* Ph69: receiver is in their own CR3 — copy kbuf directly to user space. */
        uint64_t recv_buf = t->ep_recv_buf_uptr;
        uint32_t kbuf_n   = sender->ipc_kbuf_len;
        if (kbuf_n > 0 && recv_buf != 0 &&
            user_range_writable(recv_buf, kbuf_n) &&
            copy_to_user_checked(recv_buf, sender->ipc_kbuf, kbuf_n)) {
            t->ipc_msg.buf_len  = kbuf_n;
            t->ipc_msg.buf_uptr = recv_buf;
        } else {
            t->ipc_msg.buf_len  = kbuf_n; /* report available bytes even if not written */
            t->ipc_msg.buf_uptr = 0;
        }
        sender->ipc_kbuf_len = 0;

        /* Ph68: take sender's staged cap. */
        struct KObject *xfer_obj    = sender->ep_cap_obj;
        uint32_t        xfer_rights = sender->ep_cap_rights;
        sender->ep_cap_obj    = 0;
        sender->ep_cap_rights = 0;

        irq_spinlock_unlock(&ep->lock, flags);

        /* Ph68: install in receiver's handle table (outside lock). */
        if (xfer_obj) {
            uint32_t new_h = deliver_cap(t, xfer_obj, xfer_rights);
            t->ipc_msg.attached_handle = new_h;
        }

        /* Wake sender after all data is set. */
        sender->state = TASK_READY;
        kobject_release(&ep->base);

        if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
            return syscall_err(IRIS_ERR_INVALID_ARG);
        return syscall_ok_u64(0);
    }

    /* No sender: enqueue receiver and block. */
    ep->ep_state     = EP_STATE_RECV;
    t->ep_next       = 0;
    t->blocking_ep   = ep;
    t->ipc_msg_ready = 0;
    t->ipc_ep_closed = 0;

    if (ep->queue_tail) { ep->queue_tail->ep_next = t; ep->queue_tail = t; }
    else                { ep->queue_head = t; ep->queue_tail = t; }

    t->state = TASK_BLOCKED_RECV;
    irq_spinlock_unlock(&ep->lock, flags);

    task_yield();

    kobject_release(&ep->base);

    if (t->ipc_ep_closed) { t->ipc_ep_closed = 0; return syscall_err(IRIS_ERR_CLOSED); }

    /* Ph69: copy staged kbuf to receiver's user space now that we're in receiver's CR3. */
    if (t->ipc_kbuf_len > 0 && t->ep_recv_buf_uptr != 0 &&
        user_range_writable(t->ep_recv_buf_uptr, t->ipc_kbuf_len) &&
        copy_to_user_checked(t->ep_recv_buf_uptr, t->ipc_kbuf, t->ipc_kbuf_len)) {
        t->ipc_msg.buf_len  = t->ipc_kbuf_len;
        t->ipc_msg.buf_uptr = t->ep_recv_buf_uptr;
    } else if (t->ipc_kbuf_len > 0) {
        t->ipc_msg.buf_len = t->ipc_kbuf_len; /* report available, even if unwritten */
    }
    t->ipc_kbuf_len     = 0;
    t->ep_recv_buf_uptr = 0;

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}

/* ── SYS_EP_NB_SEND ──────────────────────────────────────────────────── */

uint64_t sys_ep_nb_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    iris_error_t err;
    struct KEndpoint *ep = ep_get(t, (handle_id_t)arg0, RIGHT_WRITE, &err);
    if (!ep) return syscall_err(err);

    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Ph69: stage kbuf. */
    t->ipc_kbuf_len = 0;
    if (t->ipc_msg.buf_len > 0 && t->ipc_msg.buf_uptr != 0) {
        uint32_t n = t->ipc_msg.buf_len;
        if (n > IRIS_IPC_BUF_SIZE) n = IRIS_IPC_BUF_SIZE;
        if (!user_range_readable(t->ipc_msg.buf_uptr, n) ||
            !copy_from_user_checked(t->ipc_kbuf, t->ipc_msg.buf_uptr, n)) {
            kobject_release(&ep->base);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
        t->ipc_kbuf_len    = n;
        t->ipc_msg.buf_len = n;
    }

    /* Ph68: stage cap before taking lock. */
    struct KObject *xfer_obj    = 0;
    uint32_t        xfer_rights = 0;
    if (t->ipc_msg.attached_handle != IRIS_MSG_NO_CAP) {
        iris_error_t cr = stage_send_cap(t, t->ipc_msg.attached_handle,
                                         t->ipc_msg.attached_rights,
                                         &xfer_obj, &xfer_rights);
        if (cr != IRIS_OK) { kobject_release(&ep->base); return syscall_err(cr); }
        t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) kobject_release(xfer_obj);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_RECV) {
        irq_spinlock_unlock(&ep->lock, flags);
        if (xfer_obj) kobject_release(xfer_obj);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *receiver = ep->queue_head;
    ep->queue_head = receiver->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    receiver->ep_next     = 0;
    receiver->blocking_ep = 0;

    copy_irismsg(&receiver->ipc_msg, &t->ipc_msg);
    receiver->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;
    receiver->ipc_msg_ready           = 1;

    if (t->ipc_kbuf_len > 0) {
        copy_kbuf(receiver->ipc_kbuf, t->ipc_kbuf, t->ipc_kbuf_len);
        receiver->ipc_kbuf_len = t->ipc_kbuf_len;
    } else {
        receiver->ipc_kbuf_len = 0;
    }

    irq_spinlock_unlock(&ep->lock, flags);

    if (xfer_obj) {
        uint32_t new_h = deliver_cap(receiver, xfer_obj, xfer_rights);
        receiver->ipc_msg.attached_handle = new_h;
    }

    receiver->state = TASK_READY;
    kobject_release(&ep->base);
    return syscall_ok_u64(0);
}

/* ── SYS_EP_NB_RECV ──────────────────────────────────────────────────── */

uint64_t sys_ep_nb_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    iris_error_t err;
    struct KEndpoint *ep = ep_get(t, (handle_id_t)arg0, RIGHT_READ, &err);
    if (!ep) return syscall_err(err);

    /* Ph69: read receiver's hint. */
    uint64_t recv_buf = 0;
    {
        struct IrisMsg hints;
        if (user_range_readable(arg1, (uint32_t)sizeof(struct IrisMsg)) &&
            copy_from_user_checked(&hints, arg1, (uint32_t)sizeof(struct IrisMsg)))
            recv_buf = hints.buf_uptr;
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_SEND) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(&ep->base);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *sender = ep->queue_head;
    ep->queue_head = sender->ep_next;
    if (!ep->queue_head) { ep->queue_tail = 0; ep->ep_state = EP_STATE_IDLE; }
    sender->ep_next     = 0;
    sender->blocking_ep = 0;

    copy_irismsg(&t->ipc_msg, &sender->ipc_msg);
    t->ipc_msg.attached_handle = IRIS_MSG_NO_CAP;

    /* Ph69: direct copy to receiver's user space (correct CR3). */
    uint32_t kbuf_n = sender->ipc_kbuf_len;
    if (kbuf_n > 0 && recv_buf != 0 &&
        user_range_writable(recv_buf, kbuf_n) &&
        copy_to_user_checked(recv_buf, sender->ipc_kbuf, kbuf_n)) {
        t->ipc_msg.buf_len  = kbuf_n;
        t->ipc_msg.buf_uptr = recv_buf;
    } else {
        t->ipc_msg.buf_len  = kbuf_n;
        t->ipc_msg.buf_uptr = 0;
    }
    sender->ipc_kbuf_len = 0;

    /* Ph68: take sender's staged cap. */
    struct KObject *xfer_obj    = sender->ep_cap_obj;
    uint32_t        xfer_rights = sender->ep_cap_rights;
    sender->ep_cap_obj    = 0;
    sender->ep_cap_rights = 0;

    irq_spinlock_unlock(&ep->lock, flags);

    if (xfer_obj) {
        uint32_t new_h = deliver_cap(t, xfer_obj, xfer_rights);
        t->ipc_msg.attached_handle = new_h;
    }

    sender->state = TASK_READY;
    kobject_release(&ep->base);

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}
