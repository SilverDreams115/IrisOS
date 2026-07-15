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

/* Fase S1: Untyped retype is the ONLY creation path (kslab variant retired). */
struct KEndpoint *kendpoint_alloc_at(void *mem);
void              kendpoint_close(struct KEndpoint *ep);

/* Fase 18: live KEndpoint object count (additive diagnostics). */
uint32_t          kendpoint_live_count(void);
void              kendpoint_cancel_waiter(struct task *t);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KENDPOINT_H */
