/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/nc/ktcb.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kobject_dispatch.h>
#include <iris/nc/knotification.h>
#include <iris/panic.h>
#include <iris/nc/kuntyped.h>
#include <iris/task.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * D2 — KTCB object identity ops.
 *
 * The KTCB IS `struct task` (KObject at offset 0).  These ops manage the
 * OBJECT lifetime only; execution teardown (queues, resources, registry) is
 * done by task_lifecycle.c before the scheduler drops its execution reference.
 * The destructor runs when the last reference (execution + all caps) drops.
 */

static _Atomic uint32_t ktcb_live;       /* objects that exist (any lifetime) */
static _Atomic uint32_t ktcb_hwm;
static _Atomic uint32_t ktcb_retyped;    /* objects created */
static _Atomic uint32_t ktcb_destroyed;  /* final destructors run */

uint32_t ktcb_live_count(void) {
    return atomic_load_explicit(&ktcb_live, memory_order_relaxed);
}

void ktcb_stats(uint32_t *live, uint32_t *hwm,
                uint32_t *retyped, uint32_t *destroyed) {
    if (live)      *live      = atomic_load_explicit(&ktcb_live,      memory_order_relaxed);
    if (hwm)       *hwm       = atomic_load_explicit(&ktcb_hwm,       memory_order_relaxed);
    if (retyped)   *retyped   = atomic_load_explicit(&ktcb_retyped,   memory_order_relaxed);
    if (destroyed) *destroyed = atomic_load_explicit(&ktcb_destroyed, memory_order_relaxed);
}

static void ktcb_live_inc(void) {
    uint32_t n = atomic_fetch_add_explicit(&ktcb_live, 1u, memory_order_relaxed) + 1u;
    atomic_fetch_add_explicit(&ktcb_retyped, 1u, memory_order_relaxed);
    uint32_t hw = atomic_load_explicit(&ktcb_hwm, memory_order_relaxed);
    /*
     * BOUND: monotonicity, not a count.  The high-water mark only rises, `n` is
     * fixed, and a failed compare-exchange reloads `hw`, so every retry either
     * raises `hw` or makes `n > hw` false.  Concurrent writers can only end it
     * sooner.
     */
    while (n > hw &&
           !atomic_compare_exchange_weak_explicit(&ktcb_hwm, &hw, n,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
}

/* Final destructor — implemented in task_lifecycle.c (it owns the backing
 * pool + registry + resource-freeing).  Frees the backing slot for reuse. */
void task_backing_free_on_destroy(struct task *t);

/*
 * A TCB has two possible storages and one destructor.
 *
 * Pool birth: the task IS a slot of the static backing array, returned to it.
 * Untyped birth: the storage IS the retyped region, returned to its parent
 * (child_count--, parent ref released).  Nothing else to unwind in that case —
 * an unconfigured TCB never held a kstack, a registry slot, a process link or
 * a sched_ctx (SC_BIND refuses unconfigured targets precisely so no reference
 * can leak here).
 */
/*
 * How many threads reached their own destruction while still on a
 * notification's waiter queue.  Must stay zero (D-7).
 *
 * The queue holds a REFERENCE today, so this cannot happen — the count is
 * measuring whether the reference is REDUNDANT, which is what removing it
 * needs.  The argument for redundancy: a thread that could be queued holds an
 * execution reference on itself, only teardown releases it, and teardown calls
 * `task_cancel_blocked_waits` before that.  An argument is not a measurement,
 * and this tree has been wrong about an ordering it had argued before.
 */
static _Atomic uint32_t ktcb_destroyed_while_queued;

uint32_t ktcb_destroyed_while_queued_count(void) {
    return atomic_load_explicit(&ktcb_destroyed_while_queued,
                                memory_order_relaxed);
}

void ktcb_obj_destroy(struct KObject *obj) {
    struct task *t = (struct task *)obj;   /* KObject at offset 0 */
    /*
     * A thread must not reach here while a notification queue still names it.
     *
     * This COUNTED while the queue held a reference, because the reference made
     * the case impossible and the count was measuring whether it was
     * redundant.  The reference is gone (D-7, class 1), so the count would be
     * a report filed after a dangling pointer: the queue would be holding
     * storage this function is about to hand back to its Untyped.  A panic
     * before that happens is the only honest replacement.
     *
     * It stays zero for the reason the enqueue comment gives: a queued thread
     * holds an execution reference on itself, and teardown cancels its queue
     * membership before releasing it.
     */
    if (t->notif_queued) {
        atomic_fetch_add_explicit(&ktcb_destroyed_while_queued, 1u,
                                  memory_order_relaxed);
        iris_panic("ktcb_obj_destroy: thread still on a notification queue");
    }
    atomic_fetch_sub_explicit(&ktcb_live, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&ktcb_destroyed, 1u, memory_order_relaxed);
    if (obj->ut_block_bytes) kobject_storage_free(obj, 0u, 0);
    else                     task_backing_free_on_destroy(t);
}



/*
 * Wire up the thread's own capability slots (ledger D-7).
 *
 * The header is embedded in the task, so this only has to make it a
 * well-formed CNode: a KObject the MDB can take a lock on, a slot count, and
 * the inline array.  It is never named by a capability, so its one reference
 * is never released and `kcnode_obj_close`/`destroy` never run on it — the
 * storage goes back when the thread's does.
 */
void ktcb_caps_init(struct task *t) {
    kobject_init(&t->caps_hdr.base, KOBJ_CNODE);
    irq_spinlock_init(&t->caps_hdr.lock);
    t->caps_hdr.slot_count = TCB_CAP_SLOTS;
    t->caps_hdr.slots      = t->caps;
    for (uint32_t i = 0; i < TCB_CAP_SLOTS; i++) {
        t->caps[i].object = 0;
        t->caps[i].rights = RIGHT_NONE;
        t->caps[i].badge  = 0;
        t->caps[i].mdb_parent = 0;
        t->caps[i].mdb_first_child = 0;
        t->caps[i].mdb_next_sib = 0;
        t->caps[i].mdb_prev_sib = 0;
        t->caps[i].mdb_cnode = 0;
        t->caps[i].mdb_flags = 0;
        t->caps[i].guard = 0;
        t->caps[i].guard_bits = 0;
    }
}

void ktcb_object_init(struct task *t) {
    if (!t) return;
    kobject_init(&t->base, KOBJ_TCB);  /* refcount = 1 (execution ref) */
    irq_spinlock_init(&t->obj_lock);
    ktcb_caps_init(t);
    t->configured = 1;   /* pool birth: execution state built by the creator */
    t->started    = 1;   /* ...and already runnable: its entry frame is set */
    ktcb_live_inc();
}

/* ── Untyped-born (canonical) KTCB ───────────────────── */

struct task *ktcb_alloc_at(void *mem) {
    struct task *t = (struct task *)mem;
    if (!t) return 0;
    /* Block arrives zero-filled from kuntyped_alloc_children_atomic; set only
     * the non-zero identity fields.  NOTE: TASK_READY == 0, so the state MUST
     * be set explicitly — an inactive TCB is never runnable. */
    kobject_init_in_untyped(&t->base, KOBJ_TCB,
                            (uint32_t)sizeof(struct task));  /* refcount = 1 (creator) */
    irq_spinlock_init(&t->obj_lock);
    ktcb_caps_init(t);
    t->state      = TASK_SUSPENDED;         /* inactive until TCB_CONFIGURE */
    t->ring       = TASK_RING3;
    t->priority   = TASK_PRIORITY_DEFAULT;
    /* A retyped TCB grants nothing until it is configured; ktcb_configure
     * gives it the ceiling of whoever configured it. */
    t->mcp        = 0;
    t->reg_slot   = -1;                     /* no scheduler identity */
    t->configured = 0;                      /* execution gate: stays closed */
    atomic_store_explicit(&t->configuring, 0u, memory_order_relaxed);
    t->started    = 0;                      /* never runnable yet */

    ktcb_live_inc();
    return t;
}
