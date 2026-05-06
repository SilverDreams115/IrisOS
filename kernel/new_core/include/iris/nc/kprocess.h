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
struct KNotification;

#define KPROCESS_VMO_MAP_MAX    0u  /* no fixed ceiling — kpage-backed linked list */
#define KPROCESS_EXIT_WATCH_MAX 4u
#define KPROCESS_CHANNEL_QUOTA  16u
#define KPROCESS_NOTIFICATION_QUOTA 16u
#define KPROCESS_VMO_QUOTA      32u

struct KExitWatch {
    struct KChannel *ch;
    handle_id_t      watched_handle;
    uint32_t         cookie;
    uint8_t          armed;
};

struct KVmoMapping {
    uint64_t     virt_base;
    uint64_t     size;
    struct KVmo *vmo;
    uint64_t     page_flags;
    struct KVmoMapping *next;
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
    uint8_t         teardown_complete; /* logical teardown already ran */
    uint8_t         aspace_reaped;     /* address space cleanup already ran */
    struct KExitWatch exit_watches[KPROCESS_EXIT_WATCH_MAX]; /* up to 4 death subscribers */
    struct KVmoMapping *vmo_mappings;  /* demand VMO registrations */
    uint32_t        vmo_mapping_count;
    HandleTable     handle_table;/* process-scoped handles/capabilities */

    /* Exception handler channel: if non-NULL, receives a FAULT_MSG_NOTIFY
     * message before the faulting task is killed.  Retained by the process. */
    struct KChannel *exception_chan;
    uint32_t owned_channels;
    uint32_t owned_notifications;
    uint32_t owned_vmos;

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
