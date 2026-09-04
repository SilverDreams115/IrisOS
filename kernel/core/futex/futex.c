#include <iris/futex.h>
#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>

/*
 * Futex wait table — hash-bucketed for per-address-group locking.
 *
 * 32 buckets × 8 slots = 256 total waiters (same capacity as before).
 * Each bucket has its own irq_spinlock so futex_wait and futex_wake on
 * different addresses never contend.  futex_cancel_waiter still walks
 * all buckets but each lock hold is short.
 *
 * Zero-initialization is sufficient: atomic_flag starts cleared (unlocked)
 * in C11, so no explicit init call is required.
 */
#define FUTEX_BUCKETS    32u
#define FUTEX_BUCKET_CAP  8u

/*
 * Stage 7 Step 15: the scope of a futex address is the ADDRESS SPACE.
 *
 * `owner` was the waiter's KProcess, used for exactly one thing — refusing to
 * wake a waiter that hashed to the same user address in a different address
 * space.  The process was standing in for the address space, which is what
 * actually makes two identical addresses different futexes, and a thread has
 * held its own VSpace capability since Step 5.  Same scoping, named after the
 * thing it scopes.
 */
struct futex_entry {
    uint64_t         uaddr;
    struct task     *waiter;
    struct KVSpace  *owner;
};

struct futex_bucket {
    irq_spinlock_t    lock;
    struct futex_entry entries[FUTEX_BUCKET_CAP];
};

static struct futex_bucket futex_buckets[FUTEX_BUCKETS];

static inline uint32_t futex_hash(uint64_t uaddr) {
    /* Mix page-level and cache-line-level bits into FUTEX_BUCKETS slots. */
    return (uint32_t)((uaddr >> 2) ^ (uaddr >> 12)) & (FUTEX_BUCKETS - 1u);
}

iris_error_t futex_wait(uint64_t uaddr, uint32_t expected, uint64_t deadline_ticks) {
    uint32_t val;

    /* Pre-check outside the lock — avoids the user copy under IRQ disable. */
    if (!copy_from_user_checked(&val, uaddr, sizeof(val)))
        return IRIS_ERR_INVALID_ARG;
    if (val != expected)
        return IRIS_ERR_WOULD_BLOCK;

    struct futex_bucket *bucket = &futex_buckets[futex_hash(uaddr)];
    uint64_t saved = irq_spinlock_lock(&bucket->lock);

    /* Re-read under lock to close the TOCTOU window. */
    if (!copy_from_user_checked(&val, uaddr, sizeof(val))) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_INVALID_ARG;
    }
    if (val != expected) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_WOULD_BLOCK;
    }

    int slot = -1;
    for (int i = 0; i < (int)FUTEX_BUCKET_CAP; i++) {
        if (bucket->entries[i].uaddr == 0) { slot = i; break; }
    }
    if (slot < 0) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_TABLE_FULL;
    }

    struct task *t = task_current();
    if (!t || !t->vspace) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_INVALID_ARG;
    }

    bucket->entries[slot].uaddr  = uaddr;
    bucket->entries[slot].waiter = t;
    bucket->entries[slot].owner  = t->vspace;
    t->state     = TASK_BLOCKED_IPC;
    t->wake_tick = deadline_ticks;

    irq_spinlock_unlock(&bucket->lock, saved);

    task_yield();
    if (t->timed_out) {
        t->timed_out = 0;
        futex_cancel_waiter(t);
        return IRIS_ERR_TIMED_OUT;
    }
    return IRIS_OK;
}

/*
 * Stage 9-evt Step 1 — the RESTARTABLE half of futex_wait (ledger D-1).
 *
 * The loop body without the task_yield: the retry is the dispatcher
 * re-executing the syscall, and the continuation is the bucket entry — state
 * on the FUTEX and the thread, not on a kernel stack.
 *
 * Returns IRIS_OK (woken), IRIS_ERR_TIMED_OUT, IRIS_ERR_WOULD_BLOCK meaning
 * "the value did not match, do not block" (the caller's contract, unchanged),
 * or IRIS_ERR_BUSY meaning "enqueued and parked — ask to be re-executed".
 * Those last two are deliberately different: one is an answer, the other is
 * not an answer yet, and collapsing them is how a futex loses a wakeup.
 */
iris_error_t futex_wait_step(uint64_t uaddr, uint32_t expected,
                             uint64_t deadline_ticks, int first) {
    struct task *t = task_current();
    if (!t) return IRIS_ERR_INVALID_ARG;

    if (!first) {
        /* Resuming.  The only two ways out of the park are the deadline and a
         * futex_wake, and the waker removes the bucket entry, so there is
         * nothing to re-check: which one happened is the whole answer. */
        if (t->timed_out) {
            t->timed_out = 0;
            t->wake_tick = 0;
            futex_cancel_waiter(t);
            return IRIS_ERR_TIMED_OUT;
        }
        t->wake_tick = 0;
        return IRIS_OK;
    }

    uint32_t val;
    if (!copy_from_user_checked(&val, uaddr, sizeof(val)))
        return IRIS_ERR_INVALID_ARG;
    if (val != expected)
        return IRIS_ERR_WOULD_BLOCK;

    struct futex_bucket *bucket = &futex_buckets[futex_hash(uaddr)];
    uint64_t saved = irq_spinlock_lock(&bucket->lock);

    /* Re-read under the lock to close the TOCTOU window. */
    if (!copy_from_user_checked(&val, uaddr, sizeof(val))) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_INVALID_ARG;
    }
    if (val != expected) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_WOULD_BLOCK;
    }

    int slot = -1;
    for (int i = 0; i < (int)FUTEX_BUCKET_CAP; i++) {
        if (bucket->entries[i].uaddr == 0) { slot = i; break; }
    }
    if (slot < 0) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_TABLE_FULL;
    }
    if (!t->vspace) {
        irq_spinlock_unlock(&bucket->lock, saved);
        return IRIS_ERR_INVALID_ARG;
    }

    bucket->entries[slot].uaddr  = uaddr;
    bucket->entries[slot].waiter = t;
    bucket->entries[slot].owner  = t->vspace;
    t->state     = TASK_BLOCKED_IPC;
    t->wake_tick = deadline_ticks;
    t->timed_out = 0;
    irq_spinlock_unlock(&bucket->lock, saved);
    return IRIS_ERR_BUSY;      /* parked: re-execute me */
}

uint32_t futex_wake(uint64_t uaddr, uint32_t count) {
    struct task *t = task_current();
    if (!t || !t->vspace) return 0;
    struct KVSpace *caller_vs = t->vspace;
    uint32_t woken = 0;

    struct futex_bucket *bucket = &futex_buckets[futex_hash(uaddr)];
    uint64_t saved = irq_spinlock_lock(&bucket->lock);

    for (int i = 0; i < (int)FUTEX_BUCKET_CAP && woken < count; i++) {
        if (bucket->entries[i].uaddr != uaddr || !bucket->entries[i].waiter)
            continue;
        if (bucket->entries[i].owner != caller_vs)
            continue;
        struct task *w = bucket->entries[i].waiter;
        bucket->entries[i].uaddr  = 0;
        bucket->entries[i].waiter = 0;
        bucket->entries[i].owner  = 0;
        if (w->state == TASK_BLOCKED_IPC)
            task_wakeup(w);
        woken++;
    }

    irq_spinlock_unlock(&bucket->lock, saved);
    return woken;
}

void futex_cancel_waiter(struct task *t) {
    if (!t) return;
    for (uint32_t b = 0; b < FUTEX_BUCKETS; b++) {
        struct futex_bucket *bucket = &futex_buckets[b];
        uint64_t saved = irq_spinlock_lock(&bucket->lock);
        for (uint32_t i = 0; i < FUTEX_BUCKET_CAP; i++) {
            if (bucket->entries[i].waiter == t) {
                bucket->entries[i].uaddr  = 0;
                bucket->entries[i].waiter = 0;
                bucket->entries[i].owner  = 0;
            }
        }
        irq_spinlock_unlock(&bucket->lock, saved);
    }
}
