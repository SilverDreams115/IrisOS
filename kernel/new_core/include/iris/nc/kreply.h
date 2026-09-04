#ifndef IRIS_NC_KREPLY_H
#define IRIS_NC_KREPLY_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

struct task; /* forward — avoids circular include with task.h */

/*
 * KReply — seL4-style reply object (Ph85; Phase S1: explicit MCS-style).
 *
 * Phase S1: reply objects are canonical kernel objects.  They are created
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
struct KSchedContext;

struct KReply {
    struct KObject  base;   /* must be first */
    irq_spinlock_t  lock;
    struct task    *caller; /* NULL after reply or cancel */
    uint8_t         staged; /* Phase S1: claimed by a receiver, not yet bound */

    /*
     * Stage 8-mcs — SCHEDULING CONTEXT DONATION.
     *
     * When a client Calls an endpoint served by a PASSIVE thread — one with no
     * scheduling context of its own — the client's SC is lent to the server
     * for the duration, so the server runs on the requester's time.  That is
     * what makes time an authority you delegate rather than a property a
     * thread is born with, and it is why a passive server cannot be starved
     * into uselessness or spun up by a client that has no budget to give.
     *
     * The donation is recorded HERE, on the reply object, not on the pair of
     * threads — because the reply is what ends it.  A server that stashes a
     * reply and answers later still has to go through this object to do it, so
     * there is exactly one place the SC can come back from, and it is the same
     * place for a normal reply, an endpoint that closed, and a caller that
     * died.
     *
     * The client is in TASK_BLOCKED_REPLY for the whole window and budget is
     * only charged to a RUNNING thread, so nothing is double-spent: the time
     * the server uses is time the client provably is not using.
     */
    struct KSchedContext *donated_sc;  /* NULL = nothing was donated */
    struct task          *donated_to;  /* the server currently running on it */
};

/* Stage 8-mcs: record that `sc` moved from the caller to `to` for this
 * binding.  Caller has already cleared its own sched_ctx and set the server's;
 * this only records where it must go back to. */
void kreply_note_donation(struct KReply *r, struct KSchedContext *sc,
                          struct task *to);
/*
 * Do the whole loan: if `receiver` is PASSIVE (no SC of its own) and `sender`
 * has one, move it across and record it on `r`.  No-op otherwise.
 *
 * One function because there are THREE rendezvous paths — the receiver taking
 * a queued sender, the receiver taking one in the non-blocking form, and the
 * sender finding a receiver already waiting — and a donation wired into two of
 * them is a server that runs unbudgeted depending on which side arrived first.
 * That is precisely the bug this shape was introduced to prevent, and it is
 * the bug T308 caught before this helper existed.
 */
void kreply_donate_on_call(struct KReply *r, struct task *sender,
                           struct task *receiver);
/* Undo the donation: puts the SC back on the caller and clears the server's,
 * whatever the reason the binding is ending.  Idempotent, and a no-op when
 * nothing was donated. */
void kreply_return_donation(struct KReply *r, struct task *back_to);

/* Phase S1: placement-init a KReply inside untyped-backed memory (the ONLY
 * production creation path).  'mem' must be a kuntyped_alloc_child(ren) area. */
struct KReply *kreply_alloc_at(void *mem);

/* Phase S1: receiver-side claim/unclaim + rendezvous bind.
 *   stage:   free → staged (IRIS_ERR_BUSY if staged or bound)
 *   unstage: staged → free (idempotent)
 *   bind:    staged → bound to 'caller' (IRIS_ERR_BUSY unless staged)
 */
iris_error_t kreply_stage(struct KReply *r);
void         kreply_unstage(struct KReply *r);
iris_error_t kreply_bind_caller(struct KReply *r, struct task *caller);

/* Phase 18/S1: live KReply count + retype/destroy counters (diagnostics). */
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
