#include <iris/nc/kprocess.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/handle_table.h>
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

/* Fase 29 — global resource-accounting instrumentation (additive, exposed via
 * SYS_RESOURCE_INFO).  A charge that hits its domain's limit increments
 * kquota_failed_charges; a provisional charge rolled back on a later failure in
 * the same operation increments kquota_rollbacks.  Both make quota-exhaustion
 * atomicity observable to T246/T250 without changing behaviour. */
static _Atomic uint32_t kquota_failed_charges;
static _Atomic uint32_t kquota_rollbacks;

uint32_t kprocess_quota_failed_count(void)  { return atomic_load_explicit(&kquota_failed_charges, memory_order_relaxed); }
uint32_t kprocess_quota_rollback_count(void){ return atomic_load_explicit(&kquota_rollbacks,      memory_order_relaxed); }
void     kprocess_quota_stat_rollback(void) { atomic_fetch_add_explicit(&kquota_rollbacks, 1u, memory_order_relaxed); }

/* Fase 20 — fault-model instrumentation (additive, exposed via SYS_SCHED_INFO
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
 * belongs to task_id.  Fase 20: SYS_EXCEPTION_RESUME calls this so a resolved
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

/* PCID allocation bitmap: 64 words × 64 bits = PCIDs 0–4095.
 * Bit 0 (PCID 0 = kernel) and bit 4095 (reserved) are pre-set.
 * PCIDs 1–4094 are available for user processes.
 * Protected by pcid_lock (irq_spinlock) so alloc/free are safe
 * from any context.  BSS zero-init of atomic_flag == unlocked. */
#define PCID_BITMAP_WORDS 64u
static uint64_t      pcid_bitmap[PCID_BITMAP_WORDS] = {
    [0]  = 1ULL,           /* PCID 0 = kernel, always reserved */
    [63] = (1ULL << 63),   /* PCID 4095, reserved */
};
static irq_spinlock_t pcid_lock; /* BSS zero = unlocked */

static iris_error_t kprocess_quota_acquire(uint32_t *counter, uint32_t *hwm,
                                           uint32_t limit, struct KProcess *p) {
    iris_error_t r = IRIS_OK;
    if (!counter || !p) return IRIS_ERR_INVALID_ARG;
    spinlock_lock(&p->base.lock);
    if (*counter >= limit) {
        r = IRIS_ERR_NO_MEMORY;
    } else {
        (*counter)++;
        if (hwm && *counter > *hwm) *hwm = *counter;   /* Fase 29: high-water */
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
        /* Fase 13 (Track B): death is delivered as a KNotification signal —
         * the watcher identifies the dead service by which bit is set and
         * re-queries SYS_PROCESS_EXIT_CODE / STATUS for detail. */
        knotification_signal(w->notif, w->signal_bits);
    }
}

static void kprocess_destroy(struct KObject *obj) {
    struct KProcess *p = (struct KProcess *)obj;

    /* Final refcount drop: finish only idempotent process-owned cleanup.
     * Task-local resources must already be gone before this point. */
    if (!p->teardown_complete) {
        kprocess_teardown(p, 0);
    }
    if (!p->aspace_reaped) {
        kprocess_reap_address_space(p);
    }
    atomic_fetch_sub_explicit(&kprocess_live, 1u, memory_order_relaxed);

    /* Return PCID to the free pool so it can be reused. */
    if (p->pcid) {
        uint64_t flags = irq_spinlock_lock(&pcid_lock);
        pcid_bitmap[p->pcid / 64u] &= ~(1ULL << (p->pcid % 64u));
        irq_spinlock_unlock(&pcid_lock, flags);
        p->pcid = 0;
    }

    kslab_free(p, (uint32_t)sizeof(struct KProcess));
}

static const struct KObjectOps kprocess_ops = {
    .destroy = kprocess_destroy
};

struct KProcess *kprocess_alloc(void) {
    /* Atomically reserve a slot before allocating memory.  The previous
     * load+check+alloc+increment pattern had a TOCTOU window where concurrent
     * callers could all pass the check before any incremented the counter,
     * allowing live process count to exceed KPROCESS_MAX_LIVE.
     *
     * Strategy: fetch-add first, then roll back if the old value was already
     * at the limit.  This is safe because kprocess_destroy always decrements,
     * so the counter never permanently overshoots if we roll back here. */
    uint32_t prev = atomic_fetch_add_explicit(&kprocess_live, 1u, memory_order_relaxed);
    if (prev >= KPROCESS_MAX_LIVE) {
        atomic_fetch_sub_explicit(&kprocess_live, 1u, memory_order_relaxed);
        return 0;
    }
    struct KProcess *p = kslab_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) {
        atomic_fetch_sub_explicit(&kprocess_live, 1u, memory_order_relaxed);
        return 0;
    }
    kobject_init(&p->base, KOBJ_PROCESS, &kprocess_ops);
    handle_table_init(&p->handle_table);
    p->phys_pages_limit = KPROCESS_PHYS_PAGES_LIMIT;
    if (iris_pcid_enabled) {
        uint64_t flags = irq_spinlock_lock(&pcid_lock);
        uint16_t pcid = 0;
        for (uint32_t w = 0; w < PCID_BITMAP_WORDS && !pcid; w++) {
            uint64_t free_bits = ~pcid_bitmap[w];
            if (!free_bits) continue;
            uint32_t bit = (uint32_t)__builtin_ctzll(free_bits);
            uint32_t id  = w * 64u + bit;
            if (id >= 1u && id <= 4094u) {
                pcid_bitmap[w] |= (1ULL << bit);
                pcid = (uint16_t)id;
            }
        }
        irq_spinlock_unlock(&pcid_lock, flags);
        if (!pcid) {
            /* All 4094 PCIDs in use — cannot happen with KPROCESS_MAX_LIVE=64 */
            atomic_fetch_sub_explicit(&kprocess_live, 1u, memory_order_relaxed);
            kslab_free(p, (uint32_t)sizeof(struct KProcess));
            return 0;
        }
        p->pcid = pcid;
    }

    /* Ph95: root CNode for hierarchical CSpace.  Soft-fail: if alloc OOMs
     * the process still works, but cspace_root_h stays HANDLE_INVALID. */
    p->cspace_root_h = HANDLE_INVALID;
    {
        struct KCNode *root_cn = kcnode_alloc(KCNODE_DEFAULT_SLOTS);
        if (root_cn) {
            handle_id_t rh = handle_table_insert(
                &p->handle_table, &root_cn->base,
                RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
            kobject_release(&root_cn->base);
            if (rh != HANDLE_INVALID)
                p->cspace_root_h = rh;
        }
    }

    return p;
}

void kprocess_free(struct KProcess *p) {
    kobject_release(&p->base);
}

/* Fase S1: the notification quota (acquire/release + owner binding) is
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

iris_error_t kprocess_quota_acquire_page(struct KProcess *p) {
    if (!p) return IRIS_ERR_INVALID_ARG;
    return kprocess_quota_acquire(&p->phys_pages_charged, &p->phys_pages_hwm,
                                  p->phys_pages_limit, p);
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
    /* Fase 20: registering on a torn-down process would re-pin exception_notif
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

    /* Fase 13 (Track I): record the fault details in the KProcess and signal the
     * handler's KNotification — the handler reads the details via
     * SYS_PROCESS_FAULT_INFO and resumes/kills via SYS_EXCEPTION_RESUME.  No
     * KChannel. */
    spinlock_lock(&p->base.lock);
    notif = p->exception_notif;
    bits  = p->exception_signal_bits;
    if (notif) {
        /* Fase 25: assign the fault a per-process generation.  1-based so 0
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
void kprocess_teardown(struct KProcess *p, struct task *exiting_thread) {
    if (!p || p->teardown_complete) return;

    kprocess_emit_exit_watch(p);
    kprocess_clear_exit_watch(p);
    kprocess_clear_exception_chan(p);
    /* Fase 20 (F15): a fault record must not outlive the process — a late
     * SYS_PROCESS_FAULT_INFO through a surviving handle reports WOULD_BLOCK,
     * not a stale fault of a dead task. */
    spinlock_lock(&p->base.lock);
    if (p->fault_valid) {
        p->fault_valid = 0;
        atomic_fetch_add_explicit(&kfault_cleanup, 1u, memory_order_relaxed);
    }
    spinlock_unlock(&p->base.lock);
    /* Fase 6.3: VMO mappings are now tracked via KVSpace.mappings and cleaned
     * by kvspace_invalidate inside kprocess_reap_address_space.  No per-process
     * VMO mapping list exists; nothing to do here. */
    irq_routing_unregister_owner(p);
    handle_table_close_all(&p->handle_table);

    (void)exiting_thread; /* thread_count tracks liveness; no per-thread ref needed */
    p->teardown_complete = 1;
    /* Fase 20: a registration that raced in between the clear above and the
     * flag store would re-pin the notification with nobody left to release
     * it; sweep again now that the flag rejects new registrations. */
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
    if (!p || p->aspace_reaped) return;

    uint64_t cr3 = p->cr3;

    /* Fase 4: invalidate the VSpace capability before destroying page tables.
     * Any capability holder that checks vs->valid after this point sees 0. */
    if (p->vspace) {
        kvspace_invalidate(p->vspace);
        kobject_release(&p->vspace->base);
        p->vspace = 0;
    }

    /* Fase 6.2: release bootstrap KFrame alloc retains after kvspace_invalidate
     * has decremented mapped_count to 0 for all bootstrap-mapped pages. */
    kprocess_release_bootstrap_frames(p);

    paging_destroy_user_space(cr3);
    p->cr3 = 0;
    p->user_cr3 = 0;
    p->aspace_reaped = 1;
}

/*
 * kprocess_live_count: count live kpage-backed process objects.
 */
uint32_t kprocess_live_count(void) {
    return atomic_load_explicit(&kprocess_live, memory_order_relaxed);
}
