#ifndef IRIS_SCHED_PRIV_H
#define IRIS_SCHED_PRIV_H

/*
 * scheduler_priv.h — internal declarations shared across the three scheduler
 * translation units: kstack.c, task_lifecycle.c, scheduler_core.c (scheduler.c).
 *
 * Nothing in this header is part of the public kernel API.  Include
 * <iris/task.h> or <iris/scheduler.h> from external callers instead.
 *
 * SMP readiness: irq_spinlock_t is used for PMM, futex, and klog.
 * cpu_local[cpu_id].current_task is kept in sync with current_task on every
 * context switch.  IA32_GS_BASE is wired to &cpu_local[0] by gdt_init() (Fase 2:
 * ring-0 always has GS_BASE = &cpu_local[cpu_id]; cpu_self() safe everywhere).
 * AP bringup and LAPIC timer are deferred; infrastructure is correct for BSP.
 */

#include <iris/task.h>
#include <iris/cpu_local.h>
#include <iris/nc/spinlock.h>
#include <stdint.h>
#include <stdatomic.h>

/*
 * CpuRunQueue — per-CPU O(1) priority run queue.
 * Each CPU owns one; tasks are assigned to a CPU at creation (home_cpu).
 * Cross-CPU wakeup enqueues into the target CPU's queue then sends an IPI.
 */
struct CpuRunQueue {
    irq_spinlock_t lock;
    int            head[256];      /* head task index per priority, -1=empty */
    int            tail[256];      /* tail task index per priority, -1=empty */
    int            next[TASK_MAX]; /* next index in same-prio FIFO, -1=end   */
    int            queued[TASK_MAX]; /* 1 if task is enqueued                 */
    uint64_t       mask[4];        /* 256-bit: bit p set ↔ prio-p non-empty  */
};

/* ── Shared state (defined in task_lifecycle.c) ──────────────────────────── */

extern struct task         tasks[TASK_MAX];
extern struct task        *current_task;
extern struct task        *task_list_head;
extern struct task        *task_list_tail;
extern uint32_t            next_id;
extern uint64_t            task_rsp[TASK_MAX];
extern uint64_t            kernel_cr3;
extern uint8_t             initial_fpu_state[512];

/*
 * set_current_task — update both the global current_task and the per-CPU
 * cpu_local.current_task.  Must only be called from post-SWAPGS ring-0 context
 * (syscall entry or ISR entry) where GS_BASE = &cpu_local[cpu_id].
 *
 * Boot-time task_init() sets the idle task directly via cpu_local[0] array
 * access (pre-SWAPGS) and does NOT call this helper.
 */
static inline void set_current_task(struct task *t) {
    current_task = t;
    cpu_self()->current_task = t;
}

/* Scheduler tick counter (defined in scheduler.c) */
extern volatile uint64_t scheduler_ticks;

/* ── Architecture helpers ────────────────────────────────────────────────── */

extern void user_entry_trampoline(void);

extern void context_switch(struct cpu_context *old,
                            struct cpu_context *new,
                            uint64_t *old_rsp,
                            uint64_t  new_rsp,
                            uint8_t  *old_fpu,
                            uint8_t  *new_fpu);

/* ── kstack.c ────────────────────────────────────────────────────────────── */

#define KSTACK_PAGE_SIZE 0x1000ULL

void kstack_panic(const char *msg);
int  kstack_alloc(struct task *t, int idx);
void kstack_free (struct task *t, int idx);

/* ── task_lifecycle.c (cross-file helpers) ───────────────────────────────── */

void task_init_fpu_state(struct task *t);
void task_reset_slot(struct task *t);
void unlink_task(struct task *t);
void reap_enqueue_dead(struct task *t);
void reap_pending_dead_task(void);

/* ── O(1) run queue (defined in task_lifecycle.c) ────────────────────────── */

extern _Atomic uint32_t  sched_live_count;  /* live (non-dead) task count */

void         rq_enqueue(struct task *t);
void         rq_remove(struct task *t);
struct task *rq_dequeue_best(void);
int          rq_top_priority(void);

#endif /* IRIS_SCHED_PRIV_H */
