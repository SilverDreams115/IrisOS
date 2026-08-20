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

void kprocess_fault_stat_nohandler(void) { atomic_fetch_add_explicit(&kfault_nohandler, 1u, memory_order_relaxed); }

/*
 * kprocess_fault_clear — drop the pending-fault record for process p if it
 * belongs to task_id.  Phase 20: SYS_EXCEPTION_RESUME calls this so a resolved
 * fault stops being reported by SYS_PROCESS_FAULT_INFO (which must return
 * WOULD_BLOCK when nothing is pending).  `killed` selects the resume/kill
 * counter.  Idempotent — a second call with no matching pending fault is a
 * no-op.
 */
void kprocess_fault_clear(struct KProcess *p, uint32_t task_id, int killed) {
    if (!p) return;
    spinlock_lock(&p->base.lock);
    if (p->fault_valid && p->fault_task_id == task_id) {
        p->fault_valid = 0;
        atomic_fetch_add_explicit(&kfault_cleanup, 1u, memory_order_relaxed);
    }
    spinlock_unlock(&p->base.lock);
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

static void kprocess_clear_exception_chan(struct KProcess *p) {
    struct KNotification *old = 0;
    if (!p) return;

    spinlock_lock(&p->base.lock);
    old = p->exception_notif;
    p->exception_notif = 0;
    spinlock_unlock(&p->base.lock);

    if (!old) return;
    kobject_active_release(&old->base);
    kobject_release(&old->base);
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
    struct KUntyped *pool = p->mem_pool;
    p->mem_pool = 0;
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
    p->mem_pool = pool;


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

iris_error_t kprocess_watch_exit(struct KProcess *p, struct KNotification *notif,
                                 uint64_t signal_bits) {
    if (!p || !notif || signal_bits == 0) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&p->base.lock);
    uint32_t slot = KPROCESS_EXIT_WATCH_MAX;
    for (uint32_t i = 0; i < KPROCESS_EXIT_WATCH_MAX; i++) {
        if (!p->exit_watches[i].armed) { slot = i; break; }
    }
    if (slot == KPROCESS_EXIT_WATCH_MAX) {
        spinlock_unlock(&p->base.lock);
        return IRIS_ERR_TABLE_FULL;
    }
    kobject_retain(&notif->base);
    p->exit_watches[slot].notif = notif;
    p->exit_watches[slot].signal_bits = signal_bits;
    p->exit_watches[slot].armed = 1;
    spinlock_unlock(&p->base.lock);

    if (!kprocess_is_alive(p)) {
        kprocess_emit_exit_watch(p);
        kprocess_clear_exit_watch(p);
    }
    return IRIS_OK;
}

iris_error_t kprocess_set_exception_handler(struct KProcess *p,
                                            struct KNotification *notif,
                                            uint64_t signal_bits) {
    struct KNotification *old;
    if (!p || !notif || signal_bits == 0) return IRIS_ERR_INVALID_ARG;

    kobject_retain(&notif->base);
    kobject_active_retain(&notif->base);

    spinlock_lock(&p->base.lock);
    /* Phase 20: registering on a torn-down process would re-pin exception_notif
     * AFTER kprocess_teardown already ran kprocess_clear_exception_chan —
     * nothing would ever release those refs (kprocess_destroy skips teardown
     * once teardown_complete is set), leaking the notification.  A racing
     * registration that reads the flag before teardown stores it is swept by
     * teardown's second clear.  NOTE: a never-started process (thread_count 0,
     * no teardown) is a legitimate target — registering BEFORE the first task
     * starts is the race-free supervisor order (phase3 selftest covers it). */
    if (p->teardown_complete) {
        spinlock_unlock(&p->base.lock);
        kobject_active_release(&notif->base);
        kobject_release(&notif->base);
        return IRIS_ERR_NOT_FOUND;
    }
    old = (p->exception_notif == notif) ? 0 : p->exception_notif;
    if (p->exception_notif == notif) {
        /* already set: drop the extra ref we took */
        spinlock_unlock(&p->base.lock);
        kobject_active_release(&notif->base);
        kobject_release(&notif->base);
        return IRIS_OK;
    }
    p->exception_notif = notif;
    p->exception_signal_bits = signal_bits;
    spinlock_unlock(&p->base.lock);

    if (old) {
        kobject_active_release(&old->base);
        kobject_release(&old->base);
    }
    return IRIS_OK;
}

int kprocess_notify_fault(struct task *t, uint64_t vector,
                           uint64_t error_code, uint64_t rip, uint64_t cr2) {
    struct KProcess *p;
    struct KNotification *notif;
    uint64_t bits;

    if (!t || !t->process) return 0;
    p = t->process;

    /* Phase 13 (Track I): record the fault details in the KProcess and signal the
     * handler's KNotification — the handler reads the details via
     * SYS_PROCESS_FAULT_INFO and resumes/kills via SYS_EXCEPTION_RESUME.  No
     * KChannel. */
    spinlock_lock(&p->base.lock);
    notif = p->exception_notif;
    bits  = p->exception_signal_bits;
    if (notif) {
        /* Phase 25: assign the fault a per-process generation.  1-based so 0
         * always means "no fault"; skip 0 on uint32 wrap.  The blocked task
         * keeps its own copy — the per-process record is last-writer-wins,
         * but each suspended task must stay resolvable by ITS generation. */
        p->fault_seq_counter++;
        if (p->fault_seq_counter == 0) p->fault_seq_counter = 1;
        p->fault_vector  = (uint32_t)vector;
        p->fault_task_id = t->id;
        p->fault_rip     = rip;
        p->fault_error   = (uint32_t)error_code;
        p->fault_cr2     = cr2;
        p->fault_seq     = p->fault_seq_counter;
        p->fault_valid   = 1;
        t->fault_seq     = p->fault_seq_counter;
        kobject_retain(&notif->base);
    }
    spinlock_unlock(&p->base.lock);
    if (!notif) return 0;

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
    kprocess_clear_exception_chan(p);
    /* Phase 20 (F15): a fault record must not outlive the process — a late
     * SYS_PROCESS_FAULT_INFO through a surviving handle reports WOULD_BLOCK,
     * not a stale fault of a dead task. */
    spinlock_lock(&p->base.lock);
    if (p->fault_valid) {
        p->fault_valid = 0;
        atomic_fetch_add_explicit(&kfault_cleanup, 1u, memory_order_relaxed);
    }
    spinlock_unlock(&p->base.lock);
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
    /* teardown_complete was claimed on entry, so the flag has rejected new
     * registrations for the whole of this function.
     *
     * Phase 20: a registration that was already past that check when we
     * claimed, and landed after the clear above, would re-pin the
     * notification with nobody left to release it; sweep again to catch it. */
    kprocess_clear_exception_chan(p);
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

    uint64_t cr3 = p->cr3;
    /* Was this address space's PML4 carved from an Untyped?  Read it before
     * the VSpace goes: a pooled page must not be returned to the PMM, which
     * does not own it.  This answers for the PML4 PAGE and nothing else — the
     * levels below it are answered for one at a time by the detach below,
     * because a single address space can hold both kinds at once (the root
     * task's: a PMM PML4 and PMM bootstrap levels, plus every level it retypes
     * for itself after kvspace_end_bootstrap). */
    int pml4_pooled = (p->vspace && p->vspace->pml4_from_pool) ? 1 : 0;

    /* Phase 4: invalidate the VSpace capability before destroying page tables.
     * Any capability holder that checks vs->valid after this point sees 0. */
    struct KVSpace *vs = p->vspace;
    if (vs) {
        kvspace_invalidate(vs);
        p->vspace = 0;
    }

    /* Stage 6-pure: take the holder's own levels out of the walk while we
     * still know which ones they are.  After this the only thing still hanging
     * off the PML4 is what alloc_table() took from the PMM, which is the one
     * thing paging_destroy_user_space_from is allowed to give back. */
    if (vs) kvspace_detach_tables(vs, cr3);

    /* Phase 6.2: release bootstrap KFrame alloc retains after kvspace_invalidate
     * has decremented mapped_count to 0 for all bootstrap-mapped pages. */
    kprocess_release_bootstrap_frames(p);

    /*
     * Tear the walk down BEFORE dropping the VSpace reference.
     *
     * Stage 6-pure Step 4 made the PML4 the VSpace object's own storage, so
     * releasing the VSpace can be what returns that page's accounting to its
     * Untyped — and then walking cr3 would be walking a region whose holder
     * has already been told it is free to RESET.  The walk first, the object
     * after: the reverse of how it reads, and the only order that is true.
     */
    paging_destroy_user_space_from(cr3, pml4_pooled);
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
