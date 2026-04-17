#ifndef IRIS_NC_KPROCESS_H
#define IRIS_NC_KPROCESS_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/handle_table.h>
#include <iris/paging.h>
#include <iris/task.h>
#include <iris/elf_loader.h>
#include <stdint.h>

struct KVmo;

#define KPROCESS_VMO_MAP_MAX 8u

struct KVmoMapping {
    uint64_t     virt_base;
    uint64_t     size;
    struct KVmo *vmo;         /* NULL = free slot */
    uint64_t     page_flags;
};

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
    uint8_t         teardown_complete; /* logical teardown already ran */
    uint8_t         aspace_reaped;     /* address space cleanup already ran */
    uint8_t         exit_watch_armed; /* one death subscriber registered */
    handle_id_t     exit_watch_handle;/* subscriber's proc_handle id for callbacks */
    uint32_t        exit_watch_cookie;/* subscriber-defined cookie echoed on death */
    struct KChannel *exit_watch_ch;   /* retained channel for death event delivery */
    struct KVmoMapping vmo_mappings[KPROCESS_VMO_MAP_MAX]; /* demand VMO registrations */
    HandleTable     handle_table;/* process-scoped handles/capabilities */

    /* Exception handler channel: if non-NULL, receives a FAULT_MSG_NOTIFY
     * message before the faulting task is killed.  Retained by the process. */
    struct KChannel *exception_chan;

    /*
     * ELF segment tracking — populated by task_spawn_elf after a successful
     * elf_loader_load.  kprocess_reap_address_space iterates these to free
     * segment backing pages before destroying the page tables.
     *
     * For kernel-linked user tasks (task_spawn_user / task_create_user_impl)
     * elf_seg_count == 0 and these fields are unused.
     */
    uint32_t elf_seg_count;
    struct {
        uint64_t phys_base;   /* physical base of segment pages */
        uint32_t page_count;  /* number of 4 KiB pages */
    } elf_segs[ELF_LOADER_MAX_LOAD_SEGS];
};

#define KPROCESS_POOL_SIZE 32  /* maximum live KProcess objects system-wide */

struct KChannel;
struct KProcess *kprocess_alloc(void);
void             kprocess_free (struct KProcess *p);
void             kprocess_teardown(struct KProcess *p, struct task *exiting_thread);
void             kprocess_reap_address_space(struct KProcess *p);
iris_error_t     kprocess_watch_exit(struct KProcess *p, struct KChannel *ch,
                                     handle_id_t watched_handle, uint32_t cookie);
iris_error_t     kprocess_set_exception_handler(struct KProcess *p, struct KChannel *ch);
void             kprocess_notify_fault(struct task *t, uint64_t vector,
                                       uint64_t error_code, uint64_t rip, uint64_t cr2);
iris_error_t     kprocess_register_vmo_map  (struct KProcess *p, uint64_t virt_base,
                                              uint64_t size, struct KVmo *vmo,
                                              uint64_t page_flags);
void             kprocess_unregister_vmo_map(struct KProcess *p, uint64_t virt_base);
iris_error_t     kprocess_resolve_demand_fault(struct task *t, uint64_t fault_addr);

/*
 * kprocess_live_count: count KProcess pool slots currently in use.
 *
 * Iterates pool_used[KPROCESS_POOL_SIZE].  Useful as a compact indicator of
 * how many processes (not merely tasks) are alive.  Called from
 * sys_diag_snapshot; safe without additional locking (byte-wide reads).
 */
uint32_t kprocess_live_count(void);

static inline int kprocess_is_alive(const struct KProcess *p) {
    return p && p->main_thread && p->main_thread->state != TASK_DEAD;
}

static inline int kprocess_teardown_complete(const struct KProcess *p) {
    return p && p->teardown_complete;
}

#endif
