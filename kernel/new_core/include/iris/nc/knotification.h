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
 *   - signal_bits is the only state protected by the KObject lock.
 *   - waiter is at most one task at a time (single-consumer model).
 *   - A second concurrent waiter is rejected with IRIS_ERR_BUSY rather than
 *     silently overwriting the current blocked waiter.
 *   - knotification_signal() is safe to call from IRQ context.
 *   - knotification_wait() must only be called from task context.
 *   - signal() wakes waiter before releasing the lock so the task
 *     sees bits > 0 when it retries.
 */

#define KNOTIF_POOL_SIZE 32  /* maximum live KNotification objects system-wide */

struct task; /* forward */

struct KNotification {
    struct KObject      base;          /* must be first */
    _Atomic uint64_t    signal_bits;   /* pending signals — bit N = signal N */
    uint8_t             closed;
    struct task        *waiter;        /* task blocked on wait, or NULL */
};

struct KNotification *knotification_alloc(void);
void                  knotification_free (struct KNotification *n);

/* Set one or more bits. Safe from IRQ context. */
void         knotification_signal(struct KNotification *n, uint64_t bits);

/* Block until signal_bits != 0. Atomically clears all pending bits
 * and returns them in *out_bits. Single-waiter only: returns IRIS_ERR_BUSY
 * if another task is already blocked on this notification. */
iris_error_t knotification_wait(struct KNotification *n, uint64_t *out_bits);

/* Non-blocking: returns pending bits (clearing them) or 0 if none pending. */
uint64_t     knotification_poll(struct KNotification *n);

#endif
