#ifndef IRIS_COMMON_TIMER_H
#define IRIS_COMMON_TIMER_H

/*
 * iris_timer.h — waiting, as a request to a server (ledger A-24).
 *
 * There is no syscall that blocks a thread on time.  `SYS_SLEEP`,
 * `SYS_CLOCK_NANOSLEEP` and `SYS_NOTIFY_WAIT_TIMEOUT` are retired, because a
 * kernel that can block on time owns a policy about time and seL4's does not.
 * What replaces them is a capability: the timer service's endpoint, which a
 * task either was granted or was not.
 *
 * `iris_timer_arm` asks it to signal a notification the caller HANDS OVER, in
 * N nanoseconds.  Waiting is then the ordinary wait every other event uses.
 *
 * One thing a caller must know, and it is inherent rather than an artefact:
 * an armed timer that is no longer wanted still fires.  A bounded wait that
 * ends early leaves a signal in flight, so `IRIS_TIMER_BIT` is reserved for it
 * and callers mask it out of what they observe.  The kernel used to hide this
 * by cancelling the deadline when the thread woke — which is exactly the
 * bookkeeping about somebody else's waiting that it should not have been
 * doing.
 */
#include <stdint.h>
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/ipc_msg.h>
#include <iris/nc/rights.h>
#include "../timer/timer_proto.h"

/* The signal bit a timeout arrives on.  63 so it cannot collide with the
 * low-numbered bits services and tests assign to their own events. */
#define IRIS_TIMER_BIT   (1ull << 63)

/*
 * Ask the timer service to signal `notif` with `bits` after `ns` nanoseconds.
 * 0 on success, negative on failure (including "no timer service granted",
 * which is a missing capability and reads as one).
 *
 * `notif_give` is a capability the caller gives away.  Transfer is a COPY
 * (ledger A-29), as it is in seL4, so giving something away is two steps and
 * both belong to the caller: derive a fresh copy of the notification per arm —
 * RIGHT_WRITE (the service signals it) and RIGHT_TRANSFER (it may be handed
 * over at all) — and delete that slot once this returns 0.  What the service
 * keeps is a derivation CHILD of it, which is the point: the service can
 * signal what it was handed and nothing else, the caller can revoke the grant
 * at any time, and the grant ends on its own when the timer fires and the
 * service deletes its copy.
 */
static inline long iris_timer_arm(long timer_ep, long notif_give,
                                  uint64_t bits, uint64_t ns,
                                  uint64_t *out_token) {
    struct IrisMsg m;
    uint8_t *b = (uint8_t *)&m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) b[i] = 0;
    m.label               = TMR_OP_ARM;
    m.words[0]            = ns;
    m.words[1]            = bits;
    m.word_count          = 2u;
    m.attached_cap        = (uint32_t)notif_give;
    m.attached_cap_rights = RIGHT_WRITE;
    long r = iris_invoke1(timer_ep, INV_EP_CALL, (long)(uintptr_t)&m);
    if (r != 0) return r;
    if (m.words[0] != 0u) return -1;
    if (out_token) *out_token = m.words[1];
    return 0;
}

/*
 * Take back a timer that is no longer wanted.
 *
 * A bounded wait that ends early leaves one armed, and a service that is never
 * told holds a table entry and a capability per abandoned wait until the
 * deadline.  The kernel used to do this silently when it cancelled a sleeping
 * thread's deadline; it is a message now, and the service checks the BADGE on
 * the capability it arrives through, so a client can only take back its own.
 */
static inline long iris_timer_cancel(long timer_ep, uint64_t token) {
    struct IrisMsg m;
    uint8_t *b = (uint8_t *)&m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(m); i++) b[i] = 0;
    m.label      = TMR_OP_CANCEL;
    m.words[0]   = token;
    m.word_count = 1u;
    long r = iris_invoke1(timer_ep, INV_EP_CALL, (long)(uintptr_t)&m);
    if (r != 0) return r;
    return (m.words[0] == 0u) ? 0 : -1;
}

#endif /* IRIS_COMMON_TIMER_H */
