#include <iris/nc/knotification.h>
#include <iris/irq_routing.h>
#include <iris/nc/kuntyped.h>
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

/* ── waiter queue helpers — intrusive through the TCB, no ceiling ─── */

static iris_error_t knotif_waiters_enqueue(struct KNotification *n, struct task *t) {
    if (t->notif_next || n->queue_tail == t) return IRIS_OK;  /* already queued */
    for (struct task *w = n->queue_head; w; w = w->notif_next)
        if (w == t) return IRIS_OK;
    t->notif_next = 0;
    if (n->queue_tail) n->queue_tail->notif_next = t;
    else               n->queue_head = t;
    n->queue_tail = t;
    n->waiter_count++;
    return IRIS_OK;
}

static void knotif_waiters_remove(struct KNotification *n, struct task *t) {
    struct task *prev = 0;
    for (struct task *w = n->queue_head; w; prev = w, w = w->notif_next) {
        if (w != t) continue;
        if (prev) prev->notif_next = w->notif_next;
        else      n->queue_head    = w->notif_next;
        if (n->queue_tail == w) n->queue_tail = prev;
        w->notif_next = 0;
        if (n->waiter_count) n->waiter_count--;
        return;
    }
}

/* Wake the first blocked waiter; remove it from the queue. */
static void knotif_waiters_wake_one(struct KNotification *n) {
    struct task *prev = 0;
    for (struct task *w = n->queue_head; w; prev = w, w = w->notif_next) {
        if (w->state != TASK_BLOCKED_IRQ) continue;
        struct task *next = w->notif_next;
        if (prev) prev->notif_next = next;
        else      n->queue_head    = next;
        if (n->queue_tail == w) n->queue_tail = prev;
        w->notif_next = 0;
        if (n->waiter_count) n->waiter_count--;
        task_wakeup(w);
        return;
    }
}

/* Wake every blocked waiter and empty the queue.  Used on close. */
static void knotif_waiters_wake_all(struct KNotification *n) {
    struct task *w = n->queue_head;
    n->queue_head = 0;
    n->queue_tail = 0;
    n->waiter_count = 0;
    while (w) {
        struct task *next = w->notif_next;
        w->notif_next = 0;
        if (w->state == TASK_BLOCKED_IRQ) {
            /*
             * Stage 9-evt Step 1: tell the waiter WHY it woke.
             *
             * A restartable wait re-executes the syscall, which re-resolves the
             * capability — and the reason this wake happened is usually that
             * the last capability was just deleted, so the re-resolution would
             * fail with NOT_FOUND and the caller would learn "no such slot"
             * instead of "the thing you were waiting on closed".  The parked
             * form could tell them apart because it never let go of the
             * object; the restartable one needs the fact recorded on the
             * thread.  Same marker and same reason as the endpoint path.
             */
            w->ipc_ep_closed = 1u;
            task_wakeup(w);
        }
        w = next;
    }
}

/* ── KObject ops ──────────────────────────────────────────────── */

static void knotification_close(struct KObject *obj) {
    struct KNotification *n = (struct KNotification *)obj;
    spinlock_lock(&n->base.lock);
    n->closed = 1;
    knotif_waiters_wake_all(n);
    spinlock_unlock(&n->base.lock);
    /* Stage 7-mem: and any interrupt bound to it stops being delivered.  The
     * binding is the notification's, so the last capability to it going is
     * what unbinds — seL4's rule, and the reason an IRQ route needs no owner. */
    irq_routing_unregister_notification(n);
}

/*
 * Phase S1: the kslab-backed variant and the per-process owner/quota binding
 * are RETIRED.  A KNotification is created only via Untyped retype; the
 * authority to create one is possession of sufficient KUntyped plus a free
 * CSpace destination slot — never a kernel-side numeric quota.
 */
static void knotification_destroy_ut(struct KObject *obj) {
    struct KNotification *n = (struct KNotification *)obj;
    knotif_live_unlink(n);
    atomic_fetch_sub_explicit(&knotif_live, 1u, memory_order_relaxed);
    kuntyped_release_child(obj, sizeof(struct KNotification));
}

static const struct KObjectOps knotification_ops_ut = {
    .close   = knotification_close,
    .destroy = knotification_destroy_ut,
};

/* ── Public API ───────────────────────────────────────────────── */

struct KNotification *knotification_alloc_at(void *mem) {
    if (!mem) return 0;
    struct KNotification *n = (struct KNotification *)mem;
    /* mem was already zeroed by the untyped carve */
    kobject_init(&n->base, KOBJ_NOTIFICATION, &knotification_ops_ut);
    knotif_live_link(n);
    atomic_fetch_add_explicit(&knotif_live, 1u, memory_order_relaxed);
    return n;
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
        /*
         * Stage 9-evt step 3: WAIT, do not yield.
         *
         * This is a KERNEL-internal wait — its only caller is the boot
         * selftest, running on the boot thread before any other exists — and
         * it used to `task_yield()`, which is how a kernel path handed the CPU
         * away through its own frame.  There is no frame to hand away to any
         * more: kernel stacks belong to the core.  With nothing else runnable
         * the honest wait is the one the dispatcher does when idle, and the
         * loop re-checks the condition on every wake, which is what makes a
         * spurious interrupt harmless.
         *
         * `sti` takes effect after the NEXT instruction, so the pair cannot
         * race: an interrupt arriving between them is taken after the hlt is
         * entered, never before it.
         */
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
        /* Resumed by knotification_signal() or knotification_close().
         * Remove self from waiters in case close woke us without removing. */
        if (t) {
            spinlock_lock(&n->base.lock);
            knotif_waiters_remove(n, t);
            spinlock_unlock(&n->base.lock);
        }
    }
}

/*
 * Stage 9-evt Step 1 — one non-blocking attempt, then park.
 *
 * This is knotification_wait's loop body with the loop and the task_yield
 * removed: the retry is the DISPATCHER re-executing the syscall, and the
 * position in the waiter list is the continuation.  Nothing is held across the
 * block, which is what makes the caller's stack frame disposable.
 *
 * A signal that arrives between the enqueue here and the reschedule is not
 * lost and does not need to be: knotification_signal sets pending bits on the
 * OBJECT and wakes the waiter, so a re-execution sees the bits.  The restart
 * model is if anything more robust than the parked one here, because there is
 * no window in which the handler holds a decision it made before sleeping.
 */
iris_error_t knotification_wait_step(struct KNotification *n, uint64_t *out_bits) {
    struct task *t = task_current();

    /* Leave the list before deciding anything: a previous park may have left
     * us on it, and a stale registration would take a signal meant for the
     * attempt we are about to make. */
    if (t) {
        spinlock_lock(&n->base.lock);
        knotif_waiters_remove(n, t);
        spinlock_unlock(&n->base.lock);
    }

    uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
    if (bits != 0) {
        uint64_t got = atomic_exchange_explicit(&n->signal_bits, 0,
                                                memory_order_acq_rel);
        if (got != 0) { *out_bits = got; return IRIS_OK; }
    }

    spinlock_lock(&n->base.lock);
    bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
    if (bits != 0) {
        spinlock_unlock(&n->base.lock);
        return IRIS_ERR_WOULD_BLOCK;   /* re-execute; the fast path will take it */
    }
    if (n->closed) {
        spinlock_unlock(&n->base.lock);
        return IRIS_ERR_CLOSED;
    }
    if (t) {
        iris_error_t r = knotif_waiters_enqueue(n, t);
        if (r != IRIS_OK) {
            spinlock_unlock(&n->base.lock);
            return r;                  /* waiter table full */
        }
        t->state = TASK_BLOCKED_IRQ;
    }
    spinlock_unlock(&n->base.lock);
    return IRIS_ERR_WOULD_BLOCK;
}

/*
 * Stage 9-evt Step 1 — one attempt of a timed wait, then park.
 *
 * knotification_wait_step plus a deadline.  The deadline is armed on the FIRST
 * attempt only: re-arming it on every re-execution would make the timeout
 * restart with the thread and never expire, which is the same trap SYS_SLEEP
 * fell into when its continuation was a duration instead of an instant.
 */
iris_error_t knotification_wait_timeout_step(struct KNotification *n,
                                             uint64_t *out_bits,
                                             uint64_t deadline_ticks,
                                             int first) {
    struct task *t = task_current();

    if (t && t->timed_out) {
        t->timed_out = 0;
        t->wake_tick = 0;
        spinlock_lock(&n->base.lock);
        knotif_waiters_remove(n, t);
        spinlock_unlock(&n->base.lock);
        return IRIS_ERR_TIMED_OUT;
    }

    iris_error_t r = knotification_wait_step(n, out_bits);
    if (r != IRIS_ERR_WOULD_BLOCK) {
        if (t) { t->wake_tick = 0; t->timed_out = 0; }
        return r;
    }

    /* Parked by the step above; arm the deadline the first time only. */
    if (t && first) {
        t->wake_tick = deadline_ticks;
        t->timed_out = 0;
    }
    return IRIS_ERR_WOULD_BLOCK;
}

/*
 * knotification_wait_timeout — blocking wait with tick deadline.
 *
 * Identical to knotification_wait() but returns IRIS_ERR_TIMED_OUT when
 * deadline_ticks (absolute scheduler_ticks value) is reached before any
 * signal arrives.  out_bits is written only on IRIS_OK.
 */
iris_error_t knotification_wait_timeout(struct KNotification *n, uint64_t *out_bits,
                                        uint64_t deadline_ticks) {
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
        if (!t) {
            spinlock_unlock(&n->base.lock);
            return IRIS_ERR_INTERNAL;
        }
        iris_error_t r = knotif_waiters_enqueue(n, t);
        if (r != IRIS_OK) {
            spinlock_unlock(&n->base.lock);
            return r;
        }
        t->state     = TASK_BLOCKED_IRQ;
        t->wake_tick = deadline_ticks;
        t->timed_out = 0;
        spinlock_unlock(&n->base.lock);
        /*
         * Stage 9-evt step 3: WAIT, do not yield.
         *
         * Same as the untimed wait above: a kernel-internal wait, on the
         * boot thread, with nothing else to run.  The deadline is re-checked
         * on every wake.
         *
         * `sti` takes effect after the NEXT instruction, so the pair cannot
         * race: an interrupt arriving between them is taken after the hlt is
         * entered, never before it.
         */
        __asm__ volatile ("sti; hlt; cli" : : : "memory");

        if (t->timed_out) {
            t->timed_out = 0;
            t->wake_tick = 0;
            spinlock_lock(&n->base.lock);
            knotif_waiters_remove(n, t);
            spinlock_unlock(&n->base.lock);
            return IRIS_ERR_TIMED_OUT;
        }

        /* Woken by signal or close; remove self in case close didn't. */
        spinlock_lock(&n->base.lock);
        knotif_waiters_remove(n, t);
        spinlock_unlock(&n->base.lock);
    }
}

uint64_t knotification_poll(struct KNotification *n) {
    uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
    if (bits == 0) return 0;
    return atomic_exchange_explicit(&n->signal_bits, 0, memory_order_acq_rel);
}
