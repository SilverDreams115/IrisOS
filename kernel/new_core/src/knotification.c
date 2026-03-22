#include <iris/nc/knotification.h>
#include <iris/task.h>
#include <stdint.h>

#define POOL_SIZE 32
static struct KNotification pool[POOL_SIZE];
static uint8_t              pool_used[POOL_SIZE];

static void knotification_close(struct KObject *obj) {
    struct KNotification *n = (struct KNotification *)obj;
    spinlock_lock(&n->base.lock);
    n->closed = 1;
    if (n->waiter && n->waiter->state == TASK_BLOCKED_IRQ) {
        n->waiter->state = TASK_READY;
        n->waiter = 0;
    }
    spinlock_unlock(&n->base.lock);
}

static void knotification_destroy(struct KObject *obj) {
    knotification_close(obj);
    struct KNotification *n = (struct KNotification *)obj;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (&pool[i] == n) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps knotification_ops = {
    .close = knotification_close,
    .destroy = knotification_destroy
};

struct KNotification *knotification_alloc(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KNotification *n = &pool[i];
            /* zero all fields */
            uint8_t *p = (uint8_t *)n;
            for (uint32_t j = 0; j < sizeof(*n); j++) p[j] = 0;
            kobject_init(&n->base, KOBJ_NOTIFICATION, &knotification_ops);
            atomic_store_explicit(&n->signal_bits, 0, memory_order_relaxed);
            n->waiter = 0;
            return n;
        }
    }
    return 0;
}

void knotification_free(struct KNotification *n) {
    kobject_release(&n->base);
}

/*
 * Signal: set bits and wake any blocked waiter.
 * Safe from IRQ context — uses spinlock only briefly.
 */
void knotification_signal(struct KNotification *n, uint64_t bits) {
    spinlock_lock(&n->base.lock);
    if (n->closed) {
        spinlock_unlock(&n->base.lock);
        return;
    }
    /* Atomically OR the bits in first — a racing wait will see them
     * even if it runs before we release the lock. */
    atomic_fetch_or_explicit(&n->signal_bits, bits, memory_order_release);
    struct task *w = n->waiter;
    if (w && w->state == TASK_BLOCKED_IRQ) {
        w->state  = TASK_READY;
        n->waiter = 0;
    }
    spinlock_unlock(&n->base.lock);
}

/*
 * Wait: block until signal_bits != 0. Returns all pending bits atomically.
 */
iris_error_t knotification_wait(struct KNotification *n, uint64_t *out_bits) {
    for (;;) {
        /* Fast path: bits already set — no need to block. */
        uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
        if (bits != 0) {
            /* Atomically clear and return them. */
            uint64_t got = atomic_exchange_explicit(&n->signal_bits, 0,
                                                    memory_order_acq_rel);
            if (got != 0) {
                *out_bits = got;
                return IRIS_OK;
            }
            /* Rare: another thread cleared bits between our load and exchange.
             * Fall through to blocking path. */
        }

        spinlock_lock(&n->base.lock);
        /* Re-check under lock to avoid lost wakeup. */
        bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
        if (bits == 0 && n->closed) {
            spinlock_unlock(&n->base.lock);
            return IRIS_ERR_CLOSED;
        }
        if (bits != 0) {
            spinlock_unlock(&n->base.lock);
            continue; /* retry fast path */
        }
        struct task *t = task_current();
        if (t) {
            if (n->waiter && n->waiter != t) {
                /* Single-waiter policy: do not silently lose the first waiter. */
                spinlock_unlock(&n->base.lock);
                return IRIS_ERR_BUSY;
            }
            n->waiter = t;
            t->state  = TASK_BLOCKED_IRQ;
        }
        spinlock_unlock(&n->base.lock);
        task_yield();
        /* Resumed by knotification_signal(). Retry. */
    }
}

uint64_t knotification_poll(struct KNotification *n) {
    uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
    if (bits == 0) return 0;
    return atomic_exchange_explicit(&n->signal_bits, 0, memory_order_acq_rel);
}
