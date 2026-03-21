#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>

#define TASK_MAX         16
#define TASK_STACK_SIZE  8192    /* kernel stack per task */
#define TASK_USTACK_SIZE     8192    /* user stack per task */
#define TASK_DEFAULT_SLICE   10      /* ticks per quantum at 100 Hz = 100ms */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD,
} task_state_t;

typedef enum {
    TASK_RING0 = 0,   /* kernel task */
    TASK_RING3 = 3,   /* user task */
} task_ring_t;

/* saved kernel-mode registers (callee-saved + rip) */
struct cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rip;
} __attribute__((packed));

struct task {
    uint32_t          id;
    task_state_t      state;
    task_ring_t       ring;

    struct cpu_context ctx;

    /* kernel stack — always present, used on interrupt entry from ring 3 */
    uint8_t           kstack[TASK_STACK_SIZE];

    /* user stack — only used for ring 3 tasks */
    uint8_t           ustack[TASK_USTACK_SIZE];

    /* user entry point and stack top (for iretq) */
    uint64_t          user_entry;
    uint64_t          user_rsp;

    /* page table root (CR3) — 0 = use kernel page table */
    uint64_t          cr3;

    /* cooperative scheduler quantum */
    uint32_t          time_slice;   /* ticks per quantum (default TASK_DEFAULT_SLICE) */
    uint32_t          ticks_left;   /* ticks remaining before need_resched */
    uint32_t          need_resched; /* set by scheduler_tick when ticks_left hits 0 */

    struct task      *next;
};

void         task_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_create_user(uint64_t entry);
void         task_yield(void);
struct task *task_current(void);

#endif
