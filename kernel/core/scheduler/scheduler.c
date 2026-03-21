#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/pmm.h>
#include <iris/tss.h>
#include <stdint.h>

extern void user_entry_trampoline(void);
extern void context_switch(struct cpu_context *old,
                           struct cpu_context *new,
                           uint64_t *old_rsp,
                           uint64_t new_rsp);

static struct task tasks[TASK_MAX];
static struct task *current_task = 0;
static struct task *task_list_head = 0;
static uint32_t next_id = 0;

/* RSP guardado por tarea */
static uint64_t task_rsp[TASK_MAX];

/* ticks del timer; por ahora sólo observación, no preempción real */
static volatile uint64_t scheduler_ticks = 0;

/* tarea idle — corre cuando no hay nada más */
static void idle_task(void) {
    for (;;) __asm__ volatile ("hlt");
}

static void setup_initial_context(struct task *t, void (*entry)(void)) {
    int idx = (int)(t - tasks);

    uint64_t stack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    stack_top &= ~0xFULL;

    /* Layout inicial para entrar con ret:
     * [rsp + 0] = entry
     * [rsp + 8] = return address dummy
     */
    stack_top -= 16;
    ((uint64_t *)stack_top)[0] = (uint64_t)(uintptr_t)entry;
    ((uint64_t *)stack_top)[1] = 0;

    task_rsp[idx] = stack_top;

    t->ctx.r15 = 0;
    t->ctx.r14 = 0;
    t->ctx.r13 = 0;
    t->ctx.r12 = 0;
    t->ctx.rbx = 0;
    t->ctx.rbp = 0;
    t->ctx.rip = (uint64_t)(uintptr_t)entry;
}

void task_init(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        tasks[i].state = TASK_DEAD;
        tasks[i].next  = 0;
        task_rsp[i]    = 0;
    }

    struct task *idle = &tasks[0];
    idle->id    = next_id++;
    idle->state = TASK_RUNNING;
    idle->next  = idle;

    setup_initial_context(idle, idle_task);

    task_list_head = idle;
    current_task   = idle;
}

struct task *task_create(void (*entry)(void)) {
    struct task *t = 0;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_DEAD) {
            t = &tasks[i];
            break;
        }
    }
    if (!t) return 0;

    t->id    = next_id++;
    t->state = TASK_READY;

    setup_initial_context(t, entry);

    struct task *tail = task_list_head;
    while (tail->next != task_list_head)
        tail = tail->next;
    tail->next = t;
    t->next    = task_list_head;

    return t;
}


struct task *task_create_user(uint64_t entry) {
    struct task *t = 0;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_DEAD) {
            t = &tasks[i];
            break;
        }
    }
    if (!t) return 0;

    t->id         = next_id++;
    t->state      = TASK_READY;
    t->ring       = TASK_RING3;
    t->user_entry = entry;
    t->cr3        = 0; /* share kernel page table for now */

    /* user stack top — 16-byte aligned */
    uint64_t ustack_top = (uint64_t)(uintptr_t)(t->ustack + TASK_USTACK_SIZE);
    ustack_top &= ~0xFULL;
    t->user_rsp = ustack_top;

    /* kernel stack: set up so context_switch lands in user_entry_trampoline */
    int idx = (int)(t - tasks);
    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    /* iretq frame layout on stack (top = low addr, popped first):
     * [kstack_top - 8]  = RIP       <- iretq pops this first
     * [kstack_top - 16] = CS
     * [kstack_top - 24] = RFLAGS
     * [kstack_top - 32] = RSP (user)
     * [kstack_top - 40] = SS
     * Push in reverse order (SS first, RIP last) */
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;            /* SS  */
    kstack_top -= 8; *(uint64_t *)kstack_top = t->user_rsp;     /* RSP */
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;          /* RFLAGS: IF=1, IOPL=0 */
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;            /* CS  */
    kstack_top -= 8; *(uint64_t *)kstack_top = entry;           /* RIP */

    /* trampoline: context_switch will ret here, then iretq to user */
    kstack_top -= 8; *(uint64_t *)kstack_top = (uint64_t)(uintptr_t)user_entry_trampoline;

    task_rsp[idx] = kstack_top;

    t->ctx.r15 = 0; t->ctx.r14 = 0; t->ctx.r13 = 0; t->ctx.r12 = 0;
    t->ctx.rbx = 0; t->ctx.rbp = 0;
    t->ctx.rip = (uint64_t)(uintptr_t)user_entry_trampoline;

    /* link into task list */
    struct task *tail = task_list_head;
    while (tail->next != task_list_head)
        tail = tail->next;
    tail->next = t;
    t->next    = task_list_head;

    return t;
}

struct task *task_current(void) {
    return current_task;
}

void task_yield(void) {
    struct task *old = current_task;
    struct task *idle = task_list_head;
    struct task *candidate = old->next;
    struct task *chosen = 0;

    /* Preferir cualquier tarea runnable que NO sea idle */
    for (int i = 0; i < TASK_MAX; i++) {
        if (candidate != idle &&
            (candidate->state == TASK_READY ||
             candidate->state == TASK_RUNNING)) {
            chosen = candidate;
            break;
        }
        candidate = candidate->next;
    }

    /* Si no hay ninguna tarea normal runnable, usar idle */
    if (!chosen) {
        if (idle != old &&
            (idle->state == TASK_READY || idle->state == TASK_RUNNING)) {
            chosen = idle;
        } else {
            return;
        }
    }

    if (chosen == old) return;

    if (old->state == TASK_RUNNING)
        old->state = TASK_READY;

    chosen->state = TASK_RUNNING;
    current_task  = chosen;

    int old_idx = (int)(old - tasks);
    int new_idx = (int)(chosen - tasks);

    /* update TSS rsp0 to new task kernel stack top */
    uint64_t new_kstack_top = (uint64_t)(uintptr_t)(chosen->kstack + TASK_STACK_SIZE);
    tss_set_rsp0(new_kstack_top);

    context_switch(&old->ctx, &chosen->ctx,
                   &task_rsp[old_idx], task_rsp[new_idx]);
}

/* ── scheduler ────────────────────────────────────────────────────── */

void scheduler_init(void) {
    task_init();
}

void scheduler_tick(void) {
    scheduler_ticks++;
}

void scheduler_add_task(void (*entry)(void)) {
    task_create(entry);
}
