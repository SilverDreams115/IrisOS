#ifndef IRIS_SCHEDULER_H
#define IRIS_SCHEDULER_H

#include <stdint.h>

#define TASK_MAX 16
#define TASK_STACK_SIZE 4096

enum task_state {
  TASK_DEAD = 0,
  TASK_READY,
  TASK_RUNNING,
};

struct cpu_context {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t rbx;
  uint64_t rbp;
  uint64_t rip;
};

struct task {
  uint32_t id;
  uint32_t state;
  struct cpu_context ctx;
  struct task *next;
  uint8_t stack[TASK_STACK_SIZE];
};

void task_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_current(void);
void task_yield(void);

void scheduler_init(void);
void scheduler_tick(void);
void scheduler_add_task(void (*entry)(void));

#endif
