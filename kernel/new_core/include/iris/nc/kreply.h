#ifndef IRIS_NC_KREPLY_H
#define IRIS_NC_KREPLY_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

struct task; /* forward — avoids circular include with task.h */

/*
 * KReply — seL4-style reply object (Ph85; Fase S1: explicit MCS-style).
 *
 * Fase S1: reply objects are canonical kernel objects.  They are created
 * ONLY via SYS_UNTYPED_RETYPE2 (storage inside the source KUntyped) and the
 * capability lives in the server's CSpace.  A server passes its reply CPtr
 * as arg2 of SYS_EP_RECV / SYS_EP_NB_RECV; the kernel BINDS the blocked
 * EP_CALL caller into that object at rendezvous.  The kernel never fabricates
 * a KReply during IPC (the Ph85 implicit kslab allocation is retired).
 *
 * Lifecycle of one binding (one-shot per protocol):
 *   free (caller==NULL, staged==0)
 *     → staged  (claimed by a receiver entering EP_RECV; staged==1)
 *     → bound   (EP_CALL rendezvous; caller != NULL, staged==0)
 *     → free    (SYS_REPLY / caller death / cancel)
 * The object itself is reusable across bindings; SYS_REPLY on a free object
 * returns IRIS_ERR_NOT_FOUND (stale/one-shot contract unchanged).
 *
 * Invariants:
 *   - caller != NULL while the caller is in TASK_BLOCKED_REPLY.
 *   - kreply_obj_close() (last capability dropped) wakes a bound caller with
 *     IRIS_ERR_CLOSED.
 *   - staged==1 marks a receiver's exclusive claim: a second concurrent
 *     EP_RECV naming the same reply object fails with IRIS_ERR_BUSY.
 */
struct KReply {
    struct KObject  base;   /* must be first */
    irq_spinlock_t  lock;
    struct task    *caller; /* NULL after reply or cancel */
    uint8_t         staged; /* Fase S1: claimed by a receiver, not yet bound */
};

/* Fase S1: placement-init a KReply inside untyped-backed memory (the ONLY
 * production creation path).  'mem' must be a kuntyped_alloc_child(ren) area. */
struct KReply *kreply_alloc_at(void *mem);

/* Fase S1: receiver-side claim/unclaim + rendezvous bind.
 *   stage:   free → staged (IRIS_ERR_BUSY if staged or bound)
 *   unstage: staged → free (idempotent)
 *   bind:    staged → bound to 'caller' (IRIS_ERR_BUSY unless staged)
 */
iris_error_t kreply_stage(struct KReply *r);
void         kreply_unstage(struct KReply *r);
iris_error_t kreply_bind_caller(struct KReply *r, struct task *caller);

/* Fase 18/S1: live KReply count + retype/destroy counters (diagnostics). */
uint32_t kreply_live_count(void);

/*
 * kreply_cancel_caller — wake caller with error when the task is torn down
 * while in TASK_BLOCKED_REPLY (e.g., process killed before server replies).
 * Clears r->caller but does NOT touch caller->pending_kreply (caller teardown
 * code must handle that separately to avoid double-release).
 */
void kreply_cancel_caller(struct KReply *r);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KREPLY_H */
