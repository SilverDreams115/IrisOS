#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>
#include <iris/ipc_msg.h>

struct KProcess;
struct KEndpoint;
struct KObject;
struct KSchedContext;
struct KReply;
struct KTcb;

#define TASK_MAX              256
#define TASK_STACK_SIZE       8192  /* kernel stack per task */
#define TASK_DEFAULT_SLICE    2     /* ticks per quantum at 100 Hz = 20ms */
#define TASK_PRIORITY_DEFAULT 128u  /* default scheduling priority */
#define TASK_PRIORITY_MAX     255u  /* highest scheduling priority */
#define TASK_PRIORITY_MIN     0u    /* lowest (idle task) */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED_IPC,       /* blocked waiting for an endpoint IPC rendezvous */
    TASK_BLOCKED_IRQ,       /* blocked waiting for a KNotification signal */
    TASK_SLEEPING,          /* blocked until a timer tick count is reached */
    TASK_BLOCKED_FAULT,     /* suspended pending exception handler decision */
    TASK_BLOCKED_SEND,      /* blocked waiting for a receiver on a KEndpoint */
    TASK_BLOCKED_RECV,      /* blocked waiting for a sender on a KEndpoint */
    TASK_BUDGET_EXHAUSTED,  /* Ph75: SC budget spent; sleeping until refill tick */
    TASK_BLOCKED_REPLY,     /* Ph85: EP_CALL caller blocked waiting for KReply invocation */
    TASK_SUSPENDED,         /* Ph96: explicitly suspended via SYS_TCB_SUSPEND */
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
    uint8_t           priority;  /* Ph73: 0=lowest, 255=highest; idle=0, user=128 */

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
    uint64_t          utext_phys;      /* physical base of userboot text copy (ring-3 only) */
    uint32_t          utext_pages;     /* page count at utext_phys; 0 if not applicable */
    struct KProcess  *process;         /* owning process; NULL for kernel tasks */

    /* cooperative scheduler quantum */
    uint32_t          time_slice;   /* ticks per quantum (default TASK_DEFAULT_SLICE) */
    uint32_t          ticks_left;   /* ticks remaining before need_resched */
    uint32_t          need_resched; /* set by scheduler_tick when ticks_left hits 0 */
    uint32_t          timed_out;   /* set by scheduler_tick when a timed block expires */
    uint64_t          wake_tick;   /* deadline tick: valid for TASK_SLEEPING and timed BLOCKED_IPC/IRQ */

    /* Synchronous endpoint IPC staging (Ph66+). */
    struct IrisMsg      ipc_msg;         /* 64-byte staging/delivery buffer */
    uint32_t            ipc_msg_ready;   /* set by sender on successful rendezvous */
    uint32_t            ipc_ep_closed;   /* set by kendpoint_close while task was blocked */
    struct task        *ep_next;         /* intrusive link for endpoint queue */
    struct KEndpoint   *blocking_ep;     /* endpoint where task is blocked, or NULL */
    /* Ph68: capability staged for transfer during a blocking send */
    struct KObject     *ep_cap_obj;      /* kobject being transferred; NULL = none */
    uint32_t            ep_cap_rights;   /* rights to grant on ep_cap_obj */
    uint64_t            ep_cap_badge;    /* Fase 9: badge carried by the staged cap */
    /* Ph69: IPC buffer staging */
    uint32_t            ipc_kbuf_len;    /* valid bytes in ipc_kbuf */
    uint64_t            ep_recv_buf_uptr;/* receiver's output buffer user addr (set at EP_RECV) */
    uint8_t             ipc_kbuf[IRIS_IPC_BUF_SIZE]; /* kernel-side bulk payload staging */
    /* Ph74: optional scheduling context — retained KSchedContext ref (NULL = best-effort) */
    struct KSchedContext *sched_ctx;
    /* Ph85: reply capability fields */
    uint32_t       ep_call_mode;    /* 1 if task entered EP via SYS_EP_CALL (wants reply) */
    struct KReply *pending_kreply;  /* non-NULL while state == TASK_BLOCKED_REPLY (task holds a ref) */
    /* Ph96: TCB capability — retained ref; NULL for kernel tasks and tasks without KTcb */
    struct KTcb   *ktcb;

    /* SMP: CPU this task is homed to (its run queue owner).
     * Set at creation time; stays constant for the task's lifetime. */
    uint8_t           home_cpu;

    /* FPU/SSE state — 512-byte FXSAVE image, must be 16-byte aligned.
     * Saved and restored on every context switch so FPU state never leaks
     * across task boundaries. Placed last to keep alignment padding minimal. */
    uint8_t           fpu_state[512] __attribute__((aligned(16)));

    struct task      *next;
};

void         task_init(void);
struct task *task_find_by_id(uint32_t id);
struct task *task_create(void (*entry)(void));
struct task *task_spawn_user(uint64_t arg0);
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

/*
 * task_wakeup — transition a blocked/sleeping task to READY and enqueue it
 * in the O(1) priority run queue.  No-op for TASK_DEAD or TASK_RUNNING.
 * Must be called with IRQs disabled or from a single-CPU context.
 */
void task_wakeup(struct task *t);

/*
 * task_suspend — move a task to TASK_SUSPENDED and remove it from the run
 * queue.  No-op for TASK_DEAD.  Caller must yield afterwards if t is the
 * current task, so that a different task is scheduled.
 */
void task_suspend(struct task *t);

#endif
