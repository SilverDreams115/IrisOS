#include <iris/nc/kprocess.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/syscall.h>
#include <iris/kslab.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/fault_proto.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic uint32_t kprocess_live;

/* Phase 29 — global resource-accounting instrumentation (additive, exposed via
 * SYS_RESOURCE_INFO).  A charge that hits its domain's limit increments
 * kquota_failed_charges; a provisional charge rolled back on a later failure in
 * the same operation increments kquota_rollbacks.  Both make quota-exhaustion
 * atomicity observable to T246/T250 without changing behaviour. */
static _Atomic uint32_t kquota_failed_charges;
static _Atomic uint32_t kquota_rollbacks;

uint32_t kprocess_quota_failed_count(void)  { return atomic_load_explicit(&kquota_failed_charges, memory_order_relaxed); }
uint32_t kprocess_quota_rollback_count(void){ return atomic_load_explicit(&kquota_rollbacks,      memory_order_relaxed); }
void     kprocess_quota_stat_rollback(void) { atomic_fetch_add_explicit(&kquota_rollbacks, 1u, memory_order_relaxed); }

/* Phase 20 — fault-model instrumentation (additive, exposed via SYS_SCHED_INFO
 * ext5 tier).  Silent; makes fault delivery/resolution observable to the
 * T140–T147 selftests without changing any behaviour.
 *   delivery  — user faults handed to a registered handler (notif signalled).
 *   nohandler — user faults with NO handler (the task is killed by idt.c).
 *   resume    — SYS_EXCEPTION_RESUME action 0 (wake at faulting rip).
 *   kill      — SYS_EXCEPTION_RESUME action 1 (kill the faulted task).
 *   cleanup   — pending-fault records cleared (on resume/kill resolution). */
static _Atomic uint32_t kfault_delivery;
static _Atomic uint32_t kfault_nohandler;
static _Atomic uint32_t kfault_resume;
static _Atomic uint32_t kfault_kill;
static _Atomic uint32_t kfault_cleanup;

uint32_t kprocess_fault_delivery_count(void)  { return atomic_load_explicit(&kfault_delivery,  memory_order_relaxed); }
uint32_t kprocess_fault_nohandler_count(void) { return atomic_load_explicit(&kfault_nohandler, memory_order_relaxed); }
uint32_t kprocess_fault_resume_count(void)    { return atomic_load_explicit(&kfault_resume,    memory_order_relaxed); }
uint32_t kprocess_fault_kill_count(void)      { return atomic_load_explicit(&kfault_kill,      memory_order_relaxed); }
uint32_t kprocess_fault_cleanup_count(void)   { return atomic_load_explicit(&kfault_cleanup,   memory_order_relaxed); }
/* Stage 7 Step 12: the record is the thread's, so thread teardown is what
 * clears it — and this counts the same thing it always did, records actually
 * cleared, from the one place that now does the clearing. */
void kprocess_fault_stat_cleanup(void) {
    atomic_fetch_add_explicit(&kfault_cleanup, 1u, memory_order_relaxed);
}

void kprocess_fault_stat_nohandler(void) { atomic_fetch_add_explicit(&kfault_nohandler, 1u, memory_order_relaxed); }

/*
 * kprocess_fault_clear — drop the pending-fault record for process p if it
 * belongs to task_id.  Phase 20: SYS_EXCEPTION_RESUME calls this so a resolved
 * fault stops being reported by SYS_PROCESS_FAULT_INFO (which must return
 * WOULD_BLOCK when nothing is pending).  `killed` selects the resume/kill
 * counter.  Idempotent — a second call with no matching pending fault is a
 * no-op.
 */
/*
 * Stage 7 Step 15: this took a KProcess, and used it for nothing but its LOCK.
 *
 * The record it clears has been the thread's since Step 6, and the other
 * writer of `fault_valid` — SYS_TCB_SET_FAULT_HANDLER, deciding whether an
 * outstanding fault moves with a re-aimed mailbox — guards it with the
 * THREAD's `obj_lock`.  Two writers of one field under two different locks is
 * not a lock; the process was serialising accesses to something that is not
 * its own.  Same lock as the other writer now, and no process argument left to
 * make it look like a process-scoped operation.
 */
void kfault_resolve(struct task *ft, int killed) {
    if (!ft) return;
    uint64_t irqfl = irq_spinlock_lock(&ft->obj_lock);
    if (ft->fault_valid) {
        ft->fault_valid = 0;
        atomic_fetch_add_explicit(&kfault_cleanup, 1u, memory_order_relaxed);
    }
    irq_spinlock_unlock(&ft->obj_lock, irqfl);
    /* Unconditional, as it has been since Phase 20: this counts RESOLUTIONS —
     * how many times a handler answered — not how many records existed to
     * clear.  A second call for an already-resolved fault is still an answer. */
    atomic_fetch_add_explicit(killed ? &kfault_kill : &kfault_resume, 1u,
                              memory_order_relaxed);
}

static iris_error_t kprocess_quota_acquire(uint32_t *counter, uint32_t *hwm,
                                           uint32_t limit, struct KProcess *p) {
    iris_error_t r = IRIS_OK;
    if (!counter || !p) return IRIS_ERR_INVALID_ARG;
    spinlock_lock(&p->base.lock);
    if (*counter >= limit) {
        r = IRIS_ERR_NO_MEMORY;
    } else {
        (*counter)++;
        if (hwm && *counter > *hwm) *hwm = *counter;   /* Phase 29: high-water */
    }
    spinlock_unlock(&p->base.lock);
    if (r != IRIS_OK)
        atomic_fetch_add_explicit(&kquota_failed_charges, 1u, memory_order_relaxed);
    return r;
}

static void kprocess_quota_release(uint32_t *counter, struct KProcess *p) {
    if (!counter || !p) return;
    spinlock_lock(&p->base.lock);
    if (*counter != 0)
        (*counter)--;
    spinlock_unlock(&p->base.lock);
}

static void kprocess_clear_exit_watch(struct KProcess *p) {
    if (!p) return;
    for (uint32_t i = 0; i < KPROCESS_EXIT_WATCH_MAX; i++) {
        struct KExitWatch *w = &p->exit_watches[i];
        if (!w->armed) continue;
        kobject_release(&w->notif->base);
        w->notif = 0;
        w->armed = 0;
    }
}

static void kprocess_emit_exit_watch(struct KProcess *p) {
    if (!p) return;
    for (uint32_t i = 0; i < KPROCESS_EXIT_WATCH_MAX; i++) {
        struct KExitWatch *w = &p->exit_watches[i];
        if (!w->armed || !w->notif) continue;
        /* Phase 13 (Track B): death is delivered as a KNotification signal —
         * the watcher identifies the dead service by which bit is set and
         * re-queries SYS_PROCESS_EXIT_CODE / STATUS for detail. */
        knotification_signal(w->notif, w->signal_bits);
    }
}

static void kprocess_destroy(struct KObject *obj) {
    struct KProcess *p = (struct KProcess *)obj;

    /* Final refcount drop: finish only idempotent process-owned cleanup.
     * Task-local resources must already be gone before this point.  Both
     * calls claim their own flag on entry, so they are no-ops when the
     * ordinary exit path already ran them — no pre-check needed here. */
    kprocess_teardown(p, 0);
    kprocess_reap_address_space(p);
    atomic_fetch_sub_explicit(&kprocess_live, 1u, memory_order_relaxed);


    /* Storage first (a block holds its own parent retain), pool retain last:
     * the block lives inside the region the pool owns, and it is what records
     * who that pool was. */
    struct KUntyped *pool = p->storage_pool;
    p->storage_pool = 0;
    kobject_storage_free(obj, (uint32_t)sizeof(struct KProcess), 0);
    if (pool) kobject_release(&pool->base);
}

static const struct KObjectOps kprocess_ops = {
    .destroy = kprocess_destroy
};

/*
 * Stage 6 Step 4 — the Untyped-born KProcess.
 *
 * A spawned process's own kernel state — this object and its 256-slot root
 * CNode, together the largest per-process kernel allocation left — is charged
 * to the same budget that already pays for its address space.  What the kernel
 * still funds from its slab is the ROOT TASK, built before any Untyped exists.
 */
struct KProcess *kprocess_alloc_from(struct KUntyped *pool,
                                     struct KCNode *cnode) {
    if (!pool || !cnode) return 0;

    /* Stage 7 Step 3: no reserve-then-roll-back, because there is no ceiling
     * to reserve against.  The gauge is bumped once, at the end, when the
     * object certainly exists — which is also what makes it match the
     * unconditional decrement in kprocess_destroy without a rollback on every
     * failure path. */
    struct KProcess *p = kuntyped_alloc_child_top(pool, sizeof(struct KProcess));
    if (!p) return 0;
    kobject_init_in_untyped(&p->base, KOBJ_PROCESS, &kprocess_ops,
                            (uint32_t)sizeof(struct KProcess));
    p->phys_pages_limit = KPROCESS_PHYS_PAGES_LIMIT;  /* Stage 7: 0 = no kernel ceiling */
    kobject_retain(&pool->base);
    p->storage_pool = pool;


    /*
     * Stage 6-pure Step 5: the root CNode is SUPPLIED, not carved.
     *
     * It was the other half of a process's kernel footprint and the bigger
     * one — 256 slots with their MDB links — carved out of the budget by the
     * kernel at a size the kernel picked.  A spawner retypes it now
     * (RETYPE2 KOBJ_CNODE) and hands it over, which also means it chooses how
     * wide its child's CSpace is.
     *
     * The process takes BOTH references the old carve left it holding: the
     * lifecycle one that keeps the object alive and the active one that keeps
     * its slots reachable.  The caller keeps its own capability; these are the
     * process's.
     */
    kobject_retain(&cnode->base);
    kobject_active_retain(&cnode->base);
    p->cspace_root = cnode;
    atomic_fetch_add_explicit(&kprocess_live, 1u, memory_order_relaxed);
    return p;
}

struct KProcess *kprocess_alloc(void) {
    /* Stage 7 Step 3: the atomic reserve-then-roll-back is gone with the
     * ceiling it enforced.  It existed to close a TOCTOU on a check against
     * KPROCESS_MAX_LIVE; with no check there is nothing to race, and the gauge
     * is bumped once at the end so it needs no unwinding. */
    struct KProcess *p = kslab_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return 0;
    kobject_init(&p->base, KOBJ_PROCESS, &kprocess_ops);
    p->phys_pages_limit = KPROCESS_PHYS_PAGES_LIMIT;  /* Stage 7: 0 = no kernel ceiling */

    /* Ph95: root CNode for hierarchical CSpace.  Soft-fail: if alloc OOMs
     * the process still works, but cspace_root stays NULL.
     *
     * Stage 4: the root is held structurally, not through the handle table.
     * kcnode_alloc returns one lifecycle ref; we add the active ref the handle
     * used to contribute, so the ownership pair is unchanged — only the way it
     * is reached is.  Both refs are dropped in kprocess_teardown. */
    p->cspace_root = kcnode_alloc(KCNODE_DEFAULT_SLOTS);
    if (p->cspace_root)
        kobject_active_retain(&p->cspace_root->base);

    atomic_fetch_add_explicit(&kprocess_live, 1u, memory_order_relaxed);
    return p;
}

void kprocess_free(struct KProcess *p) {
    if (!p) return;
    /* Idempotent: this drops the CREATION reference, and there is exactly one
     * of those however many paths reach here (last thread exits, kill of a
     * never-started process, or a failed create unwinding). */
    if (__atomic_exchange_n(&p->initial_ref_dropped, 1u, __ATOMIC_ACQ_REL))
        return;
    kobject_release(&p->base);
}

/* Phase S1: the notification quota (acquire/release + owner binding) is
 * RETIRED — notifications are created from Untyped, and Untyped is the
 * budget.  The VMO and page quotas below stay: they account the legacy
 * KProcess/KVMO objects that have not yet migrated to the canonical model
 * (LEGACY_FOR_KPROCESS_KVMO in the convergence ledger). */

iris_error_t kprocess_quota_acquire_vmo(struct KProcess *p) {
    return kprocess_quota_acquire(&p->owned_vmos, &p->owned_vmos_hwm,
                                  KPROCESS_VMO_QUOTA, p);
}

void kprocess_quota_release_vmo(struct KProcess *p) {
    kprocess_quota_release(&p->owned_vmos, p);
}

/*
 * Stage 7: the per-process PAGE quota is RETIRED.
 *
 * It was a second ceiling on top of the real one, and since Stage 6-pure it
 * contradicted the model rather than reinforcing it: a VMO's pages come out of
 * an Untyped the caller NAMED, and exhausting that Untyped is what "out of
 * memory" means.  phys_pages_limit was a number the kernel invented — the very
 * thing Stage 6 Step 5 removed for VMO pages and then left standing beside
 * them, so a holder with a large delegated budget still stopped at 8 MB that
 * nobody granted and nobody could raise.
 *
 * The counters stay as pure INSTRUMENTATION: how many pages a domain's VMOs
 * hold is worth reporting, and SYS_RESOURCE_INFO's readers already know a
 * limit of 0 means "no kernel ceiling here" — that is what the notification
 * quota did when it retired in Phase S1.  What is gone is the refusal.
 */
iris_error_t kprocess_quota_acquire_page(struct KProcess *p) {
    if (!p) return IRIS_ERR_INVALID_ARG;
    spinlock_lock(&p->base.lock);
    p->phys_pages_charged++;
    if (p->phys_pages_charged > p->phys_pages_hwm)
        p->phys_pages_hwm = p->phys_pages_charged;
    spinlock_unlock(&p->base.lock);
    return IRIS_OK;
}

void kprocess_quota_release_page(struct KProcess *p) {
    kprocess_quota_release(&p->phys_pages_charged, p);
}

/*
 * kprocess_watch_exit — REMOVED (Stage 7 Step 10) with SYS_PROCESS_WATCH.
 * A death is watched on the THREAD that dies (SYS_TCB_WATCH), by whoever holds
 * its TCB.  The emit/clear pair below stays only as long as the watch ARRAY
 * does, and both go with KProcess.
 */


/*
 * Stage 7 Step 12 — a fault is delivered by the THREAD's own registration.
 *
 * Everything this needs is on the execution that faulted: whom to tell, where
 * to put its capability, and which generation the fault is.  The last-faulter
 * pointer KProcess kept is gone with the question it answered — a read names
 * the thread now, and the thread IS the record.
 */
int kprocess_notify_fault(struct task *t, uint64_t vector,
                           uint64_t error_code, uint64_t rip, uint64_t cr2) {
    struct KNotification *notif;
    struct KCNode        *dest_cs = 0;
    uint32_t              dest_slot = 0;
    uint64_t              bits;

    if (!t) return 0;

    notif = t->fault_notif;
    bits  = t->fault_bits;
    if (!notif) return 0;

    t->fault_seq_counter++;
    if (t->fault_seq_counter == 0) t->fault_seq_counter = 1;
    t->fault_vector = (uint32_t)vector;
    t->fault_rip    = rip;
    t->fault_error  = (uint32_t)error_code;
    t->fault_cr2    = cr2;
    t->fault_seq    = t->fault_seq_counter;
    t->fault_valid  = 1;

    dest_cs   = t->fault_cspace;
    dest_slot = t->fault_slot;
    kobject_retain(&notif->base);
    if (dest_cs) {
        kobject_retain(&dest_cs->base);
        kobject_active_retain(&dest_cs->base);
    }

    /*
     * Hand the handler the faulting THREAD, as a capability, BEFORE the
     * signal — a handler woken by it finds the mailbox already filled.  Not
     * exclusive: the slot is a mailbox, and a previous fault's capability
     * still in it means the handler already answered that one.
     */
    if (dest_cs) {
        (void)kcnode_slot_install_linked(dest_cs, dest_slot, &t->base,
                                         RIGHT_READ | RIGHT_WRITE, 0,
                                         0, 0, /*exclusive=*/0, /*legacy=*/1);
        kobject_active_release(&dest_cs->base);
        kobject_release(&dest_cs->base);
    }

    knotification_signal(notif, bits);
    kobject_release(&notif->base);
    atomic_fetch_add_explicit(&kfault_delivery, 1u, memory_order_relaxed);
    return 1;
}

/* Ordering: emit_exit_watch (Track B: a KNotification signal) fires before
 * handle_table_close_all so the exit_code is already set when watchers wake.
 * teardown_complete provides idempotency; this function is called from both
 * task_exit_current (normal exit) and kprocess_destroy (fallback path). */
/*
 * Join a thread to this process, or refuse because teardown has begun.
 *
 * The check and the count increment are ONE operation under the process lock,
 * and kprocess_teardown claims its flag under the same lock, so the two cannot
 * both win.  ktcb_configure used to read teardown_complete on its own and then
 * attach several steps later; a kill landing in that window produced a thread
 * whose ->process had already had its address space reaped, and the scheduler
 * would run a ring-3 thread with cr3 == 0 under the KERNEL's page tables.
 * A flag read separately from the action it guards is not a gate.
 */
/*
 * Stage 7 Step 13: joining a process TAKES A REFERENCE to it.
 *
 * A thread dereferences `t->process` on every syscall that reads a default
 * budget or an identity, and until now it did so holding nothing — the object
 * stayed alive because SYS_PROCESS_CREATE kept a "creation reference" that no
 * capability accounted for and only the last thread's exit released.  That
 * reference is what made a created-but-never-started process unreclaimable
 * except by SYS_PROCESS_KILL: with no thread to exit, nothing dropped it, and
 * its address space, its root CNode and the budget its header was carved from
 * stayed pinned for the life of the system.
 *
 * With the join holding a reference, the creation one has no job left.  A
 * process lives exactly as long as capabilities and threads reference it, like
 * every other kernel object — which is what makes "delete its capabilities"
 * the whole of killing a process that never ran.
 */
iris_error_t kprocess_attach_thread(struct KProcess *p) {
    iris_error_t r = IRIS_OK;
    if (!p) return IRIS_ERR_INVALID_ARG;
    spinlock_lock(&p->base.lock);
    if (p->teardown_complete || !p->cr3) {
        r = IRIS_ERR_BAD_HANDLE;
    } else {
        p->thread_count++;
        p->threads_ever = 1;
    }
    spinlock_unlock(&p->base.lock);
    if (r == IRIS_OK) kobject_retain(&p->base);
    return r;
}

void kprocess_teardown(struct KProcess *p, struct task *exiting_thread) {
    if (!p) return;
    /* Claimed, not observed — same reason as kprocess_reap_address_space:
     * teardown_complete is set at the END of a long non-idempotent sequence,
     * so a reader that only checks it can start a second run at any point
     * before that.  Claiming on entry also means every OTHER reader of the
     * flag (the destroy fallbacks, kprocess_attach_thread) refuses from the
     * moment teardown begins rather than from the moment it ends, which is the
     * direction they all want.
     *
     * Under p->base.lock rather than a bare atomic exchange, because
     * kprocess_attach_thread has to be able to test the flag and act on the
     * answer without the flag changing in between. */
    {
        int already;
        spinlock_lock(&p->base.lock);
        already = p->teardown_complete;
        p->teardown_complete = 1;
        spinlock_unlock(&p->base.lock);
        if (already) return;
    }

    kprocess_emit_exit_watch(p);
    kprocess_clear_exit_watch(p);
    /* Stage 7 Step 12: the fault record and the handler registration are the
     * THREAD's, and a terminating thread clears its own — so there is nothing
     * process-scoped left to clear here.  The property the old block existed
     * for still holds and is asserted the same way (T145/T146/T147): a read
     * after death answers WOULD_BLOCK rather than a stale fault. */
    /* Phase 6.3: VMO mappings are now tracked via KVSpace.mappings and cleaned
     * by kvspace_invalidate inside kprocess_reap_address_space.  No per-process
     * VMO mapping list exists; nothing to do here. */
    irq_routing_unregister_owner(p);

    /* Stage 4: the CSpace root used to be owned by the handle table, so
     * handle_table_close_all above released it.  It is now held structurally,
     * so its refs are dropped here — at the same point in teardown, to keep
     * the ordering the rest of the sequence was written against.  Dropping the
     * last ref runs the CNode destructor, which tears down every slot through
     * the MDB tree; no lock is held here, which is the precondition that
     * teardown must satisfy. */
    if (p->cspace_root) {
        struct KCNode *root = p->cspace_root;
        p->cspace_root = 0;
        /* Stage 5 Step 3: empty the root's slots BEFORE dropping its refs.
         *
         * A CSpace may name its own CNodes — the root task holds a capability
         * to its own root CNode — and a slot holds references on what it
         * names, so a CNode reachable from itself never reaches zero
         * references and the close callback that empties it never runs.  The
         * whole CSpace would outlive the process.  Emptying first breaks any
         * such cycle without depending on the refcount the cycle is holding
         * up, and is idempotent for the ordinary acyclic case. */
        kcnode_teardown_slots(root);
        kobject_active_release(&root->base);
        kobject_release(&root->base);
    }

    (void)exiting_thread; /* thread_count tracks liveness; no per-thread ref needed */
}

iris_error_t kprocess_register_bootstrap_frame(struct KProcess *p, struct KFrame *f) {
    if (!p || !f) return IRIS_ERR_INVALID_ARG;
    if (p->bootstrap_frame_count >= KPROCESS_BOOTSTRAP_FRAME_MAX) return IRIS_ERR_NO_MEMORY;
    p->bootstrap_frames[p->bootstrap_frame_count++] = f;
    return IRIS_OK;
}

void kprocess_release_bootstrap_frames(struct KProcess *p) {
    if (!p) return;
    for (uint32_t i = 0; i < p->bootstrap_frame_count; i++) {
        if (p->bootstrap_frames[i]) {
            kobject_release(&p->bootstrap_frames[i]->base);
            p->bootstrap_frames[i] = 0;
        }
    }
    p->bootstrap_frame_count = 0;
}

void kprocess_reap_address_space(struct KProcess *p) {
    if (!p) return;
    /* CLAIM the reap, do not merely observe that nobody has done it.  Every
     * step below is destructive and none is idempotent: releasing the
     * bootstrap KFrames twice drops refs the process never held, and
     * destroying cr3 twice frees the same page twice (to the PMM for a
     * kernel-funded space, to the pool's child count for a budgeted one).  A
     * plain read-then-write leaves the window between them open to anything
     * that preempts, so the flag is set by exchange and the losers return. */
    if (__atomic_exchange_n(&p->aspace_reaped, 1u, __ATOMIC_ACQ_REL)) return;

    /* Phase 4: invalidate the VSpace capability so no holder can map through a
     * dying address space.  Stage 7 Step 11: that is ALL this does to it — the
     * walk comes down in the VSpace's own destructor, when the last capability
     * to the address space goes, which may be later than this and is the only
     * moment at which nothing can be using it. */
    struct KVSpace *vs = p->vspace;
    if (vs) {
        kvspace_invalidate(vs);
        p->vspace = 0;
    }

    /* Phase 6.2: release bootstrap KFrame alloc retains after kvspace_invalidate
     * has decremented mapped_count to 0 for all bootstrap-mapped pages. */
    kprocess_release_bootstrap_frames(p);

    p->cr3 = 0;
    if (vs) kobject_release(&vs->base);
    /* aspace_reaped was claimed on entry, not set here. */
}

/*
 * kprocess_live_count: count live kpage-backed process objects.
 */
uint32_t kprocess_live_count(void) {
    return atomic_load_explicit(&kprocess_live, memory_order_relaxed);
}
