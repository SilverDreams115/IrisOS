/* tests/kernel/include/iris/task.h — minimal stub for host unit tests.
 * Provides only the fields accessed by kendpoint.c and kchannel.c lifetime paths. */
#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>

#define TASK_MAX              256u
#define TASK_STACK_SIZE       8192u
#define TASK_PRIORITY_DEFAULT 128u
#define TASK_PRIORITY_MAX     255u
#define TASK_PRIORITY_MIN     0u

typedef enum {
    TASK_READY, TASK_RUNNING, TASK_BLOCKED_IPC, TASK_BLOCKED_IRQ,
    TASK_SLEEPING, TASK_BLOCKED_FAULT, TASK_BLOCKED_SEND, TASK_BLOCKED_RECV,
    TASK_BUDGET_EXHAUSTED, TASK_BLOCKED_REPLY, TASK_DEAD,
} task_state_t;

struct KProcess;
struct KEndpoint;
struct KObject;
struct KSchedContext;
struct KReply;

struct task {
    uint32_t          id;
    task_state_t      state;
    uint8_t           priority;
    uint64_t          wake_tick;
    uint8_t           timed_out;
    uint32_t          need_resched;
    struct KProcess  *process;
    struct task      *ep_next;
    struct KEndpoint *blocking_ep;
    uint8_t           ipc_ep_closed;
    struct KObject   *ep_cap_obj;
    uint32_t          ep_cap_rights;
    uint32_t          ipc_kbuf_len;
    uint32_t          ep_call_mode;
    struct KReply    *pending_kreply;
    uint8_t           home_cpu;
};

void         task_wakeup(struct task *t);
struct task *task_current(void);
void         task_yield(void);

#endif
