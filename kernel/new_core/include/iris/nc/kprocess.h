#ifndef IRIS_NC_KPROCESS_H
#define IRIS_NC_KPROCESS_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/paging.h>
#include <iris/task.h>
#include <stdint.h>

struct KNotification;
struct KVSpace;
struct KFrame;

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
/* KPROCESS_VMO_QUOTA / KPROCESS_PHYS_PAGES_LIMIT DELETED (Stage 7-mem) — the
 * per-process VMO ceiling and the page counter went with the owner relation.
 * A VMO's accounting is the Untyped it was carved from. */
/* Stage 7: RETIRED.  The per-process page ceiling was a number the kernel
 * invented; since Stage 6-pure a VMO's pages come from an Untyped the caller
 * named, and exhausting THAT is what running out means.  phys_pages_limit
 * reports 0 — "no kernel ceiling" — the way the notification quota did when it
 * retired in Phase S1.  The constant is kept only so the retirement is legible. */


/*
 * struct KProcess DELETED (Stage 7-proc).
 *
 * What is left in this file is the FAULT machinery and the global gauges,
 * which never belonged to a process — a fault is taken by a thread and the
 * counters are facts about the kernel.  The file keeps its name until the
 * symbols are renamed; nothing here allocates, owns or names a process.
 */

#define KPROCESS_POOL_SIZE 0u  /* no static pool — kpage-backed; 0 = unbounded allocator ceiling */


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
/* Join a thread to `p` — thread_count++ and threads_ever = 1 — or refuse with
 * IRIS_ERR_BAD_HANDLE because teardown has already been claimed.  Test and act
 * under one lock hold; see kprocess.c for why a separate flag read is not a
 * gate.  Every path that gives a thread a process goes through this. */
/* Phase S1: kprocess_quota_{acquire,release}_notification retired (Untyped is
 * the budget for notifications).  VMO/page quotas remain for legacy objects. */
/* Phase 29 — global resource-accounting gauges (SYS_RESOURCE_INFO). */
uint32_t         kprocess_quota_failed_count(void);
uint32_t         kprocess_quota_rollback_count(void);
void             kprocess_quota_stat_rollback(void);
int              kprocess_notify_fault(struct task *t, uint64_t vector,
                                       uint64_t error_code, uint64_t rip, uint64_t cr2);
/* Stage 8-mcs: deliver a TIMEOUT fault to the thread's timeout handler.
 * Returns 0 when none is armed — the caller then falls back to blocking the
 * thread until its period refills the budget. */
int              ktimeout_notify_fault(struct task *t);
/* Phase 20: fault-model instrumentation + resume-time pending-fault clear. */
/* Drop `ft`'s pending fault record, and the process's pointer to it if that is
 * the one it names.  Takes the THREAD rather than its id: the caller has
 * already resolved it, and an id comparison here was a second place the
 * kernel selected a thread by number. */
void             kfault_resolve(struct task *ft, int killed);
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

/*
 * kprocess_live_count: count live KProcess objects currently allocated.
 *
 * Backed by an internal atomic counter updated on alloc/destroy. Useful as a
 * compact indicator of how many processes (not merely tasks) are alive.
 */

/* kprocess_is_alive DELETED (Stage 7 Step 13) — it backed SYS_PROCESS_STATUS,
 * and liveness is the EXECUTION's state (SYS_TCB_GET_INFO), not a bit derived
 * from a thread count somebody else owns. */

/* kprocess_teardown_complete DELETED (Stage 7-proc) with the object it asked
 * about.  A thread's terminal flag is the equivalent question, asked of the
 * execution that answers it. */

#endif
