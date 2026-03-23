#ifndef IRIS_NC_KPROCESS_H
#define IRIS_NC_KPROCESS_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/handle_table.h>
#include <iris/paging.h>
#include <iris/task.h>
#include <stdint.h>

/*
 * KProcess — process control object.
 *
 * Owns process-scoped resources and points to the initial thread
 * (which lives in the kernel's static tasks[] array).
 *
 * The KProcess and the initial thread have independent lifecycles:
 *   - main_thread->state becomes TASK_DEAD when the process exits.
 *   - The KProcess (and handles pointing to it) lives until its
 *     refcount drops to zero (all handles closed).
 *
 * Lifecycle split:
 *   - kprocess_teardown(): logical process teardown while task/context still
 *     exists; closes process-scoped handles and unregisters global ownership.
 *   - kprocess_reap_address_space(): runs only after switching away from the
 *     exiting task's CR3; frees the process-owned address space.
 *   - destroy(): final object cleanup when refcount reaches zero. It may finish
 *     any missing idempotent cleanup, but must not touch task-local resources
 *     such as the user stack or scheduler linkage.
 *
 * Invariants:
 *   - base must be first (KObject cast rules).
 *   - main_thread is set after thread creation and cleared once teardown starts.
 *   - teardown_complete == 1 means logical teardown already ran.
 *   - aspace_reaped == 1 means cr3-owned memory is already gone.
 *   - Before final destroy of an exited process, task-local resources must
 *     already have been released by task_exit_current()/reaper paths.
 */
struct KProcess {
    struct KObject  base;   /* must be first */
    struct task    *main_thread; /* may be NULL briefly during bootstrap */
    uint64_t        cr3;         /* page table root for the process */
    uint64_t        brk;         /* process heap break */
    uint8_t         teardown_complete; /* logical teardown already ran */
    uint8_t         aspace_reaped;     /* address space cleanup already ran */
    /*
     * ns_authority: set once by the kernel at bootstrap time for the
     * service manager process.  Controls whether SYS_NS_REGISTER is
     * permitted for this process.
     *
     * Authority model: the flag is a property of the KProcess object
     * itself, not of a module-global pointer.  This means authority
     * does not become stale if the service manager is ever restarted
     * (new KProcess → new flag grant), and there is no external
     * coupling to a named svcmgr identity in the syscall layer.
     *
     * Set via kprocess_set_ns_authority().  Never cleared after set.
     * Only one process holds this flag at any given time by convention.
     */
    uint8_t         ns_authority;
    uint8_t         exit_watch_armed; /* one death subscriber registered */
    handle_id_t     exit_watch_handle;/* subscriber's proc_handle id for callbacks */
    uint32_t        exit_watch_cookie;/* subscriber-defined cookie echoed on death */
    struct KChannel *exit_watch_ch;   /* retained channel for death event delivery */
    HandleTable     handle_table;/* process-scoped handles/capabilities */
};

#define KPROCESS_POOL_SIZE 16  /* maximum live KProcess objects system-wide */

struct KChannel;
struct KProcess *kprocess_alloc(void);
void             kprocess_free (struct KProcess *p);
void             kprocess_teardown(struct KProcess *p, struct task *exiting_thread);
void             kprocess_reap_address_space(struct KProcess *p);
iris_error_t     kprocess_watch_exit(struct KProcess *p, struct KChannel *ch,
                                     handle_id_t watched_handle, uint32_t cookie);

static inline int kprocess_is_alive(const struct KProcess *p) {
    return p && p->main_thread && p->main_thread->state != TASK_DEAD;
}

static inline int kprocess_teardown_complete(const struct KProcess *p) {
    return p && p->teardown_complete;
}

/*
 * kprocess_set_ns_authority: grant NS registration authority to p.
 * Must be called exactly once, at bootstrap time, for the service
 * manager process.  Must be called before svcmgr runs (before any
 * task_yield after the spawn), to ensure visibility.
 */
static inline void kprocess_set_ns_authority(struct KProcess *p) {
    if (p) p->ns_authority = 1;
}

/*
 * kprocess_has_ns_authority: returns non-zero if p may call
 * SYS_NS_REGISTER.  Used by sys_ns_register to enforce authority.
 */
static inline int kprocess_has_ns_authority(const struct KProcess *p) {
    return p && p->ns_authority;
}

#endif
