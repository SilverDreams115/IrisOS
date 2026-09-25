/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_NC_KENDPOINT_H
#define IRIS_NC_KENDPOINT_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>

/* Endpoint queue state (mutually exclusive: either senders wait OR receivers wait). */
#define EP_STATE_IDLE 0
#define EP_STATE_SEND 1   /* sender(s) queued, no receiver ready */
#define EP_STATE_RECV 2   /* receiver(s) queued, no sender ready */

struct task;

struct KEndpoint {
    struct KObject   base;       /* must be first */
    irq_spinlock_t   lock;
    int              ep_state;
    int              closed;
    struct task     *queue_head;
    struct task     *queue_tail;
};

/* Phase S1: Untyped retype is the ONLY creation path (kslab variant retired). */
struct KEndpoint *kendpoint_alloc_at(void *mem);
void              kendpoint_close(struct KEndpoint *ep);

/* Phase 18: live KEndpoint object count (additive diagnostics). */
uint32_t          kendpoint_live_count(void);
void              kendpoint_cancel_waiter(struct task *t);

/*
 * Ledger A-22 — deliver a FAULT as a call on this endpoint.  See the
 * definition in syscall_endpoint.c: same rendezvous as SYS_EP_CALL with
 * everything a syscall brings stripped out, because the kernel composed the
 * message and there is no syscall frame under the caller.
 */
struct ipc_stage;
int               kendpoint_fault_call(struct task *t, struct KEndpoint *ep,
                                       const struct ipc_stage *msg);

/*
 * Ledger A-23 — hand a BOUND NOTIFICATION's signal to a thread that is blocked
 * receiving on an endpoint.  Returns 1 if it was delivered (the thread was
 * dequeued and woken), 0 if the thread was not blocked on an endpoint, in
 * which case the caller keeps the bits.
 */
int               kendpoint_deliver_notification(struct task *t, uint64_t bits);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KENDPOINT_H */
