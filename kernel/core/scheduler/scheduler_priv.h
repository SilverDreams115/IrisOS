#ifndef IRIS_SCHED_PRIV_H
#define IRIS_SCHED_PRIV_H

/*
 * scheduler_priv.h — internal declarations shared across the three scheduler
 * translation units: kstack.c, task_lifecycle.c, scheduler_core.c (scheduler.c).
 *
 * Nothing in this header is part of the public kernel API.  Include
 * <iris/task.h> or <iris/scheduler.h> from external callers instead.
 *
 * SMP readiness (Phase 58+): irq_spinlock_t is used for PMM, futex, and klog.
 * cpu_local[cpu_id].current_task is kept in sync with the current_task global
 * on every context switch.  GSBASE is wired to &cpu_local[0] by gdt_init().
 * AP bringup, LAPIC timer, and GS-relative task_current() are deferred to
 * a later phase; the infrastructure is correct for single-CPU use today.
 */

#include <iris/task.h>
#include <iris/cpu_local.h>
#include <stdint.h>
#include <stdatomic.h>

/* ── Shared state (defined in task_lifecycle.c) ──────────────────────────── */

extern struct task         tasks[TASK_MAX];
extern struct task        *current_task;
extern struct task        *task_list_head;
extern struct task        *task_list_tail;
extern _Atomic(struct task *) pending_reap_task;
extern uint32_t            next_id;
extern uint64_t            task_rsp[TASK_MAX];
extern uint64_t            kernel_cr3;
extern uint8_t             initial_fpu_state[512];

/*
 * set_current_task — atomically update both the global current_task and the
 * per-CPU cpu_local[cpu_id].current_task.  All scheduler context switches must
 * go through this helper so the GS-relative path (task_current via cpu_self())
 * stays coherent with the global on every CPU.
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
void reap_pending_dead_task(void);

#endif /* IRIS_SCHED_PRIV_H */
