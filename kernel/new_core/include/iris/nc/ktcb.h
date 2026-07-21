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
 * Sets refcount = 1 (the creator/scheduler execution reference).  Pool births
 * are pre-configured (configured = 1): their execution state is fully built
 * by the task_lifecycle creation path before they are ever visible. */
void ktcb_object_init(struct task *t);

/*
 * Fase S2 Etapa 0 (charter §2.2/O1) — canonical TCB birth from Untyped.
 *
 * ktcb_alloc_at: placement-init a KTCB whose storage IS the retyped region
 * (RETYPE2(KOBJ_TCB) path; block zero-filled by kuntyped_alloc_children_atomic;
 * destructor returns it via kuntyped_release_child).  The object is born
 * INACTIVE: configured = 0, no registry slot, no kstack, no process — a full
 * capability citizen (GET_INFO / SET_PRIORITY / delete / transfer) that cannot
 * execute until TCB_CONFIGURE exists (roadmap Etapa 5/6; the execution path
 * for now remains SYS_THREAD_CREATE, ledger: ACTIVE_LEGACY).  Execution
 * syscalls on an unconfigured TCB fail NOT_SUPPORTED without side effects.
 */
struct task *ktcb_alloc_at(void *mem);

/* Object destructor hook (called by task_lifecycle when refcount hits 0) — see
 * task_backing_free_on_destroy in task_lifecycle.c. */

/* Fase 18/S2 — object/execution/registry gauges (SYS_UNTYPED_QUERY kind 4). */
uint32_t ktcb_live_count(void);              /* live KTCB objects (incl. terminated-with-caps) */
void     ktcb_stats(uint32_t *live, uint32_t *hwm,
                    uint32_t *retyped, uint32_t *destroyed);

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KTCB_H */
