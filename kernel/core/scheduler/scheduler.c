#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/pmm.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
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
static uint64_t kernel_cr3 = 0;

/* Cooperative scheduler — tasks yield explicitly via task_yield().
 * IRQ0 increments ticks for future timeslice accounting only.
 * Preemption (need_resched flag + forced yield on IRQ exit) is NOT
 * implemented yet and must be added as a separate stage. */
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



void task_init(void)
{
    kernel_cr3 = pml4_get_current();
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

    t->id         = next_id++;
    t->state      = TASK_READY;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->need_resched = 0;

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

    t->id           = next_id++;
    t->state        = TASK_READY;
    t->ring         = TASK_RING3;
    t->time_slice   = TASK_DEFAULT_SLICE;
    t->ticks_left   = TASK_DEFAULT_SLICE;
    t->need_resched = 0;
    t->user_entry   = entry;

    t->cr3 = paging_create_user_space();
    if (t->cr3 == 0) return 0;

    /* allocate and map user stack at USER_STACK_TOP in process CR3 */
    uint32_t ustack_pages = (uint32_t)(USER_STACK_SIZE / 4096ULL);
    uint64_t ustack_phys  = pmm_alloc_pages(ustack_pages);
    if (ustack_phys == 0) return 0;

    for (uint32_t pg = 0; pg < ustack_pages; pg++) {
        uint64_t virt = USER_STACK_BASE + (uint64_t)pg * 4096ULL;
        uint64_t phys = ustack_phys  + (uint64_t)pg * 4096ULL;
        paging_map_in(t->cr3, virt, phys,
                      PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    t->user_stack_base  = USER_STACK_BASE;
    t->user_stack_top   = USER_STACK_TOP;
    t->user_stack_pages = ustack_pages;
    t->user_rsp         = USER_STACK_TOP & ~0xFULL;

    /* kernel stack: iretq frame for user_entry_trampoline */
    int idx = (int)(t - tasks);
    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;
    kstack_top -= 8; *(uint64_t *)kstack_top = t->user_rsp;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;
    kstack_top -= 8; *(uint64_t *)kstack_top = entry;
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

    chosen->state      = TASK_RUNNING;
    chosen->ticks_left = chosen->time_slice;  /* reload quantum */
    chosen->need_resched = 0;
    current_task       = chosen;

    int old_idx = (int)(old - tasks);
    int new_idx = (int)(chosen - tasks);

    /* update TSS rsp0 to new task kernel stack top */
    uint64_t new_kstack_top = (uint64_t)(uintptr_t)(chosen->kstack + TASK_STACK_SIZE);
    tss_set_rsp0(new_kstack_top);
    syscall_set_kstack(new_kstack_top);  /* per-task syscall kernel stack */

    /* switch CR3: user task gets its own page table, kernel tasks go back to kernel CR3 */
    if (chosen->cr3 != 0)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(chosen->cr3) : "memory");
    else
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");

    context_switch(&old->ctx, &chosen->ctx,
                   &task_rsp[old_idx], task_rsp[new_idx]);
}

/* ── scheduler ────────────────────────────────────────────────────── */

void scheduler_init(void) {
    task_init();
}

void scheduler_tick(void) {
    scheduler_ticks++;
    if (!current_task) return;
    if (current_task->ticks_left > 0)
        current_task->ticks_left--;
    if (current_task->ticks_left == 0)
        current_task->need_resched = 1;
}

void scheduler_add_task(void (*entry)(void)) {
    task_create(entry);
}
