#ifndef IRIS_NC_KTCB_H
#define IRIS_NC_KTCB_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <stdint.h>

struct task;

/*
 * Fase S2 D2 — the KTCB is `struct task` itself (KObject at offset 0).  The old
 * `struct KTcb { KObject; struct task *task; }` wrapper is REMOVED: there is one
 * canonical structure with one object identity and four separated lifetimes
 * (object / execution / registry / storage — see sel4-task-model.md).
 *
 * A KOBJ_TCB capability is a KObject* aliasing &task->base.  Handle-table caps
 * hold object references; the scheduler holds one "execution reference" from
 * creation, dropped at termination.  When the last reference drops, the object
 * destructor frees the backing storage.
 */

/* Placement-init the KObject header + object identity on a task backing slot.
 * Sets refcount = 1 (the creator/scheduler execution reference). */
void ktcb_object_init(struct task *t);

/* Object destructor hook (called by task_lifecycle when refcount hits 0) — see
 * task_backing_free_on_destroy in task_lifecycle.c. */

/* Fase 18/S2 — object/execution/registry gauges (SYS_UNTYPED_QUERY kind 4). */
uint32_t ktcb_live_count(void);              /* live KTCB objects (incl. terminated-with-caps) */
void     ktcb_stats(uint32_t *live, uint32_t *hwm,
                    uint32_t *retyped, uint32_t *destroyed);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KTCB_H */
