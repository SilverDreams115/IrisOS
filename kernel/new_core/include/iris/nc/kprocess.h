#ifndef IRIS_NC_KPROCESS_H
#define IRIS_NC_KPROCESS_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/paging.h>
#include <iris/task.h>
#include <stdint.h>

struct KVmo;
struct KNotification;
struct KVSpace;
struct KFrame;

#define KPROCESS_EXIT_WATCH_MAX 8u
#define KPROCESS_MAX_LIVE       64u /* bounded by TASK_MAX; enforced in kprocess_alloc */
/* Phase S1: KPROCESS_NOTIFICATION_QUOTA retired — the capacity to create
 * notifications is possessing Untyped memory plus CSpace slots, never a
 * kernel-side numeric quota.  (SYS_RESOURCE_INFO reports notifs_limit = 0.) */
/* Phase 29: RESTORED 128 → 32.  Phase 28.1 temporarily raised this to 128 to
 * work around a caller-charged accounting BUG: a loader (svc_load) created each
 * child's segment+stack VMOs under ITS OWN ownership, so a supervisor holding N
 * children accumulated ~4*N VMOs against its own quota.  Phase 29 fixes the root
 * cause — a VMO created for a child is now charged to the CHILD via an explicit,
 * capability-authorized payer (sys_vmo_create charge-target; svc_loader passes
 * the child process cap) — so the loader's own_vmos stays flat regardless of how
 * many children it launches.  32 is now a genuine PER-PROCESS ceiling on the
 * VMOs a single domain owns, not a proxy for how many children a supervisor can
 * launch.  Raising the constant is no longer the answer. */
#define KPROCESS_VMO_QUOTA      32u
/* Stage 7: RETIRED.  The per-process page ceiling was a number the kernel
 * invented; since Stage 6-pure a VMO's pages come from an Untyped the caller
 * named, and exhausting THAT is what running out means.  phys_pages_limit
 * reports 0 — "no kernel ceiling" — the way the notification quota did when it
 * retired in Phase S1.  The constant is kept only so the retirement is legible. */
#define KPROCESS_PHYS_PAGES_LIMIT 0u

/* Maximum bootstrap KFrame retains stored in KProcess.bootstrap_frames[].
 * Enforced by the upfront guard in task_create_user_impl. */
#define KPROCESS_BOOTSTRAP_FRAME_MAX 32u

struct KExitWatch {
    struct KNotification *notif;       /* signalled on watched-process death */
    uint64_t              signal_bits; /* bits OR'd into notif on exit */
    uint8_t               armed;
};

/*
 * KProcess — process control object.
 *
 * Owns process-scoped resources and points to the initial thread
 * (which lives in the kernel's static tasks[] array).
 *
 * The KProcess and the initial thread have independent lifecycles:
 *   - thread_count hits 0 when the last thread calls task_exit_current.
 *   - The KProcess (and handles pointing to it) lives until its
 *     refcount drops to zero (all handles closed).
 *
 * Lifecycle split:
 *   - kprocess_teardown(): logical teardown when thread_count reaches 0;
 *     closes process-scoped handles and unregisters global ownership.
 *   - kprocess_reap_address_space(): runs only after switching away from the
 *     last thread's CR3; frees the process-owned address space.
 *   - destroy(): final object cleanup when refcount reaches zero. It may finish
 *     any missing idempotent cleanup, but must not touch task-local resources
 *     such as the user stack or scheduler linkage.
 *
 * Invariants:
 *   - base must be first (KObject cast rules).
 *   - thread_count tracks live threads; 0 means process is fully exited.
 *   - teardown_complete == 1 means logical teardown has STARTED — the flag is
 *     claimed on entry, not stored on completion, so exactly one caller ever
 *     runs the sequence.  Readers treat it as "this process is finished", and
 *     refusing from the start of teardown rather than the end is the point.
 *   - aspace_reaped == 1 means cr3-owned memory is gone or is being reclaimed
 *     right now; claimed on entry for the same reason.
 *   - Before final destroy of an exited process, task-local resources must
 *     already have been released by task_exit_current()/reaper paths.
 */
struct KProcess {
    struct KObject  base;       /* must be first */
    uint32_t        thread_count; /* live threads in this process; 0 = dead */
    /* Has this process EVER had a thread?  thread_count == 0 does not say:
     * it is equally true of a process created and never started and of one
     * whose last thread is midway through exiting, and those two need
     * opposite handling — the first has no teardown path but this flag, the
     * second already has one running.  Set at every thread_count increment,
     * never cleared. */
    uint8_t         threads_ever;
    uint64_t        cr3;          /* page table root for the process */
    uint64_t        user_cr3;     /* cr3|pcid|(1<<63 if PCID) — no-flush variant for iretq */
    uint16_t        pcid;         /* PCID assigned at alloc (0 = unused/PCID disabled) */
    uint8_t         teardown_complete; /* logical teardown already ran */
    uint32_t        exit_code;    /* exit code from SYS_EXIT; 0 if killed externally */
    uint8_t         aspace_reaped;     /* address space cleanup already ran */
    struct KExitWatch exit_watches[KPROCESS_EXIT_WATCH_MAX]; /* up to 4 death subscribers */
    /* Phase 6.3: vmo_mappings removed — VMO pages are now KFrame-backed and
     * tracked in KVSpace.mappings; kvspace_invalidate handles teardown. */

    /* Exception handler (Phase 13/Track I: KNotification, no longer a KChannel).
     * If exception_notif is non-NULL, the kernel records the fault details in
     * fault_* and signals exception_signal_bits on it before the faulting task
     * is killed; the handler reads the details via SYS_PROCESS_FAULT_INFO. */
    struct KNotification *exception_notif;
    uint64_t exception_signal_bits;
    uint32_t fault_vector;
    uint32_t fault_task_id;
    uint64_t fault_rip;
    uint32_t fault_error;
    uint64_t fault_cr2;
    /* Stage 6 Step 2: the creation reference — the one that lets a running
     * process outlive the last capability to it — is dropped exactly once.
     * Before this flag, a process created and never STARTED could not be
     * reclaimed at all: kill saw no threads and returned success without
     * dropping it, so the KProcess, its VSpace, its PML4 and (now) its
     * page-table budget stayed alive with no way to get them back. */
    uint8_t  initial_ref_dropped;
    /* Stage 6 Step 4: the Untyped this object and its root CNode were carved
     * from, retained for their lifetime.  NULL = kernel-slab (root task). */
    struct KUntyped *mem_pool;
    uint8_t  fault_valid;
    /* Phase 25: per-process fault generation.  fault_seq_counter increments on
     * every delivery (1-based, wraps); fault_seq is the generation of the
     * currently pending record.  The generation of the fault a TASK is blocked
     * on lives in task->fault_seq (per-TCB — the per-process record is
     * last-writer-wins but each blocked task keeps its own generation). */
    uint32_t fault_seq;
    uint32_t fault_seq_counter;
    /* Phase 29 — resource accounting.  A KProcess IS a resource domain: every
     * object is charged to the KProcess that logically OWNS it (its payer),
     * selected by explicit capability authority at creation — NOT to whoever
     * ran the syscall (see docs/architecture/resource-ownership-accounting.md).
     * usage counters are the live charge; *_hwm is the monotonic high-water
     * mark (never decreases), an observable, defensible ceiling witness. */
    /* Phase S1: owned_notifications/_hwm retired with the notification quota —
     * notifications are Untyped-funded (see sel4-convergence-ledger.md). */
    uint32_t owned_vmos;
    uint32_t phys_pages_charged; /* sparse-VMO pages charged at eager map-time
                                  * allocation; vs phys_pages_limit */
    uint32_t phys_pages_limit;   /* set to KPROCESS_PHYS_PAGES_LIMIT at alloc */
    uint32_t owned_vmos_hwm;
    uint32_t phys_pages_hwm;

    /* Ph95 (Phase 8): root CNode for hierarchical CSpace traversal.
     *
     * Stage 4: this was a `handle_id_t` into the process's own handle table,
     * which made the handle table a hard dependency of EVERY CPtr resolution —
     * `cspace_resolve_slot` had to look the root up by handle before it could
     * walk a single slot.  The namespace meant to REPLACE handles was rooted in
     * one.  It is now a structural back-reference, exactly like `vspace` below:
     * the kernel reaches a process's CSpace root directly, and resolving a CPtr
     * touches no handle table at all.  This is also what let the cross-process
     * paths (SYS_CSPACE_MINT_INTO, retype2) stop reaching into ANOTHER
     * process's handle table to find its root.
     *
     * Holds one lifecycle ref + one active ref (the pair the handle used to
     * own), both released in kprocess_teardown.  NULL if kcnode_alloc OOM'd at
     * creation — the process still runs, it just has no CSpace. */
    struct KCNode *cspace_root;

    /* Phase 4: VSpace capability wrapping this process's address space.
     * Holds one lifecycle ref (kobject_retain).  NULL if kvspace_alloc OOM'd at
     * creation or if the process has not yet had its CR3 assigned. */
    struct KVSpace *vspace;

    /* Phase 6.2: Bootstrap KFrame alloc retains.
     * Populated by task_create_user_impl for each page mapped via
     * bootstrap_kframe_map.  Released by kprocess_release_bootstrap_frames
     * inside kprocess_reap_address_space, always AFTER kvspace_invalidate
     * so mapped_count is 0 at the time each alloc retain is dropped. */
    struct KFrame   *bootstrap_frames[KPROCESS_BOOTSTRAP_FRAME_MAX];
    uint32_t         bootstrap_frame_count;

};

#define KPROCESS_POOL_SIZE 0u  /* no static pool — kpage-backed; 0 = unbounded allocator ceiling */

struct KProcess *kprocess_alloc(void);

/*
 * Stage 6-pure Step 5 — a process COMPOSED from objects its creator made.
 *
 * `pool` is the Untyped the process object itself comes out of (the one its
 * address space was retyped from), and `cnode` is the root CSpace the spawner
 * retyped and is handing over.  Both are required: the root task's path is
 * kprocess_alloc(), which is the only one the kernel still funds.
 *
 * The process takes its own lifecycle + active references on `cnode`; the
 * caller keeps its capability.
 */
struct KProcess *kprocess_alloc_from(struct KUntyped *pool,
                                     struct KCNode *cnode);
void             kprocess_free (struct KProcess *p);
void             kprocess_teardown(struct KProcess *p, struct task *exiting_thread);
void             kprocess_reap_address_space(struct KProcess *p);
/* Phase S1: kprocess_quota_{acquire,release}_notification retired (Untyped is
 * the budget for notifications).  VMO/page quotas remain for legacy objects. */
iris_error_t     kprocess_quota_acquire_vmo(struct KProcess *p);
void             kprocess_quota_release_vmo(struct KProcess *p);
iris_error_t     kprocess_quota_acquire_page(struct KProcess *p);
void             kprocess_quota_release_page(struct KProcess *p);
/* Phase 29 — global resource-accounting gauges (SYS_RESOURCE_INFO). */
uint32_t         kprocess_quota_failed_count(void);
uint32_t         kprocess_quota_rollback_count(void);
void             kprocess_quota_stat_rollback(void);
iris_error_t     kprocess_watch_exit(struct KProcess *p, struct KNotification *notif,
                                     uint64_t signal_bits);
iris_error_t     kprocess_set_exception_handler(struct KProcess *p,
                                                 struct KNotification *notif,
                                                 uint64_t signal_bits);
int              kprocess_notify_fault(struct task *t, uint64_t vector,
                                       uint64_t error_code, uint64_t rip, uint64_t cr2);
/* Phase 20: fault-model instrumentation + resume-time pending-fault clear. */
void             kprocess_fault_clear(struct KProcess *p, uint32_t task_id, int killed);
void             kprocess_fault_stat_nohandler(void);
uint32_t         kprocess_fault_delivery_count(void);
uint32_t         kprocess_fault_nohandler_count(void);
uint32_t         kprocess_fault_resume_count(void);
uint32_t         kprocess_fault_kill_count(void);
uint32_t         kprocess_fault_cleanup_count(void);
/* Phase 6.2: Bootstrap frame tracking.
 * kprocess_register_bootstrap_frame stores one alloc retain in bootstrap_frames[].
 * kprocess_release_bootstrap_frames drops all alloc retains; must be called after
 * kvspace_invalidate so that mapped_count is 0 when each frame is released. */
iris_error_t kprocess_register_bootstrap_frame(struct KProcess *p, struct KFrame *f);
void         kprocess_release_bootstrap_frames(struct KProcess *p);

/*
 * kprocess_live_count: count live KProcess objects currently allocated.
 *
 * Backed by an internal atomic counter updated on alloc/destroy. Useful as a
 * compact indicator of how many processes (not merely tasks) are alive.
 */
uint32_t kprocess_live_count(void);

static inline int kprocess_is_alive(const struct KProcess *p) {
    return p && p->thread_count > 0;
}

static inline int kprocess_teardown_complete(const struct KProcess *p) {
    return p && p->teardown_complete;
}

#endif
