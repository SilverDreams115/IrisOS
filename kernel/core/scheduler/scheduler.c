#include "scheduler_priv.h"
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kprocess.h>
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

void task_yield(void) {
    /* Save and clear IF for the scheduler critical section; restore on exit.
     * Each task's IF state is preserved across context switches via RFLAGS
     * save/restore in context_switch. */
    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) : : "memory");

    reap_pending_dead_task();

    struct task *old  = current_task;
    struct task *idle = task_list_head;
    struct task *candidate = old->next;
    struct task *chosen = 0;

    for (int i = 0; i < TASK_MAX; i++) {
        if (candidate != idle && task_is_runnable(candidate->state)) {
            chosen = candidate;
            break;
        }
        candidate = candidate->next;
    }

    if (!chosen) {
        /* No non-idle runnable task.  When idle is current, fast-forward
         * scheduler_ticks to the nearest wake_tick deadline so timed-out
         * tasks become READY even when the timer ISR does not fire (e.g.,
         * QEMU TCG: no IRQs delivered during ring-0 spin). */
        if (old == idle) {
            uint64_t min_wake = UINT64_MAX;
            for (int j = 0; j < TASK_MAX; j++) {
                if (tasks[j].wake_tick != 0 && tasks[j].wake_tick < min_wake)
                    min_wake = tasks[j].wake_tick;
            }
            if (min_wake != UINT64_MAX && min_wake > scheduler_ticks)
                scheduler_ticks = min_wake;
            for (int j = 0; j < TASK_MAX; j++) {
                if (tasks[j].state == TASK_SLEEPING &&
                    tasks[j].wake_tick != 0 &&
                    tasks[j].wake_tick <= scheduler_ticks) {
                    tasks[j].state     = TASK_READY;
                    tasks[j].wake_tick = 0;
                }
                if ((tasks[j].state == TASK_BLOCKED_IPC ||
                     tasks[j].state == TASK_BLOCKED_IRQ) &&
                    tasks[j].wake_tick != 0 &&
                    tasks[j].wake_tick <= scheduler_ticks) {
                    tasks[j].timed_out = 1;
                    tasks[j].state     = TASK_READY;
                    tasks[j].wake_tick = 0;
                }
            }
            /* Re-scan for newly runnable tasks after the fast-forward. */
            candidate = old->next;
            for (int i = 0; i < TASK_MAX; i++) {
                if (candidate != idle && task_is_runnable(candidate->state)) {
                    chosen = candidate;
                    break;
                }
                candidate = candidate->next;
            }
        }
        if (!chosen) {
            if (idle != old && task_is_runnable(idle->state)) {
                chosen = idle;
            } else {
                __asm__ volatile ("pushq %0; popfq" : : "r"(saved_flags) : "memory");
                return;
            }
        }
    }

    if (chosen == old) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(saved_flags) : "memory");
        return;
    }

    if (old->state == TASK_RUNNING)
        old->state = TASK_READY;

    chosen->state      = TASK_RUNNING;
    chosen->ticks_left = chosen->time_slice;
    chosen->need_resched = 0;
    current_task       = chosen;

    int old_idx = (int)(old - tasks);
    int new_idx = (int)(chosen - tasks);

    uint64_t new_kstack_top = (uint64_t)(uintptr_t)(chosen->kstack + TASK_STACK_SIZE);
    tss_set_rsp0(new_kstack_top);
    syscall_set_kstack(new_kstack_top);

    if (chosen->process && chosen->process->cr3 != 0)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(chosen->process->cr3) : "memory");
    else
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");

    if (old->state == TASK_DEAD)
        pending_reap_task = old;

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
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= scheduler_ticks) {
            tasks[i].state     = TASK_READY;
            tasks[i].wake_tick = 0;
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
            tasks[i].state     = TASK_READY;
            tasks[i].wake_tick = 0;
        }
    }
    if (!current_task) return;
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
    uint32_t count = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_DEAD)
            count++;
    }
    return count;
}

uint64_t sched_current_ticks(void) {
    return scheduler_ticks;
}

uint64_t sched_wall_ticks(void) {
    return wall_ticks;
}
