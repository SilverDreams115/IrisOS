#ifndef IRIS_NC_KPROCESS_H
#define IRIS_NC_KPROCESS_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/handle_table.h>
#include <iris/paging.h>
#include <iris/task.h>
#include <stdint.h>

struct KVmo;
struct KNotification;
struct KVSpace;
struct KFrame;

#define KPROCESS_EXIT_WATCH_MAX 8u
#define KPROCESS_MAX_LIVE       64u /* bounded by TASK_MAX; enforced in kprocess_alloc */
#define KPROCESS_CHANNEL_QUOTA  16u
#define KPROCESS_NOTIFICATION_QUOTA 16u
#define KPROCESS_VMO_QUOTA      32u
#define KPROCESS_PHYS_PAGES_LIMIT 2048u /* 8MB per process; set in kprocess_alloc */

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
 *   - teardown_complete == 1 means logical teardown already ran.
 *   - aspace_reaped == 1 means cr3-owned memory is already gone.
 *   - Before final destroy of an exited process, task-local resources must
 *     already have been released by task_exit_current()/reaper paths.
 */
struct KProcess {
    struct KObject  base;       /* must be first */
    uint32_t        thread_count; /* live threads in this process; 0 = dead */
    uint64_t        cr3;          /* page table root for the process */
    uint64_t        user_cr3;     /* cr3|pcid|(1<<63 if PCID) — no-flush variant for iretq */
    uint16_t        pcid;         /* PCID assigned at alloc (0 = unused/PCID disabled) */
    uint8_t         teardown_complete; /* logical teardown already ran */
    uint32_t        exit_code;    /* exit code from SYS_EXIT; 0 if killed externally */
    uint8_t         aspace_reaped;     /* address space cleanup already ran */
    struct KExitWatch exit_watches[KPROCESS_EXIT_WATCH_MAX]; /* up to 4 death subscribers */
    /* Fase 6.3: vmo_mappings removed — VMO pages are now KFrame-backed and
     * tracked in KVSpace.mappings; kvspace_invalidate handles teardown. */
    HandleTable     handle_table;/* process-scoped handles/capabilities */

    /* Exception handler channel: if non-NULL, receives a FAULT_MSG_NOTIFY
     * message before the faulting task is killed.  Retained by the process. */
    struct KChannel *exception_chan;
    uint32_t owned_channels;
    uint32_t owned_notifications;
    uint32_t owned_vmos;
    uint32_t phys_pages_charged; /* sparse-VMO pages charged at eager map-time
                                  * allocation; vs phys_pages_limit */
    uint32_t phys_pages_limit;   /* set to KPROCESS_PHYS_PAGES_LIMIT at alloc */

    /* Ph95 (Phase 8): root CNode handle for hierarchical CSpace traversal.
     * HANDLE_INVALID if not yet allocated (e.g. kpage_alloc OOM at creation). */
    handle_id_t cspace_root_h;

    /* Fase 4: VSpace capability wrapping this process's address space.
     * Holds one lifecycle ref (kobject_retain).  NULL if kvspace_alloc OOM'd at
     * creation or if the process has not yet had its CR3 assigned. */
    struct KVSpace *vspace;

    /* Fase 6.2: Bootstrap KFrame alloc retains.
     * Populated by task_create_user_impl for each page mapped via
     * bootstrap_kframe_map.  Released by kprocess_release_bootstrap_frames
     * inside kprocess_reap_address_space, always AFTER kvspace_invalidate
     * so mapped_count is 0 at the time each alloc retain is dropped. */
    struct KFrame   *bootstrap_frames[KPROCESS_BOOTSTRAP_FRAME_MAX];
    uint32_t         bootstrap_frame_count;

};

#define KPROCESS_POOL_SIZE 0u  /* no static pool — kpage-backed; 0 = unbounded allocator ceiling */

struct KChannel;
struct KProcess *kprocess_alloc(void);
void             kprocess_free (struct KProcess *p);
void             kprocess_teardown(struct KProcess *p, struct task *exiting_thread);
void             kprocess_reap_address_space(struct KProcess *p);
iris_error_t     kprocess_quota_acquire_channel(struct KProcess *p);
void             kprocess_quota_release_channel(struct KProcess *p);
iris_error_t     kprocess_quota_acquire_notification(struct KProcess *p);
void             kprocess_quota_release_notification(struct KProcess *p);
iris_error_t     kprocess_quota_acquire_vmo(struct KProcess *p);
void             kprocess_quota_release_vmo(struct KProcess *p);
iris_error_t     kprocess_quota_acquire_page(struct KProcess *p);
void             kprocess_quota_release_page(struct KProcess *p);
iris_error_t     kprocess_watch_exit(struct KProcess *p, struct KNotification *notif,
                                     uint64_t signal_bits);
iris_error_t     kprocess_set_exception_handler(struct KProcess *p, struct KChannel *ch);
int              kprocess_notify_fault(struct task *t, uint64_t vector,
                                       uint64_t error_code, uint64_t rip, uint64_t cr2);
/* Fase 6.2: Bootstrap frame tracking.
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
