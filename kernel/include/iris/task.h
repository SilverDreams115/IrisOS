#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>

struct KProcess;

#define TASK_MAX         64
#define TASK_STACK_SIZE  8192    /* kernel stack per task */
#define TASK_DEFAULT_SLICE   10      /* ticks per quantum at 100 Hz = 100ms */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,       /* generic blocked — legacy, avoid for new code */
    TASK_BLOCKED_IPC,   /* blocked waiting for an IPC message or KChannel recv */
    TASK_BLOCKED_IRQ,   /* blocked waiting for a KNotification signal */
    TASK_SLEEPING,      /* blocked until a timer tick count is reached */
    TASK_DEAD,
} task_state_t;

/* Helper: true when task is runnable (may be scheduled) */
static inline int task_is_runnable(task_state_t s) {
    return s == TASK_READY || s == TASK_RUNNING;
}

typedef enum {
    TASK_RING0 = 0,   /* kernel task */
    TASK_RING3 = 3,   /* user task */
} task_ring_t;

/* saved kernel-mode registers (callee-saved + rip + rflags) */
struct cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rip;
    uint64_t rflags;   /* IF state per-task — prevents IRQ preemption contamination */
} __attribute__((packed));

struct task {
    uint32_t          id;
    task_state_t      state;
    task_ring_t       ring;

    struct cpu_context ctx;

    /* kernel stack — allocated from the kstack virtual region in paging.h.
     * kstack points to the lowest byte of the usable stack (the guard page
     * sits one page below this address and is intentionally not mapped).
     * kstack_phys is the physical base used to free the PMM pages on teardown.
     * Both fields are 0 for an uninitialized/dead task slot. */
    uint8_t          *kstack;
    uint64_t          kstack_phys;

    /* user entry point and virtual stack info (ring 3 only) */
    uint64_t          user_entry;
    uint64_t          user_rsp;        /* current user stack pointer (for iretq) */
    uint64_t          user_stack_base; /* virtual base of user stack region */
    uint64_t          user_stack_top;  /* virtual top of user stack region */
    uint32_t          user_stack_pages; /* number of pages allocated */
    uint64_t          ustack_phys;     /* physical base of user stack */
    struct KProcess  *process;         /* owning process; NULL for kernel tasks */

    /* cooperative scheduler quantum */
    uint32_t          time_slice;   /* ticks per quantum (default TASK_DEFAULT_SLICE) */
    uint32_t          ticks_left;   /* ticks remaining before need_resched */
    uint32_t          need_resched; /* set by scheduler_tick when ticks_left hits 0 */
    uint64_t          wake_tick;    /* valid when state == TASK_SLEEPING */

    /* FPU/SSE state — 512-byte FXSAVE image, must be 16-byte aligned.
     * Saved and restored on every context switch so FPU state never leaks
     * across task boundaries. Placed last to keep alignment padding minimal. */
    uint8_t           fpu_state[512] __attribute__((aligned(16)));

    struct task      *next;
};

void         task_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_create_user(uint64_t entry);
struct task *task_spawn_user(uint64_t entry, uint64_t arg0);
struct task *task_thread_create(struct KProcess *proc, uint64_t entry_vaddr,
                                uint64_t user_rsp, uint64_t arg);
void         task_set_bootstrap_arg0(struct task *t, uint64_t arg0);
void         task_abort_spawned_user(struct task *t);
void         task_exit_current(void);
void         task_yield(void);
struct task *task_current(void);

/*
 * task_kill_process — forcibly terminate all threads of a process.
 *
 * Iterates the task pool and calls task_kill_external on every task whose
 * process pointer matches proc.  Safe to call when none of proc's threads
 * is the current_task (i.e. only from an external caller).
 * Idempotent: already-dead tasks are skipped.
 */
void task_kill_process(struct KProcess *proc);

/*
 * task_kill_external — forcibly terminate a task that is NOT the current task.
 *
 * Safe to call only when the target is blocked or ready (not running).
 * Because the caller is executing on a different CR3, address-space reap
 * is performed immediately rather than via the pending_reap deferred path.
 *
 * Idempotent: if t is already TASK_DEAD this is a no-op.
 * Must NOT be called with t == task_current(); use task_exit_current() instead.
 */
void task_kill_external(struct task *t);

#endif
