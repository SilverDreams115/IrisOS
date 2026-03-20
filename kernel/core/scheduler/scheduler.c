#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/pmm.h>
#include <stdint.h>

extern void context_switch(struct cpu_context *old,
                           struct cpu_context *new,
                           uint64_t *old_rsp,
                           uint64_t new_rsp);

static struct task tasks[TASK_MAX];
static struct task *current_task = 0;
static struct task *task_list_head = 0;
static uint32_t next_id = 0;

/* RSP guardado por tarea.
 * No tocamos task.h por ahora para mantener el parche acotado.
 */
static uint64_t task_rsp[TASK_MAX];

/* tarea idle — corre cuando no hay nada más */
static void idle_task(void) {
    for (;;) __asm__ volatile ("hlt");
}

static void setup_initial_context(struct task *t, void (*entry)(void)) {
    int idx = (int)(t - tasks);

    uint64_t stack_top = (uint64_t)(uintptr_t)(t->stack + TASK_STACK_SIZE);
    stack_top &= ~0xFULL;

    /* Layout inicial para entrar con ret:
     *
     * [rsp + 0] = entry
     * [rsp + 8] = return address dummy
     *
     * Así, después de ret, la función entra con alineación ABI razonable.
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

    /* crear tarea idle como tarea 0 */
    struct task *idle = &tasks[0];
    idle->id    = next_id++;
    idle->state = TASK_RUNNING;
    idle->next  = idle;

    setup_initial_context(idle, idle_task);

    task_list_head = idle;
    current_task   = idle;
}

struct task *task_create(void (*entry)(void)) {
    /* buscar slot libre */
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

    /* insertar en la lista circular después de idle */
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
    struct task *next = old->next;

    /* buscar la próxima tarea READY o RUNNING */
    struct task *candidate = next;
    for (int i = 0; i < TASK_MAX; i++) {
        if (candidate->state == TASK_READY ||
            candidate->state == TASK_RUNNING) {
            break;
        }
        candidate = candidate->next;
    }

    if (candidate == old) return; /* nada que cambiar */

    if (old->state == TASK_RUNNING)
        old->state = TASK_READY;

    candidate->state = TASK_RUNNING;
    current_task     = candidate;

    int old_idx = (int)(old - tasks);
    int new_idx = (int)(candidate - tasks);

    context_switch(&old->ctx, &candidate->ctx,
                   &task_rsp[old_idx], task_rsp[new_idx]);
}

/* ── scheduler ────────────────────────────────────────────────────── */

void scheduler_init(void) {
    task_init();
}

void scheduler_tick(void) {
    /* llamado desde el handler de IRQ0 (timer) */
    task_yield();
}

void scheduler_add_task(void (*entry)(void)) {
    task_create(entry);
}
