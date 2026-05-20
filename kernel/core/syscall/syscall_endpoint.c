#include "syscall_priv.h"
#include <iris/nc/kendpoint.h>
#include <iris/ipc_msg.h>

static inline void copy_irismsg(struct IrisMsg *dst, const struct IrisMsg *src) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < (uint32_t)sizeof(struct IrisMsg); i++)
        d[i] = s[i];
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

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_ENDPOINT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KEndpoint *ep = (struct KEndpoint *)obj;

    /* Copy message from user space into sender's staging buffer. */
    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_RECV) {
        /* Rendezvous: a receiver is already waiting. */
        struct task *receiver = ep->queue_head;
        ep->queue_head = receiver->ep_next;
        if (!ep->queue_head) {
            ep->queue_tail = 0;
            ep->ep_state   = EP_STATE_IDLE;
        }
        receiver->ep_next     = 0;
        receiver->blocking_ep = 0;

        copy_irismsg(&receiver->ipc_msg, &t->ipc_msg);
        receiver->ipc_msg_ready = 1;
        receiver->state         = TASK_READY;

        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_ok_u64(0);
    }

    /* No receiver waiting: enqueue sender and block. */
    ep->ep_state     = EP_STATE_SEND;
    t->ep_next       = 0;
    t->blocking_ep   = ep;
    t->ipc_msg_ready = 0;
    t->ipc_ep_closed = 0;

    if (ep->queue_tail) {
        ep->queue_tail->ep_next = t;
        ep->queue_tail          = t;
    } else {
        ep->queue_head = t;
        ep->queue_tail = t;
    }

    t->state = TASK_BLOCKED_SEND;
    irq_spinlock_unlock(&ep->lock, flags);

    task_yield();

    kobject_release(obj);

    if (t->ipc_ep_closed) {
        t->ipc_ep_closed = 0;
        return syscall_err(IRIS_ERR_CLOSED);
    }
    return syscall_ok_u64(0);
}

/* ── SYS_EP_RECV ─────────────────────────────────────────────────────── */

uint64_t sys_ep_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_ENDPOINT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KEndpoint *ep = (struct KEndpoint *)obj;

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state == EP_STATE_SEND) {
        /* Rendezvous: a sender is waiting — grab its message directly. */
        struct task *sender = ep->queue_head;
        ep->queue_head = sender->ep_next;
        if (!ep->queue_head) {
            ep->queue_tail = 0;
            ep->ep_state   = EP_STATE_IDLE;
        }
        sender->ep_next     = 0;
        sender->blocking_ep = 0;
        sender->state       = TASK_READY;

        copy_irismsg(&t->ipc_msg, &sender->ipc_msg);

        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);

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

    if (ep->queue_tail) {
        ep->queue_tail->ep_next = t;
        ep->queue_tail          = t;
    } else {
        ep->queue_head = t;
        ep->queue_tail = t;
    }

    t->state = TASK_BLOCKED_RECV;
    irq_spinlock_unlock(&ep->lock, flags);

    task_yield();

    kobject_release(obj);

    if (t->ipc_ep_closed) {
        t->ipc_ep_closed = 0;
        return syscall_err(IRIS_ERR_CLOSED);
    }

    /* ipc_msg was filled by the sender during rendezvous. */
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

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_ENDPOINT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KEndpoint *ep = (struct KEndpoint *)obj;

    if (!copy_from_user_checked(&t->ipc_msg, arg1, (uint32_t)sizeof(struct IrisMsg))) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_RECV) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *receiver = ep->queue_head;
    ep->queue_head = receiver->ep_next;
    if (!ep->queue_head) {
        ep->queue_tail = 0;
        ep->ep_state   = EP_STATE_IDLE;
    }
    receiver->ep_next     = 0;
    receiver->blocking_ep = 0;

    copy_irismsg(&receiver->ipc_msg, &t->ipc_msg);
    receiver->ipc_msg_ready = 1;
    receiver->state         = TASK_READY;

    irq_spinlock_unlock(&ep->lock, flags);
    kobject_release(obj);
    return syscall_ok_u64(0);
}

/* ── SYS_EP_NB_RECV ──────────────────────────────────────────────────── */

uint64_t sys_ep_nb_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_writable(arg1, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_ENDPOINT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KEndpoint *ep = (struct KEndpoint *)obj;

    uint64_t flags = irq_spinlock_lock(&ep->lock);

    if (ep->closed) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_CLOSED);
    }

    if (ep->ep_state != EP_STATE_SEND) {
        irq_spinlock_unlock(&ep->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }

    struct task *sender = ep->queue_head;
    ep->queue_head = sender->ep_next;
    if (!ep->queue_head) {
        ep->queue_tail = 0;
        ep->ep_state   = EP_STATE_IDLE;
    }
    sender->ep_next     = 0;
    sender->blocking_ep = 0;
    sender->state       = TASK_READY;

    copy_irismsg(&t->ipc_msg, &sender->ipc_msg);

    irq_spinlock_unlock(&ep->lock, flags);
    kobject_release(obj);

    if (!copy_to_user_checked(arg1, &t->ipc_msg, (uint32_t)sizeof(struct IrisMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    return syscall_ok_u64(0);
}
