#ifndef IRIS_NC_SPINLOCK_H
#define IRIS_NC_SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct spinlock {
    atomic_flag flag;
} spinlock_t;

/*
 * Invariants:
 *   - Protects mutable state, not object lifecycle
 *   - Not reentrant
 *   - Must not be acquired twice by the same thread
 *   - Busy-wait is acceptable at this early stage
 */
static inline void spinlock_init(spinlock_t *lock) {
    atomic_flag_clear(&lock->flag);
}

static inline bool spinlock_try_lock(spinlock_t *lock) {
    return !atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire);
}

static inline void spinlock_lock(spinlock_t *lock) {
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
        __asm__ __volatile__("pause");
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

/*
 * irq_spinlock_t — spinlock that also saves and disables IRQs on acquire.
 *
 * Correctness contract:
 *   - saved_flags lives on the caller's stack, NOT inside the lock struct.
 *     On SMP each CPU acquires with its own flags; sharing flags in the lock
 *     would clobber the other CPU's saved state on unlock.
 *   - IRQs are disabled BEFORE the CAS loop so that an IRQ cannot fire
 *     between the save and the lock acquisition — which could deadlock if the
 *     IRQ handler tries to acquire the same lock on the same CPU.
 *   - Must not be acquired recursively (not reentrant).
 *
 * Usage:
 *   uint64_t saved = irq_spinlock_lock(&l);
 *   ...critical section...
 *   irq_spinlock_unlock(&l, saved);
 */
typedef struct {
    spinlock_t spin;
} irq_spinlock_t;

static inline void irq_spinlock_init(irq_spinlock_t *l) {
    spinlock_init(&l->spin);
}

static inline uint64_t irq_spinlock_lock(irq_spinlock_t *l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spinlock_lock(&l->spin);
    return flags;
}

static inline void irq_spinlock_unlock(irq_spinlock_t *l, uint64_t flags) {
    spinlock_unlock(&l->spin);
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory");
}

#endif
