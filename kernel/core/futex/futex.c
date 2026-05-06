#include <iris/futex.h>
#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <iris/nc/kprocess.h>
#include <stdint.h>

#define FUTEX_TABLE_SIZE 256

struct futex_entry {
    uint64_t         uaddr;
    struct task     *waiter;
    struct KProcess *owner;  /* process whose address space owns uaddr */
};

static struct futex_entry futex_table[FUTEX_TABLE_SIZE];

iris_error_t futex_wait(uint64_t uaddr, uint32_t expected) {
    uint32_t val;
    int slot;
    uint64_t saved_flags;

    /* Pre-check outside the lock — avoids the page walk under cli. */
    if (!copy_from_user_checked(&val, uaddr, sizeof(val)))
        return IRIS_ERR_INVALID_ARG;
    if (val != expected)
        return IRIS_ERR_WOULD_BLOCK;

    /* Re-read under IRQ disable to close the TOCTOU window between the
     * value check above and the slot registration below.  futex_wake must
     * run on the same core to clear a slot, so cli makes check+register
     * atomic with respect to any concurrent wake on this CPU. */
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");

    if (!copy_from_user_checked(&val, uaddr, sizeof(val))) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory");
        return IRIS_ERR_INVALID_ARG;
    }
    if (val != expected) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory");
        return IRIS_ERR_WOULD_BLOCK;
    }

    slot = -1;
    for (int i = 0; i < FUTEX_TABLE_SIZE; i++) {
        if (futex_table[i].uaddr == 0) { slot = i; break; }
    }
    if (slot < 0) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory");
        return IRIS_ERR_TABLE_FULL;
    }

    struct task *t = task_current();
    if (!t || !t->process) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory");
        return IRIS_ERR_INVALID_ARG;
    }

    futex_table[slot].uaddr  = uaddr;
    futex_table[slot].waiter = t;
    futex_table[slot].owner  = t->process;
    t->state = TASK_BLOCKED_IPC;

    __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory");

    task_yield();
    /* Woken by futex_wake or cancelled by futex_cancel_waiter. */
    return IRIS_OK;
}

uint32_t futex_wake(uint64_t uaddr, uint32_t count) {
    struct KProcess *caller_proc = task_current()->process;
    uint32_t woken = 0;
    for (int i = 0; i < FUTEX_TABLE_SIZE && woken < count; i++) {
        if (futex_table[i].uaddr != uaddr || !futex_table[i].waiter)
            continue;
        /* Only wake waiters within the same address space. */
        if (futex_table[i].owner != caller_proc)
            continue;
        struct task *t = futex_table[i].waiter;
        futex_table[i].uaddr  = 0;
        futex_table[i].waiter = 0;
        futex_table[i].owner  = 0;
        if (t->state == TASK_BLOCKED_IPC)
            t->state = TASK_READY;
        woken++;
    }
    return woken;
}

void futex_cancel_waiter(struct task *t) {
    if (!t) return;
    for (int i = 0; i < FUTEX_TABLE_SIZE; i++) {
        if (futex_table[i].waiter == t) {
            futex_table[i].uaddr  = 0;
            futex_table[i].waiter = 0;
            futex_table[i].owner  = 0;
        }
    }
}
