#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>

#define TASK_MAX        16
#define TASK_STACK_SIZE 8192

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_DEAD,
} task_state_t;

struct cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rip;
} __attribute__((packed));

struct task {
    uint32_t         id;
    task_state_t     state;
    struct cpu_context ctx;
    uint8_t          stack[TASK_STACK_SIZE];
    struct task     *next;
};

void         task_init(void);
struct task *task_create(void (*entry)(void));
void         task_yield(void);
struct task *task_current(void);

#endif
