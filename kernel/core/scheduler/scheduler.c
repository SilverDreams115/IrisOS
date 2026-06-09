#include "scheduler_priv.h"
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kschedctx.h>
#include <stdint.h>

/*
 * scheduler.c — scheduler loop: yield, tick, sleep, diagnostics.
 *
 * Task creation/teardown lives in task_lifecycle.c.
 * Kernel stack management lives in kstack.c.
 */

volatile uint64_t scheduler_ticks = 0;
/* wall_ticks is incremented only by the real PIT ISR path (scheduler_tick).
 * Unlike scheduler_ticks it is never fast-forwarded by the idle-loop clock
 * workaround, so it reflects real elapsed time and is used by SYS_CLOCK_GET. */
static volatile uint64_t wall_ticks = 0;

/*
 * sched_handle_idle — fast-forward clock when the idle task is current and no
 * non-idle task is runnable.  Advances scheduler_ticks to the nearest sleeping
 * task's wake_tick so timed-out tasks become READY even when the timer ISR does
 * not fire (QEMU TCG: no IRQs delivered during ring-0 spin).
 *
 * On return *out_chosen is set to the first runnable non-idle task found after
 * the fast-forward, or remains NULL if none.
 */
static void sched_handle_idle(struct task *idle, struct task **out_chosen) {
    /* Fast-forward clock to nearest deadline so timed tasks wake even with no IRQs. */
    uint64_t min_wake = UINT64_MAX;
    for (int j = 0; j < TASK_MAX; j++) {
        if (tasks[j].wake_tick != 0 && tasks[j].wake_tick < min_wake)
            min_wake = tasks[j].wake_tick;
    }
    if (min_wake != UINT64_MAX && min_wake > scheduler_ticks)
        scheduler_ticks = min_wake;

    /* Wake any tasks whose deadlines passed and enqueue them. */
    for (int j = 0; j < TASK_MAX; j++) {
        if (&tasks[j] == idle) continue;
        if (tasks[j].state == TASK_SLEEPING &&
            tasks[j].wake_tick != 0 &&
            tasks[j].wake_tick <= scheduler_ticks) {
            tasks[j].wake_tick = 0;
            task_wakeup(&tasks[j]);
        } else if (tasks[j].state == TASK_BUDGET_EXHAUSTED &&
                   tasks[j].wake_tick != 0 &&
                   tasks[j].wake_tick <= scheduler_ticks) {
            if (tasks[j].sched_ctx)
                tasks[j].sched_ctx->remaining_budget = tasks[j].sched_ctx->budget_ticks;
            tasks[j].wake_tick = 0;
            task_wakeup(&tasks[j]);
        } else if ((tasks[j].state == TASK_BLOCKED_IPC ||
                    tasks[j].state == TASK_BLOCKED_IRQ) &&
                   tasks[j].wake_tick != 0 &&
                   tasks[j].wake_tick <= scheduler_ticks) {
            tasks[j].timed_out = 1;
            tasks[j].wake_tick = 0;
            task_wakeup(&tasks[j]);
        }
    }
    *out_chosen = rq_dequeue_best();
}

void task_yield(void) {
    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) : : "memory");

    reap_pending_dead_task();

    struct task *old  = current_task;
    struct task *idle = task_list_head;

    /* O(1): dequeue highest-priority READY non-idle task. */
    struct task *chosen = rq_dequeue_best();

    if (!chosen) {
        if (old == idle)
            sched_handle_idle(idle, &chosen);

        if (!chosen) {
            if (old != idle && task_is_runnable(idle->state)) {
                chosen = idle;
            } else {
                __asm__ volatile ("pushq %0; popfq" : : "r"(saved_flags) : "memory");
                return;
            }
        }
    }

    /* Re-enqueue old if it was preempted (state still RUNNING) so it
     * stays schedulable; do this after finding chosen to avoid dequeuing
     * old as the winner. */
    if (old->state == TASK_RUNNING) {
        old->state = TASK_READY;
        if (old != idle)
            rq_enqueue(old);
    }

    /* Avoid switching to ourselves (can occur if old == chosen after re-enqueue). */
    if (chosen == old) {
        old->state = TASK_RUNNING;
        __asm__ volatile ("pushq %0; popfq" : : "r"(saved_flags) : "memory");
        return;
    }

    if (old->state == TASK_DEAD)
        reap_enqueue_dead(old);

    chosen->state      = TASK_RUNNING;
    chosen->ticks_left = chosen->time_slice;
    chosen->need_resched = 0;
    set_current_task(chosen);
    cpu_self()->context_switches++;

    int old_idx = (int)(old - tasks);
    int new_idx = (int)(chosen - tasks);

    uint64_t new_kstack_top = (uint64_t)(uintptr_t)(chosen->kstack + TASK_STACK_SIZE);
    tss_set_rsp0(new_kstack_top);
    syscall_set_kstack(new_kstack_top);
    syscall_set_user_cr3(chosen->process ? chosen->process->user_cr3 : 0);

    if (chosen->process && chosen->process->cr3 != 0) {
        uint64_t cr3 = chosen->process->cr3;
        if (iris_pcid_enabled)
            cr3 |= (uint64_t)chosen->process->pcid;
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    } else {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }

    context_switch(&old->ctx, &chosen->ctx,
                   &task_rsp[old_idx], task_rsp[new_idx],
                   old->fpu_state, chosen->fpu_state);

    __asm__ volatile ("pushq %0; popfq" : : "r"(saved_flags) : "memory");
}

void scheduler_init(void) {
    task_init();
}

void scheduler_tick(void) {
    reap_pending_dead_task();

    scheduler_ticks++;
    wall_ticks++;
    if (current_task == task_list_head)
        cpu_self()->idle_ticks++;

    /*
     * O(N) timeout scan — Phase 1 TODO:
     *   Replace with a min-heap (binary heap or pairing heap) keyed on wake_tick.
     *   Current complexity: O(TASK_MAX=256) per tick at 100 Hz = 25,600 comparisons/s.
     *   Acceptable for Phase 0; becomes a bottleneck at higher TASK_MAX or tick rate.
     *
     *   SMP concern: this loop runs under CLI on the IRQ-handling CPU only.  On SMP,
     *   tasks homed to other CPUs can have their wake_tick expire here, but task_wakeup
     *   sends an IPI to the home CPU — correct but wastes IRQ budget.  A per-CPU timer
     *   wheel (one wheel per CPU, drained on that CPU's tick) removes the cross-CPU IPI.
     *
     *   Do NOT restructure this loop as a "shortcut early exit" — tasks[i].wake_tick == 0
     *   is the common case for non-sleeping tasks and the branch predictor handles it well.
     */
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= scheduler_ticks) {
            tasks[i].wake_tick = 0;
            task_wakeup(&tasks[i]);
        }
        /* Ph75: refill budget for exhausted tasks whose period has elapsed */
        if (tasks[i].state == TASK_BUDGET_EXHAUSTED &&
            tasks[i].wake_tick != 0 &&
            tasks[i].wake_tick <= scheduler_ticks) {
            if (tasks[i].sched_ctx)
                tasks[i].sched_ctx->remaining_budget = tasks[i].sched_ctx->budget_ticks;
            tasks[i].wake_tick = 0;
            task_wakeup(&tasks[i]);
        }
        /* Timed block expired (channel or notification).
         * spinlock_lock uses CAS without CLI — calling kchannel_cancel_waiter /
         * knotification_cancel_waiter here would deadlock if any task holds
         * live_lock at this IRQ boundary.  We only set the signal; the woken
         * task removes itself from the waiter list in task context. */
        if ((tasks[i].state == TASK_BLOCKED_IPC ||
             tasks[i].state == TASK_BLOCKED_IRQ) &&
            tasks[i].wake_tick != 0 &&
            tasks[i].wake_tick <= scheduler_ticks) {
            tasks[i].timed_out = 1;
            tasks[i].wake_tick = 0;
            task_wakeup(&tasks[i]);
        }
    }

    if (!current_task) return;

    /* Ph75: budget enforcement — decrement remaining_budget for current task */
    if (current_task->sched_ctx && current_task->state == TASK_RUNNING) {
        struct KSchedContext *sc = current_task->sched_ctx;
        if (sc->remaining_budget > 0)
            sc->remaining_budget--;
        if (sc->remaining_budget == 0) {
            current_task->state        = TASK_BUDGET_EXHAUSTED;
            current_task->wake_tick    = scheduler_ticks + sc->period_ticks;
            current_task->need_resched = 1;
            return;
        }
    }

    /* O(1) preemption check via ready_mask bitmap */
    if (current_task->state == TASK_RUNNING) {
        if (rq_top_priority() > (int)(uint8_t)current_task->priority)
            current_task->need_resched = 1;
    }

    if (current_task->ticks_left > 0)
        current_task->ticks_left--;
    if (current_task->ticks_left == 0)
        current_task->need_resched = 1;
}

void scheduler_add_task(void (*entry)(void)) {
    task_create(entry);
}

void scheduler_sleep_current(uint64_t ticks) {
    struct task *t = task_current();
    if (!t || ticks == 0) return;

    t->wake_tick = scheduler_ticks + ticks;
    if (t->wake_tick < scheduler_ticks)
        t->wake_tick = UINT64_MAX;
    t->state        = TASK_SLEEPING;
    t->need_resched = 1;
    task_yield();
}

uint32_t sched_live_task_count(void) {
    return atomic_load_explicit(&sched_live_count, memory_order_relaxed);
}

uint64_t sched_current_ticks(void) {
    return scheduler_ticks;
}

uint64_t sched_wall_ticks(void) {
    return wall_ticks;
}

uint64_t sched_context_switches(void) {
    uint64_t total = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        if (cpu_local[i].rq) total += cpu_local[i].context_switches;
    return total;
}

uint64_t sched_idle_ticks(void) {
    uint64_t total = 0;
    for (int i = 0; i < MAX_CPUS; i++)
        if (cpu_local[i].rq) total += cpu_local[i].idle_ticks;
    return total;
}
