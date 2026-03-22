#ifndef IRIS_NC_SPINLOCK_H
#define IRIS_NC_SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct spinlock {
    atomic_flag flag;
} spinlock_t;

/*
 * Invariantes:
 *   - Protege estado mutable, no lifecycle
 *   - No es reentrante
 *   - No debe adquirirse dos veces por el mismo thread
 *   - Busy-wait aceptable en esta fase temprana
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

#endif
