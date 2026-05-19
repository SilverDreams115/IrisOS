#include <iris/futex.h>
#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

#define FUTEX_TABLE_SIZE 256

struct futex_entry {
    uint64_t         uaddr;
    struct task     *waiter;
    struct KProcess *owner;  /* process whose address space owns uaddr */
};

static struct futex_entry futex_table[FUTEX_TABLE_SIZE];

/* IRQ-disabling spinlock: futex_wake may be called from task context while
 * futex_wait's check+register sequence must be atomic with respect to wakes
 * on the same CPU.  Disabling IRQs prevents the timer ISR (which calls
 * reap_pending_dead_task) from observing a partially-registered slot.
 *
 * Static zero-initialization is correct: atomic_flag starts cleared (=unlocked)
 * in C11, so no explicit futex_init() call is required. */
static irq_spinlock_t futex_lock;

iris_error_t futex_wait(uint64_t uaddr, uint32_t expected) {
    uint32_t val;

    /* Pre-check outside the lock — avoids the user copy under IRQ disable. */
    if (!copy_from_user_checked(&val, uaddr, sizeof(val)))
        return IRIS_ERR_INVALID_ARG;
    if (val != expected)
        return IRIS_ERR_WOULD_BLOCK;

    /* Re-read under lock to close the TOCTOU window between the value check
     * above and the slot registration below.  The lock disables IRQs so no
     * futex_wake can interleave between check and register on this CPU. */
    uint64_t saved = irq_spinlock_lock(&futex_lock);

    if (!copy_from_user_checked(&val, uaddr, sizeof(val))) {
        irq_spinlock_unlock(&futex_lock, saved);
        return IRIS_ERR_INVALID_ARG;
    }
    if (val != expected) {
        irq_spinlock_unlock(&futex_lock, saved);
        return IRIS_ERR_WOULD_BLOCK;
    }

    int slot = -1;
    for (int i = 0; i < FUTEX_TABLE_SIZE; i++) {
        if (futex_table[i].uaddr == 0) { slot = i; break; }
    }
    if (slot < 0) {
        irq_spinlock_unlock(&futex_lock, saved);
        return IRIS_ERR_TABLE_FULL;
    }

    struct task *t = task_current();
    if (!t || !t->process) {
        irq_spinlock_unlock(&futex_lock, saved);
        return IRIS_ERR_INVALID_ARG;
    }

    futex_table[slot].uaddr  = uaddr;
    futex_table[slot].waiter = t;
    futex_table[slot].owner  = t->process;
    t->state = TASK_BLOCKED_IPC;

    irq_spinlock_unlock(&futex_lock, saved);

    task_yield();
    /* Woken by futex_wake or cancelled by futex_cancel_waiter. */
    return IRIS_OK;
}

uint32_t futex_wake(uint64_t uaddr, uint32_t count) {
    struct task *t = task_current();
    if (!t || !t->process) return 0;
    struct KProcess *caller_proc = t->process;
    uint32_t woken = 0;

    uint64_t saved = irq_spinlock_lock(&futex_lock);
    for (int i = 0; i < FUTEX_TABLE_SIZE && woken < count; i++) {
        if (futex_table[i].uaddr != uaddr || !futex_table[i].waiter)
            continue;
        if (futex_table[i].owner != caller_proc)
            continue;
        struct task *w = futex_table[i].waiter;
        futex_table[i].uaddr  = 0;
        futex_table[i].waiter = 0;
        futex_table[i].owner  = 0;
        if (w->state == TASK_BLOCKED_IPC)
            w->state = TASK_READY;
        woken++;
    }
    irq_spinlock_unlock(&futex_lock, saved);
    return woken;
}

void futex_cancel_waiter(struct task *t) {
    if (!t) return;
    uint64_t saved = irq_spinlock_lock(&futex_lock);
    for (int i = 0; i < FUTEX_TABLE_SIZE; i++) {
        if (futex_table[i].waiter == t) {
            futex_table[i].uaddr  = 0;
            futex_table[i].waiter = 0;
            futex_table[i].owner  = 0;
        }
    }
    irq_spinlock_unlock(&futex_lock, saved);
}
