#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/kpage.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

static struct KNotification *live_head = 0;
static spinlock_t            live_lock;
static _Atomic uint32_t      knotif_live;

static void knotif_live_link(struct KNotification *n) {
    spinlock_lock(&live_lock);
    n->live_next = live_head;
    n->live_prev = 0;
    if (live_head) live_head->live_prev = n;
    live_head = n;
    spinlock_unlock(&live_lock);
}

static void knotif_live_unlink(struct KNotification *n) {
    spinlock_lock(&live_lock);
    if (n->live_prev) n->live_prev->live_next = n->live_next;
    else              live_head = n->live_next;
    if (n->live_next) n->live_next->live_prev = n->live_prev;
    n->live_prev = 0;
    n->live_next = 0;
    spinlock_unlock(&live_lock);
}

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

static void knotification_owner_release(struct KNotification *n) {
    struct KProcess *owner;
    if (!n) return;

    spinlock_lock(&n->base.lock);
    owner = n->owner;
    n->owner = 0;
    spinlock_unlock(&n->base.lock);

    if (!owner) return;
    kprocess_quota_release_notification(owner);
    kobject_release(&owner->base);
}

static void knotification_destroy(struct KObject *obj) {
    knotification_close(obj);
    struct KNotification *n = (struct KNotification *)obj;
    knotification_owner_release(n);
    knotif_live_unlink(n);
    atomic_fetch_sub_explicit(&knotif_live, 1u, memory_order_relaxed);
    kpage_free(n, (uint32_t)sizeof(struct KNotification));
}

static const struct KObjectOps knotification_ops = {
    .close = knotification_close,
    .destroy = knotification_destroy
};

/* ── Public API ───────────────────────────────────────────────── */

struct KNotification *knotification_alloc(void) {
    struct KNotification *n = kpage_alloc((uint32_t)sizeof(struct KNotification));
    if (!n) return 0;
    kobject_init(&n->base, KOBJ_NOTIFICATION, &knotification_ops);
    atomic_store_explicit(&n->signal_bits, 0, memory_order_relaxed);
    knotif_waiters_clear(n);
    knotif_live_link(n);
    atomic_fetch_add_explicit(&knotif_live, 1u, memory_order_relaxed);
    return n;
}

iris_error_t knotification_bind_owner(struct KNotification *n, struct KProcess *owner) {
    iris_error_t r;
    if (!n || !owner) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&n->base.lock);
    if (n->owner) {
        r = (n->owner == owner) ? IRIS_OK : IRIS_ERR_BUSY;
        spinlock_unlock(&n->base.lock);
        return r;
    }
    spinlock_unlock(&n->base.lock);

    r = kprocess_quota_acquire_notification(owner);
    if (r != IRIS_OK) return r;
    kobject_retain(&owner->base);

    spinlock_lock(&n->base.lock);
    if (n->owner) {
        spinlock_unlock(&n->base.lock);
        kobject_release(&owner->base);
        kprocess_quota_release_notification(owner);
        return (n->owner == owner) ? IRIS_OK : IRIS_ERR_BUSY;
    }
    n->owner = owner;
    spinlock_unlock(&n->base.lock);
    return IRIS_OK;
}

void knotification_free(struct KNotification *n) {
    kobject_release(&n->base);
}

void knotification_cancel_waiter(struct task *t) {
    if (!t) return;
    spinlock_lock(&live_lock);
    struct KNotification *n = live_head;
    while (n) {
        struct KNotification *next = n->live_next;
        spinlock_lock(&n->base.lock);
        knotif_waiters_remove(n, t);
        spinlock_unlock(&n->base.lock);
        n = next;
    }
    spinlock_unlock(&live_lock);
}

uint32_t knotification_live_count(void) {
    return atomic_load_explicit(&knotif_live, memory_order_relaxed);
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
