#ifndef IRIS_NC_KNOTIFICATION_H
#define IRIS_NC_KNOTIFICATION_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <stdint.h>
#include <stdatomic.h>

/*
 * KNotification — lightweight signal object.
 *
 * Represents a set of up to 64 independent signal bits.
 * Kernel code (e.g. IRQ handlers) can call knotification_signal() to
 * set bits. A waiting task (ring-3 server or kernel task) calls
 * knotification_wait() which blocks until at least one bit is set,
 * then atomically clears and returns the pending bits.
 *
 * Invariants:
 *   - signal_bits is protected by both atomics and the KObject spinlock.
 *   - Up to KNOTIF_WAITERS_MAX tasks may block concurrently.
 *   - knotification_signal() wakes exactly one waiter (the first blocked one).
 *   - knotification_close() wakes all waiters so they can observe CLOSED.
 *   - knotification_signal() is safe to call from IRQ context.
 *   - knotification_wait() must only be called from task context.
 */

#define KNOTIF_POOL_SIZE    64  /* maximum live KNotification objects system-wide */
#define KNOTIF_WAITERS_MAX   4  /* max tasks blocked on one notification at once */

struct task; /* forward */

struct KNotification {
    struct KObject      base;                        /* must be first */
    _Atomic uint64_t    signal_bits;                 /* pending signals — bit N = signal N */
    uint8_t             closed;
    uint32_t            waiter_count;
    struct task        *waiters[KNOTIF_WAITERS_MAX]; /* tasks blocked on wait */
};

struct KNotification *knotification_alloc(void);
void                  knotification_free (struct KNotification *n);
void                  knotification_cancel_waiter(struct task *t);

/* Set one or more bits. Safe from IRQ context. */
void         knotification_signal(struct KNotification *n, uint64_t bits);

/* Block until signal_bits != 0. Atomically clears all pending bits
 * and returns them in *out_bits. Up to KNOTIF_WAITERS_MAX tasks may block
 * concurrently; returns IRIS_ERR_BUSY if the waiter table is full. */
iris_error_t knotification_wait(struct KNotification *n, uint64_t *out_bits);

/* Non-blocking: returns pending bits (clearing them) or 0 if none pending. */
uint64_t     knotification_poll(struct KNotification *n);
uint32_t     knotification_live_count(void);

#endif
