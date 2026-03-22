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
 * Invariants:
 *   - base must be first (KObject cast rules).
 *   - main_thread is set after thread creation and never changed.
 *   - destroy() frees the pool slot only; does not touch the task.
 */
struct KProcess {
    struct KObject  base;   /* must be first */
    struct task    *main_thread; /* may be NULL briefly during bootstrap */
    uint64_t        cr3;         /* page table root for the process */
    uint64_t        brk;         /* process heap break */
    HandleTable     handle_table;/* process-scoped handles/capabilities */
};

struct KProcess *kprocess_alloc(void);
void             kprocess_free (struct KProcess *p);

static inline int kprocess_is_alive(const struct KProcess *p) {
    return p && p->main_thread && p->main_thread->state != TASK_DEAD;
}

#endif
