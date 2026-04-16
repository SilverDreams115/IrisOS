#include <iris/nc/knotification.h>
#include <iris/task.h>
#include <stdint.h>

static struct KNotification pool[KNOTIF_POOL_SIZE];
static uint8_t              pool_used[KNOTIF_POOL_SIZE];

/* ── waiter array helpers ─────────────────────────────────────── */

static void knotif_waiters_clear(struct KNotification *n) {
    for (uint32_t i = 0; i < KNOTIF_WAITERS_MAX; i++) n->waiters[i] = 0;
    n->waiter_count = 0;
}

static iris_error_t knotif_waiters_enqueue(struct KNotification *n, struct task *t) {
    for (uint32_t i = 0; i < KNOTIF_WAITERS_MAX; i++) {
        if (n->waiters[i] == t) return IRIS_OK; /* already queued */
    }
    for (uint32_t i = 0; i < KNOTIF_WAITERS_MAX; i++) {
        if (!n->waiters[i]) {
            n->waiters[i] = t;
            n->waiter_count++;
            return IRIS_OK;
        }
    }
    return IRIS_ERR_BUSY; /* table full */
}

static void knotif_waiters_remove(struct KNotification *n, struct task *t) {
    for (uint32_t i = 0; i < KNOTIF_WAITERS_MAX; i++) {
        if (n->waiters[i] != t) continue;
        n->waiters[i] = 0;
        if (n->waiter_count) n->waiter_count--;
        return;
    }
}

/* Wake the first blocked waiter; remove it from the array. */
static void knotif_waiters_wake_one(struct KNotification *n) {
    for (uint32_t i = 0; i < KNOTIF_WAITERS_MAX; i++) {
        struct task *w = n->waiters[i];
        if (!w) continue;
        if (w->state == TASK_BLOCKED_IRQ) {
            w->state    = TASK_READY;
            n->waiters[i] = 0;
            if (n->waiter_count) n->waiter_count--;
            return;
        }
    }
}

/* Wake all blocked waiters; clear the array. Used on close. */
static void knotif_waiters_wake_all(struct KNotification *n) {
    for (uint32_t i = 0; i < KNOTIF_WAITERS_MAX; i++) {
        struct task *w = n->waiters[i];
        if (w && w->state == TASK_BLOCKED_IRQ)
            w->state = TASK_READY;
        n->waiters[i] = 0;
    }
    n->waiter_count = 0;
}

/* ── KObject ops ──────────────────────────────────────────────── */

static void knotification_close(struct KObject *obj) {
    struct KNotification *n = (struct KNotification *)obj;
    spinlock_lock(&n->base.lock);
    n->closed = 1;
    knotif_waiters_wake_all(n);
    spinlock_unlock(&n->base.lock);
}

static void knotification_destroy(struct KObject *obj) {
    knotification_close(obj);
    struct KNotification *n = (struct KNotification *)obj;
    for (int i = 0; i < KNOTIF_POOL_SIZE; i++) {
        if (&pool[i] == n) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps knotification_ops = {
    .close = knotification_close,
    .destroy = knotification_destroy
};

/* ── Public API ───────────────────────────────────────────────── */

struct KNotification *knotification_alloc(void) {
    for (int i = 0; i < KNOTIF_POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KNotification *n = &pool[i];
            uint8_t *p = (uint8_t *)n;
            for (uint32_t j = 0; j < sizeof(*n); j++) p[j] = 0;
            kobject_init(&n->base, KOBJ_NOTIFICATION, &knotification_ops);
            atomic_store_explicit(&n->signal_bits, 0, memory_order_relaxed);
            knotif_waiters_clear(n);
            return n;
        }
    }
    return 0;
}

void knotification_free(struct KNotification *n) {
    kobject_release(&n->base);
}

void knotification_cancel_waiter(struct task *t) {
    if (!t) return;
    for (int i = 0; i < KNOTIF_POOL_SIZE; i++) {
        if (!pool_used[i]) continue;
        struct KNotification *n = &pool[i];
        spinlock_lock(&n->base.lock);
        knotif_waiters_remove(n, t);
        spinlock_unlock(&n->base.lock);
    }
}

uint32_t knotification_live_count(void) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < KNOTIF_POOL_SIZE; i++) {
        if (pool_used[i]) live++;
    }
    return live;
}

/*
 * Signal: set bits and wake one blocked waiter.
 * Safe from IRQ context.
 */
void knotification_signal(struct KNotification *n, uint64_t bits) {
    spinlock_lock(&n->base.lock);
    if (n->closed) {
        spinlock_unlock(&n->base.lock);
        return;
    }
    atomic_fetch_or_explicit(&n->signal_bits, bits, memory_order_release);
    knotif_waiters_wake_one(n);
    spinlock_unlock(&n->base.lock);
}

/*
 * Wait: block until signal_bits != 0. Returns all pending bits atomically.
 * Up to KNOTIF_WAITERS_MAX tasks may block concurrently.
 */
iris_error_t knotification_wait(struct KNotification *n, uint64_t *out_bits) {
    for (;;) {
        uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
        if (bits != 0) {
            uint64_t got = atomic_exchange_explicit(&n->signal_bits, 0,
                                                    memory_order_acq_rel);
            if (got != 0) {
                *out_bits = got;
                return IRIS_OK;
            }
        }

        spinlock_lock(&n->base.lock);
        bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
        if (bits == 0 && n->closed) {
            spinlock_unlock(&n->base.lock);
            return IRIS_ERR_CLOSED;
        }
        if (bits != 0) {
            spinlock_unlock(&n->base.lock);
            continue;
        }
        struct task *t = task_current();
        if (t) {
            iris_error_t r = knotif_waiters_enqueue(n, t);
            if (r != IRIS_OK) {
                spinlock_unlock(&n->base.lock);
                return r; /* IRIS_ERR_BUSY: waiter table full */
            }
            t->state = TASK_BLOCKED_IRQ;
        }
        spinlock_unlock(&n->base.lock);
        task_yield();
        /* Resumed by knotification_signal() or knotification_close().
         * Remove self from waiters in case close woke us without removing. */
        if (t) {
            spinlock_lock(&n->base.lock);
            knotif_waiters_remove(n, t);
            spinlock_unlock(&n->base.lock);
        }
    }
}

uint64_t knotification_poll(struct KNotification *n) {
    uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
    if (bits == 0) return 0;
    return atomic_exchange_explicit(&n->signal_bits, 0, memory_order_acq_rel);
}
