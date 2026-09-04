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

#define KNOTIF_POOL_SIZE     0  /* no static pool — kpage-backed; 0 = unbounded allocator ceiling */
#define KNOTIF_WAITERS_MAX   4  /* max tasks blocked on one notification at once */

struct task; /* forward */

struct KNotification {
    struct KObject      base;                        /* must be first */
    _Atomic uint64_t    signal_bits;                 /* pending signals — bit N = signal N */
    uint8_t             closed;
    uint32_t            waiter_count;
    struct task        *waiters[KNOTIF_WAITERS_MAX]; /* tasks blocked on wait */
    struct KNotification *live_prev;
    struct KNotification *live_next;
};

/* Phase S1: Untyped retype is the ONLY creation path (the kslab variant and
 * the per-process owner/quota binding are retired — Untyped is the budget). */
struct KNotification *knotification_alloc_at(void *mem);
void                  knotification_free (struct KNotification *n);
void                  knotification_cancel_waiter(struct task *t);

/* Set one or more bits. Safe from IRQ context. */
void         knotification_signal(struct KNotification *n, uint64_t bits);

/* Block until signal_bits != 0. Atomically clears all pending bits
 * and returns them in *out_bits. Up to KNOTIF_WAITERS_MAX tasks may block
 * concurrently; returns IRIS_ERR_BUSY if the waiter table is full. */
iris_error_t knotification_wait(struct KNotification *n, uint64_t *out_bits);
/*
 * Stage 9-evt Step 1 — the RESTARTABLE half of knotification_wait.
 *
 * Does one non-blocking attempt.  Returns IRIS_OK with the bits, IRIS_ERR_CLOSED,
 * IRIS_ERR_BUSY (waiter table full), or IRIS_ERR_WOULD_BLOCK meaning "you are
 * enqueued and parked — ask to be re-executed".  It holds nothing across the
 * block: the caller's continuation is its membership of the waiter list, which
 * is state on the NOTIFICATION and the THREAD, not on a kernel stack.
 *
 * It removes the caller from the waiter list on entry, so a re-execution after
 * any kind of wake — signal, close, or a spurious one — starts from a clean
 * position rather than accumulating registrations.
 */
iris_error_t knotification_wait_step(struct KNotification *n, uint64_t *out_bits);
/*
 * Stage 9-evt Step 1 — the restartable half of knotification_wait_timeout.
 *
 * Same contract as knotification_wait_step, plus a deadline: on the first
 * attempt it arms `wake_tick`, and it reports IRIS_ERR_TIMED_OUT when the
 * scheduler has marked the thread timed out.  `first` distinguishes the two
 * entries, because the waker CLEARS the state the handler parked on and a
 * handler cannot infer "resuming" from what is left.
 */
iris_error_t knotification_wait_timeout_step(struct KNotification *n,
                                             uint64_t *out_bits,
                                             uint64_t deadline_ticks,
                                             int first);
iris_error_t knotification_wait_timeout(struct KNotification *n, uint64_t *out_bits,
                                        uint64_t deadline_ticks);

/* Non-blocking: returns pending bits (clearing them) or 0 if none pending. */
uint64_t     knotification_poll(struct KNotification *n);
uint32_t     knotification_live_count(void);

#endif
