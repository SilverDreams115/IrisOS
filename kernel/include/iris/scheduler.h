#ifndef IRIS_SCHEDULER_H
#define IRIS_SCHEDULER_H

#include <stdint.h>
#include <iris/elf_loader.h>

void     scheduler_init(void);
void     scheduler_tick(void);
void     scheduler_add_task(void (*entry)(void));
void     scheduler_sleep_current(uint64_t ticks);

/*
 * task_spawn_elf — spawn a ring-3 process from a loaded ELF image.
 *
 * Takes ownership of *img on success: the KProcess and its handle table will
 * free the segment pages and page tables when the process exits.  The caller
 * must NOT call elf_loader_free_image on a successful return.
 *
 * On failure the caller must call elf_loader_free_image(img) to release the
 * physical pages and page tables that elf_loader_load allocated.  task_spawn_elf
 * itself never calls elf_loader_free_image — the responsibility split is:
 *   - success path: KProcess teardown owns all ELF resources
 *   - failure path: caller owns them and must call elf_loader_free_image
 *
 * @img    Populated iris_elf_image_t from a successful elf_loader_load call.
 * @arg0   Value delivered to the child process via the bootstrap contract
 *         (RBX on first ring-3 entry and at USER_STACK_TOP-8).
 *
 * Returns a pointer to the new task on success, NULL on failure (out of task
 * slots, out of physical memory for the stack, or mapping failure).
 */
struct task *task_spawn_elf(iris_elf_image_t *img, uint64_t arg0);

/*
 * Diagnostics accessors — cheap, read-only, safe to call from syscall context.
 *
 * sched_live_task_count: number of scheduler task slots in any non-DEAD state.
 *   Includes the idle task.  Useful as a coarse live-process indicator.
 *   Cost: O(TASK_MAX) scan.
 *
 * sched_current_ticks: current scheduler tick counter value.
 *   Incremented at TASK_DEFAULT_SLICE Hz; wraps at UINT64_MAX (>5000 years at 100Hz).
 *   Use the low 32 bits for short-lived deltas; use both halves for absolute timestamps.
 */
uint32_t sched_live_task_count(void);
uint64_t sched_current_ticks(void);

#endif
