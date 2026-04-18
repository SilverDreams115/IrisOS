#include <iris/futex.h>
#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <stdint.h>

#define FUTEX_TABLE_SIZE 64

struct futex_entry {
    uint64_t    uaddr;
    struct task *waiter;
};

static struct futex_entry futex_table[FUTEX_TABLE_SIZE];

iris_error_t futex_wait(uint64_t uaddr, uint32_t expected) {
    uint32_t val;
    int slot;

    if (!copy_from_user_checked(&val, uaddr, sizeof(val)))
        return IRIS_ERR_INVALID_ARG;

    if (val != expected)
        return IRIS_ERR_WOULD_BLOCK;  /* value changed; retry in userspace */

    slot = -1;
    for (int i = 0; i < FUTEX_TABLE_SIZE; i++) {
        if (futex_table[i].uaddr == 0) { slot = i; break; }
    }
    if (slot < 0)
        return IRIS_ERR_TABLE_FULL;

    struct task *t = task_current();
    futex_table[slot].uaddr  = uaddr;
    futex_table[slot].waiter = t;
    t->state = TASK_BLOCKED_IPC;
    task_yield();

    /* Woken by futex_wake or cancelled by futex_cancel_waiter. */
    return IRIS_OK;
}

uint32_t futex_wake(uint64_t uaddr, uint32_t count) {
    uint32_t woken = 0;
    for (int i = 0; i < FUTEX_TABLE_SIZE && woken < count; i++) {
        if (futex_table[i].uaddr != uaddr || !futex_table[i].waiter)
            continue;
        struct task *t = futex_table[i].waiter;
        futex_table[i].uaddr  = 0;
        futex_table[i].waiter = 0;
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
        }
    }
}
