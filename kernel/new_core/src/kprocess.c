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
/*
 * Deliver a fault to one of the thread's two handler registrations.
 *
 * `timeout` selects WHICH registration is told, and nothing else differs: a
 * timeout fault and an exception fault are the same delivery — record the
 * fault on the thread, publish the thread's capability into the mailbox the
 * registrant named, parent it to the slot the registration was made with,
 * then signal.  Two field groups, one delivery path, for the same reason
 * there is one registration path: a second copy is a second thing to drift.
 *
 * The two cannot collide on the shared record.  A thread that has taken an
 * exception is TASK_BLOCKED_FAULT and is not running, so it cannot also be
 * spending budget; a thread whose budget expired is suspended before this is
 * called.  One record, one fault in flight.
 */
static int kfault_deliver(struct task *t, uint64_t vector,
                          uint64_t error_code, uint64_t rip, uint64_t cr2,
                          int timeout) {
    struct KNotification *notif;
    struct KCNode        *dest_cs = 0;
    uint32_t              dest_slot = 0;
    struct KCNode        *src_cn = 0;
    uint32_t              src_idx = 0;
    uint64_t              bits;

    if (!t) return 0;

    notif = timeout ? t->timeout_notif : t->fault_notif;
    bits  = timeout ? t->timeout_bits  : t->fault_bits;
    if (!notif) return 0;

    t->fault_seq_counter++;
    if (t->fault_seq_counter == 0) t->fault_seq_counter = 1;
    t->fault_vector = (uint32_t)vector;
    t->fault_rip    = rip;
    t->fault_error  = (uint32_t)error_code;
    t->fault_cr2    = cr2;
    t->fault_seq    = t->fault_seq_counter;
    t->fault_valid  = 1;

    dest_cs   = timeout ? t->timeout_cspace  : t->fault_cspace;
    dest_slot = timeout ? t->timeout_slot    : t->fault_slot;
    src_cn    = timeout ? t->timeout_src_cn  : t->fault_src_cn;
    src_idx   = timeout ? t->timeout_src_idx : t->fault_src_idx;
    kobject_retain(&notif->base);
    if (dest_cs) {
        kobject_retain(&dest_cs->base);
        kobject_active_retain(&dest_cs->base);
        if (src_cn) {
            kobject_retain(&src_cn->base);
            kobject_active_retain(&src_cn->base);
        }
    }

    /*
     * Hand the handler the faulting THREAD, as a capability, BEFORE the
     * signal — a handler woken by it finds the mailbox already filled.  Not
     * exclusive: the slot is a mailbox, and a previous fault's capability
     * still in it means the handler already answered that one.
     */
    if (dest_cs) {
        /*
         * Stage 8-cap / D-6: as a CHILD of the slot the registration was made
         * with, so revoking that capability reaches every copy the kernel
         * handed out.  Verified by IDENTITY, not occupancy — between arming
         * and faulting the registrant's slot can have been deleted and
         * refilled, and a child under the new occupant would hang off an
         * ancestor that never authorised it.
         *
         * When it does not hold, the capability is still delivered (a handler
         * with a signal and no thread to answer with is a deadlock dressed as
         * a working handler) but as a root, and the gauge T305 watches counts
         * it.  That is the honest failure: the delegation outlived the
         * capability it was made with, and the count says so.
         */
        int parented = src_cn && kcnode_slot_holds(src_cn, src_idx, &t->base);
        (void)kcnode_slot_install_linked(dest_cs, dest_slot, &t->base,
                                         RIGHT_READ | RIGHT_WRITE, 0,
                                         parented ? src_cn : 0, src_idx,
                                         /*exclusive=*/0, /*legacy=*/!parented);
        kobject_active_release(&dest_cs->base);
        kobject_release(&dest_cs->base);
        if (src_cn) {
            kobject_active_release(&src_cn->base);
            kobject_release(&src_cn->base);
        }
    }

    knotification_signal(notif, bits);
    kobject_release(&notif->base);
    atomic_fetch_add_explicit(&kfault_delivery, 1u, memory_order_relaxed);
    return 1;
}

int kprocess_notify_fault(struct task *t, uint64_t vector,
                          uint64_t error_code, uint64_t rip, uint64_t cr2) {
    return kfault_deliver(t, vector, error_code, rip, cr2, /*timeout=*/0);
}

/*
 * Stage 8-mcs — a thread's scheduling context ran out of budget.
 *
 * Reported as a fault with a distinguished vector so a handler reads it with
 * the same SYS_TCB_FAULT_INFO it already uses, and answers it with the same
 * SYS_EXCEPTION_RESUME.  There is no rip/cr2 to report: the thread did not do
 * anything wrong at an address, it ran out of time, and the fields the record
 * cannot fill are zero rather than stale.
 *
 * Returns 0 when no timeout handler is armed, which is the default and the
 * pre-Stage-8 behaviour: the caller then blocks the thread until its period
 * refills the budget, and nobody is told.
 */
int ktimeout_notify_fault(struct task *t) {
    return kfault_deliver(t, IRIS_FAULT_VECTOR_TIMEOUT, 0, 0, 0,
                          /*timeout=*/1);
}

/* Ordering: emit_exit_watch (Track B: a KNotification signal) fires before
 * handle_table_close_all so the exit_code is already set when watchers wake.
 * teardown_complete provides idempotency; this function is called from both
 * task_exit_current (normal exit) and kprocess_destroy (fallback path). */




