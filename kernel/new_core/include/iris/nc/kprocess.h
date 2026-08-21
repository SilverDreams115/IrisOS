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
/*
 * Stage 7 Step 3: KPROCESS_MAX_LIVE RETIRES.
 *
 * 64 was an invented ceiling of the same class as the page quota Step 2
 * removed — a number the kernel picked, refused at, and could not be asked to
 * raise.  Since Stage 6 Step 4 a spawned KProcess is a child block of an
 * Untyped its creator named, and its root CNode comes from the same region, so
 * what bounds how many processes exist is how much memory somebody delegated.
 * Refusing at 64 on top of that told a holder with a large budget that it had
 * run out when it had not, and told a holder with a tiny one nothing at all.
 *
 * What bounds a spawn now, all of it derived rather than declared:
 *   - the creator's Untyped, which pays for the KProcess header, the root
 *     CNode, the PML4 and every paging level (a bump allocator that does not
 *     rewind, so the bound is real and the holder can measure it);
 *   - TASK_MAX (256), the thread registry, for a process that runs a thread —
 *     a separate resource with a separate ceiling, which is why conflating the
 *     two in one constant was wrong even as an approximation;
 *   - the PCID pool (1..4094) when PCID is enabled, which is hardware.
 *
 * kprocess_live_count() survives as INSTRUMENTATION, exposed through
 * SYS_SCHED_INFO — the same fate the notification quota had in Phase S1 and
 * the page quota had in Step 2.  What is gone is the refusal.
 */
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
    /* Stage 7 Step 5: `cr3` is a CACHE of vspace->cr3, kept only for the two
     * places that still ask a PROCESS about its address space — the teardown
     * gate and kprocess_reap_address_space.  user_cr3 and pcid are gone: they
     * described a walk, so they live on the walk (struct KVSpace), and the
     * scheduler reads them from the thread's own VSpace. */
    uint64_t        cr3;          /* == vspace->cr3 while the process is alive */
    uint8_t         teardown_complete; /* logical teardown already ran */
    /* exit_code DELETED (Stage 7 Step 15) — the code belongs to the execution
     * that produced it (struct task), and SYS_PROCESS_EXIT_CODE, the only
     * thing that ever read the process copy, retired in Step 10. */
    uint8_t         aspace_reaped;     /* address space cleanup already ran */
    /* Stage 7 Step 10: nothing ARMS these any more — SYS_PROCESS_WATCH is
     * retired and a death is watched on the thread.  The array and its
     * emit/clear pair are dead weight kept only until KProcess itself goes,
     * and are what a process server replaces with its own child table. */
    struct KExitWatch exit_watches[KPROCESS_EXIT_WATCH_MAX];
    /* Phase 6.3: vmo_mappings removed — VMO pages are now KFrame-backed and
     * tracked in KVSpace.mappings; kvspace_invalidate handles teardown. */

    /* Stage 7 Step 12: exception delivery is entirely the THREAD's — the
     * handler notification, its signal bits, the mailbox CNode and slot, and
     * the fault record all live on struct task.  What stood here was the
     * process-scoped version of every one of them, kept alive by one thing: a
     * handler could only name the faulting thread by id, so the registration
     * had to hang off something a supervisor could hold.  Step 7 gave it the
     * thread's capability and Step 12 moved the registration to match. */
    /* Stage 6 Step 2: the creation reference — the one that lets a running
     * process outlive the last capability to it — is dropped exactly once.
     * Before this flag, a process created and never STARTED could not be
     * reclaimed at all: kill saw no threads and returned success without
     * dropping it, so the KProcess, its VSpace, its PML4 and (now) its
     * page-table budget stayed alive with no way to get them back. */
    uint8_t  initial_ref_dropped;
    /*
     * Stage 6 Step 4: the Untyped this object's own block was carved from,
     * retained for its lifetime.  NULL = kernel-slab (root task).
     *
     * Stage 7 Step 14 renamed it from `mem_pool`, because that is all it does
     * now.  It used to be the DEFAULT BUDGET: the Untyped the kernel charged
     * an allocation to when a syscall did not name one — device capabilities,
     * initrd image copies, anonymous VMOs.  Every one of those takes the
     * budget as a required argument now, so nothing reads this to decide whose
     * memory pays.  What is left is the storage anchor: the block lives inside
     * the region this pool owns, so the pool must outlive the block.
     */
    struct KUntyped *storage_pool;
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
    uint32_t phys_pages_limit;   /* KPROCESS_PHYS_PAGES_LIMIT (0) since Stage 7:
                                  * reported, never enforced — see the constant */
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
/* Join a thread to `p` — thread_count++ and threads_ever = 1 — or refuse with
 * IRIS_ERR_BAD_HANDLE because teardown has already been claimed.  Test and act
 * under one lock hold; see kprocess.c for why a separate flag read is not a
 * gate.  Every path that gives a thread a process goes through this. */
iris_error_t     kprocess_attach_thread(struct KProcess *p);
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
int              kprocess_notify_fault(struct task *t, uint64_t vector,
                                       uint64_t error_code, uint64_t rip, uint64_t cr2);
/* Phase 20: fault-model instrumentation + resume-time pending-fault clear. */
/* Drop `ft`'s pending fault record, and the process's pointer to it if that is
 * the one it names.  Takes the THREAD rather than its id: the caller has
 * already resolved it, and an id comparison here was a second place the
 * kernel selected a thread by number. */
void             kprocess_fault_clear(struct KProcess *p, struct task *ft, int killed);
void             kprocess_fault_stat_nohandler(void);
uint32_t         kprocess_fault_delivery_count(void);
uint32_t         kprocess_fault_nohandler_count(void);
uint32_t         kprocess_fault_resume_count(void);
uint32_t         kprocess_fault_kill_count(void);
uint32_t         kprocess_fault_cleanup_count(void);
void             kprocess_fault_stat_cleanup(void);
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

/* kprocess_is_alive DELETED (Stage 7 Step 13) — it backed SYS_PROCESS_STATUS,
 * and liveness is the EXECUTION's state (SYS_TCB_GET_INFO), not a bit derived
 * from a thread count somebody else owns. */

static inline int kprocess_teardown_complete(const struct KProcess *p) {
    return p && p->teardown_complete;
}

#endif
