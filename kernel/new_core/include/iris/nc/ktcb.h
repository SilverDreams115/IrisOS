#ifndef IRIS_NC_KTCB_H
#define IRIS_NC_KTCB_H

#ifdef __KERNEL__
#include <iris/nc/kobject.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>
#include <stdint.h>

struct task;

/*
 * KTcb — thread control block capability.
 *
 * Wraps a single struct task *.  The pointer is NULLed when the thread exits
 * (task_exit_current / task_kill_external call ktcb_nullify before releasing
 * the kernel's own ref).  Callers must hold the lock while inspecting task.
 *
 * task_id is a snapshot of t->id taken at creation; remains valid after death
 * so SYS_TCB_GET_INFO can return the ID even if the thread is dead.
 */
struct KTcb {
    struct KObject  base;     /* must be first */
    irq_spinlock_t  lock;
    struct task    *task;     /* NULL when thread is dead */
    uint32_t        task_id;  /* snapshot of t->id at creation */
};

struct KTcb *ktcb_alloc(struct task *t);
void         ktcb_nullify(struct KTcb *tcb); /* sets task=NULL; called on thread death */
void         ktcb_free(struct KTcb *tcb);    /* kobject_release wrapper */

#endif /* __KERNEL__ */
#endif /* IRIS_NC_KTCB_H */
