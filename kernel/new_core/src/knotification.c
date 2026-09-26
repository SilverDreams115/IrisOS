/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/nc/knotification.h>
#include <iris/nc/kendpoint.h>
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
    /*
     * Ledger A-44 — a wait queue holds what it names, here as on the endpoint.
     *
     * The two early returns above are deliberately NOT counted: a task already
     * on this queue is already held, and a second reference would never be
     * given back.
     */
    kobject_retain(&t->base);
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
        kobject_release(&w->base);   /* A-44: the queue's */
        return;
    }
}

/* Wake the first blocked waiter; remove it from the queue.  1 if one was
 * woken, 0 if nobody was waiting — which is what decides whether the BOUND
 * thread (A-23) gets the signal instead. */
static int knotif_waiters_wake_one(struct KNotification *n) {
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
        kobject_release(&w->base);   /* A-44: the queue's */
        return 1;
    }
    return 0;
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
        /* A-44: every waiter left the queue above, woken or not. */
        kobject_release(&w->base);
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
    /* A-23: and the same for a bound THREAD.  The bind holds an active+
     * lifecycle pair on this object, so `close` firing means the last OTHER
     * capability went; breaking the binding here is what lets the object
     * actually die instead of being kept alive by its own thread. */
    knotification_unbind(n);
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
    int woke = knotif_waiters_wake_one(n);
    struct task *bound = woke ? 0 : n->bound_tcb;
    spinlock_unlock(&n->base.lock);

    /*
     * Ledger A-23 — nobody was WAITING, so the bound thread gets it.
     *
     * Outside the notification's lock on purpose: delivering takes the
     * endpoint's lock to dequeue the thread, and taking the two in this order
     * here while some other path takes them in the other order is how a
     * deadlock is built.  Nothing between the unlock and the delivery can
     * invalidate it — the bits are already set, so a thread that consumes them
     * first simply makes the delivery a no-op.
     */
    if (bound) {
        uint64_t got = atomic_exchange_explicit(&n->signal_bits, 0,
                                                memory_order_acq_rel);
        if (got != 0 && !kendpoint_deliver_notification(bound, got)) {
            /* It was not blocked on an endpoint after all: put the bits back
             * for whoever does come to wait, rather than swallowing them. */
            atomic_fetch_or_explicit(&n->signal_bits, got, memory_order_release);
        }
    }
}

/* A-23: take whatever is pending, atomically.  Used on the way into an
 * endpoint receive by a thread with a bound notification. */
uint64_t knotification_take_pending(struct KNotification *n) {
    if (!n) return 0;
    return atomic_exchange_explicit(&n->signal_bits, 0, memory_order_acq_rel);
}

/*
 * A-23 — bind / unbind.
 *
 * One notification per thread and one thread per notification.  A second bind
 * either way is refused rather than silently replacing, because "which thread
 * does a signal wake" is not a question a system should answer differently
 * depending on the order two supervisors happened to make their calls.
 */
iris_error_t knotification_bind(struct KNotification *n, struct task *t) {
    if (!n || !t) return IRIS_ERR_INVALID_ARG;
    iris_error_t r = IRIS_OK;
    spinlock_lock(&n->base.lock);
    if (n->closed)                            r = IRIS_ERR_CLOSED;
    else if (n->bound_tcb && n->bound_tcb != t) r = IRIS_ERR_ALREADY_EXISTS;
    else if (t->bound_notif && t->bound_notif != n) r = IRIS_ERR_ALREADY_EXISTS;
    else if (n->bound_tcb == t)               r = IRIS_ERR_ALREADY_EXISTS;
    else {
        /*
         * A LIFECYCLE reference, not an active one.
         *
         * An active reference is what keeps an object OPEN, and taking one
         * here would mean a notification could never be closed while a thread
         * was bound to it — the binding would keep alive the very thing it
         * points at, forever.  The lifecycle reference is enough: it keeps the
         * storage valid, `close` still fires when the last capability goes,
         * and `close` breaks the binding.
         */
        kobject_retain(&n->base);
        n->bound_tcb  = t;
        t->bound_notif = n;
    }
    spinlock_unlock(&n->base.lock);
    return r;
}

void knotification_unbind(struct KNotification *n) {
    if (!n) return;
    struct task *t;
    spinlock_lock(&n->base.lock);
    t = n->bound_tcb;
    n->bound_tcb = 0;
    if (t && t->bound_notif == n) t->bound_notif = 0;
    spinlock_unlock(&n->base.lock);
    if (t) kobject_release(&n->base);
}

/* The thread side of the same break, for teardown. */
void knotification_unbind_task(struct task *t) {
    if (!t || !t->bound_notif) return;
    knotification_unbind(t->bound_notif);
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



uint64_t knotification_poll(struct KNotification *n) {
    uint64_t bits = atomic_load_explicit(&n->signal_bits, memory_order_acquire);
    if (bits == 0) return 0;
    return atomic_exchange_explicit(&n->signal_bits, 0, memory_order_acq_rel);
}
