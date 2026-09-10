/*
 * kfault.c — delivering a fault to the handler a thread named, and the two
 * counters that make quota exhaustion and fault handling observable.
 *
 * This file was kprocess.c.  `struct KProcess` was deleted in Stage 7-proc and
 * the name stayed for four stages, pointing readers at an object that does not
 * exist.  The `kprocess_` prefix on the counter accessors is kept on purpose:
 * it is the name `sys_sched_info` reports them under, and renaming a wire
 * label to tidy a file is the trade the other way round.
 */
#include <iris/nc/kprocess.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/syscall.h>
#include <iris/kslab.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/fault_proto.h>
#include <iris/nc/kendpoint.h>
#include <iris/ipc_msg.h>
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


/* The fault record's wire layout is byte-offset based (fault_proto.h), and the
 * message body is not guaranteed aligned for a 32/64-bit store at every offset,
 * so it is written a byte at a time.  Little-endian, matching every other
 * multi-byte field IRIS puts on a wire. */
static void kfault_wr32(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i < 4u; i++) p[i] = (uint8_t)(v >> (8u * i));
}
static void kfault_wr64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8u; i++) p[i] = (uint8_t)(v >> (8u * i));
}

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
    struct KEndpoint *ep;
    uint64_t          badge;

    if (!t) return 0;

    ep    = timeout ? t->timeout_ep       : t->fault_ep;
    badge = timeout ? t->timeout_ep_badge : t->fault_ep_badge;
    if (!ep) return 0;

    t->fault_seq_counter++;
    if (t->fault_seq_counter == 0) t->fault_seq_counter = 1;
    t->fault_vector = (uint32_t)vector;
    t->fault_rip    = rip;
    t->fault_error  = (uint32_t)error_code;
    t->fault_cr2    = cr2;
    t->fault_seq    = t->fault_seq_counter;
    t->fault_valid  = 1;

    /*
     * The record IS the message.
     *
     * It used to be written here and read back later through
     * SYS_TCB_FAULT_INFO, which meant a handler needed a capability to the
     * faulting thread just to find out what had happened to it — so every
     * fault minted one into a mailbox.  The wire layout is unchanged
     * (fault_proto.h): what changed is that it travels in the IPC rather than
     * sitting in the kernel waiting to be fetched.
     */
    struct IrisMsg msg;
    for (uint32_t i = 0; i < sizeof(msg) / sizeof(uint64_t); i++)
        ((uint64_t *)&msg)[i] = 0;
    msg.label        = FAULT_MSG_NOTIFY;
    msg.word_count   = FAULT_MSG_LEN / sizeof(uint64_t);
    msg.sender_badge = badge;
    {
        uint8_t *d = (uint8_t *)msg.words;
        _Static_assert(FAULT_MSG_LEN <= IRIS_MSG_WORDS * sizeof(uint64_t),
                       "the fault record must fit in the message registers");
        kfault_wr32(d + FAULT_OFF_VECTOR, (uint32_t)vector);
        kfault_wr32(d + FAULT_OFF_TASK_ID, t->id);
        kfault_wr64(d + FAULT_OFF_RIP,     rip);
        kfault_wr32(d + FAULT_OFF_ERROR,  (uint32_t)error_code);
        kfault_wr32(d + FAULT_OFF_SEQ,     t->fault_seq);
        kfault_wr64(d + FAULT_OFF_CR2,     cr2);
    }

    /*
     * Ledger A-22 — the thread CALLS its fault handler.
     *
     * A failure here means the endpoint is closed: there is a registration but
     * nobody behind it, which is the same situation as no registration at all
     * and is reported the same way, so the caller kills the thread instead of
     * leaving it blocked on an answer that can never come.
     *
     * The scheduling context travels with the call for exception faults, so a
     * pager runs on the time of the client it is serving — kendpoint_fault_call
     * uses the same donation every passive server gets.  A TIMEOUT fault is the
     * one case where it must not: the whole message is "this thread's budget
     * ran out", and donating an exhausted budget to the handler would starve
     * the one principal able to do something about it.
     */
    if (!kendpoint_fault_call(t, ep, &msg)) {
        t->fault_valid = 0;
        return 0;
    }

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




