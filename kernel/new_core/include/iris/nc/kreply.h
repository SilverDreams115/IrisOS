#ifndef IRIS_NC_KREPLY_H
#define IRIS_NC_KREPLY_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

struct task; /* forward — avoids circular include with task.h */

/*
 * KReply — seL4-style one-shot reply capability (Ph85).
 *
 * Created by the kernel during SYS_EP_CALL rendezvous and delivered
 * to the server's handle table via msg.attached_handle.  The server
 * invokes it with SYS_REPLY to unblock the waiting caller.
 *
 * Invariants:
 *   - caller != NULL while the caller is in TASK_BLOCKED_REPLY.
 *   - kreply_obj_close() wakes caller with IRIS_ERR_CLOSED if the handle
 *     is dropped without SYS_REPLY being called.
 *   - After SYS_REPLY, caller == NULL; subsequent SYS_REPLY calls return
 *     IRIS_ERR_NOT_FOUND.
 */
struct KReply {
    struct KObject  base;   /* must be first */
    irq_spinlock_t  lock;
    struct task    *caller; /* NULL after reply or cancel */
};

struct KReply *kreply_alloc(struct task *caller);

/*
 * kreply_cancel_caller — wake caller with error when the task is torn down
 * while in TASK_BLOCKED_REPLY (e.g., process killed before server replies).
 * Clears r->caller but does NOT touch caller->pending_kreply (caller teardown
 * code must handle that separately to avoid double-release).
 */
void kreply_cancel_caller(struct KReply *r);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KREPLY_H */
