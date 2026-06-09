/* Test stub — replaces the real spinlock.h for host unit tests.
 * Preserves struct layouts (atomic_flag = 1 byte) but removes CLI/PAUSE asm
 * that traps in user mode.  Single-threaded tests need no actual locking. */
#ifndef IRIS_NC_SPINLOCK_H
#define IRIS_NC_SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef struct spinlock { atomic_flag flag; } spinlock_t;
typedef struct { spinlock_t spin; } irq_spinlock_t;

static inline void spinlock_init(spinlock_t *l)        { (void)l; }
static inline bool spinlock_try_lock(spinlock_t *l)    { (void)l; return true; }
static inline void spinlock_lock(spinlock_t *l)        { (void)l; }
static inline void spinlock_unlock(spinlock_t *l)      { (void)l; }

static inline void irq_spinlock_init(irq_spinlock_t *l) { (void)l; }
static inline uint64_t irq_spinlock_lock(irq_spinlock_t *l) { (void)l; return 0; }
static inline void irq_spinlock_unlock(irq_spinlock_t *l, uint64_t f) { (void)l; (void)f; }

#endif
