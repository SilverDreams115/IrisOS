#include <iris/task.h>
#include <iris/scheduler.h>
#include <iris/pmm.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kprocess.h>
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

/* Current scheduler semantics:
 * - Runnable tasks are time-sliced by IRQ0.
 * - When the current task exhausts its quantum, IRQ exit can switch tasks.
 * - Tasks may also block explicitly (IPC/IRQ/sleep) and yield cooperatively.
 * This is a small timer-driven scheduler, not cooperative-only anymore. */
static volatile uint64_t scheduler_ticks = 0;

/* tarea idle — corre cuando no hay nada más */
static void idle_task(void) {
    for (;;) __asm__ volatile ("hlt");
}

static void free_user_stack_pages(struct task *t) {
    if (!t || t->ustack_phys == 0 || t->user_stack_pages == 0) return;
    for (uint32_t pg = 0; pg < t->user_stack_pages; pg++) {
        pmm_free_page(t->ustack_phys + (uint64_t)pg * 4096ULL);
    }
}

static void task_reset_slot(struct task *t) {
    if (!t) return;
    int idx = (int)(t - tasks);
    uint8_t *raw = (uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(*t); i++) raw[i] = 0;
    t->state = TASK_DEAD;
    t->ring  = TASK_RING0;
    task_rsp[idx] = 0;
}

static void unlink_task(struct task *t) {
    if (!t || !task_list_head) return;

    /* Single-node circular list: t is the only element. */
    if (task_list_head == t && t->next == t) {
        task_list_head = 0;
        t->next = 0;
        return;
    }

    /* Find the predecessor (node whose ->next == t).
     * This correctly handles head removal (predecessor is the tail)
     * and non-head removal (predecessor is an interior node). */
    struct task *pred = task_list_head;
    do {
        if (pred->next == t) {
            pred->next = t->next;
            if (task_list_head == t)
                task_list_head = t->next;
            t->next = 0;
            return;
        }
        pred = pred->next;
    } while (pred && pred != task_list_head);

    /* t not found in list — no-op (safe, no corruption). */
}

static void reap_dead_task_after_switch_prep(struct task *t) {
    if (!t || t->state != TASK_DEAD) return;

    struct KProcess *proc = t->process;

    /* Runs after CR3 already switched away from the dead task. Only process
     * address-space/object cleanup belongs here; task-local resources are gone. */
    if (proc) {
        kprocess_reap_address_space(proc);
    }

    task_reset_slot(t);

    if (proc) {
        kprocess_free(proc);
    }
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

    t->ctx.r15    = 0;
    t->ctx.r14    = 0;
    t->ctx.r13    = 0;
    t->ctx.r12    = 0;
    t->ctx.rbx    = 0;
    t->ctx.rbp    = 0;
    t->ctx.rip    = (uint64_t)(uintptr_t)entry;
    t->ctx.rflags = 0x202ULL;  /* IF=1, reserved bit 1 — interrupts enabled */
}



void task_init(void)
{
    kernel_cr3 = pml4_get_current();
    for (int i = 0; i < TASK_MAX; i++) {
        task_reset_slot(&tasks[i]);
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

    task_reset_slot(t);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;

    setup_initial_context(t, entry);

    struct task *tail = task_list_head;
    while (tail->next != task_list_head)
        tail = tail->next;
    tail->next = t;
    t->next    = task_list_head;

    return t;
}






/* Bootstrap contract for spawned ring-3 tasks:
 *   - arg0 is delivered in RBX on first user entry
 *   - a legacy mirror is also written at USER_STACK_TOP-8 during transition
 */
void task_set_bootstrap_arg0(struct task *t, uint64_t arg0) {
    if (!t || t->ring != TASK_RING3 || !t->process || !t->process->cr3) return;
    t->ctx.rbx = arg0;
    paging_write_u64_in(t->process->cr3, USER_STACK_TOP - 8, arg0);
}

static void free_phys_pages_range(uint64_t base_phys, uint32_t page_count) {
    if (base_phys == 0) return;
    for (uint32_t pg = 0; pg < page_count; pg++)
        pmm_free_page(base_phys + (uint64_t)pg * 4096ULL);
}

/* Internal: create a ring-3 task, optionally passing arg0 via the bootstrap
 * contract above. */
static struct task *task_create_user_impl(uint64_t entry, uint64_t arg0) {
    struct task *t = 0;
    struct KProcess *proc = 0;
    uint64_t ustack_phys = 0;
    uint32_t ustack_pages = (uint32_t)(USER_STACK_SIZE / 4096ULL);
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_DEAD) {
            t = &tasks[i];
            break;
        }
    }
    if (!t) return 0;

    task_reset_slot(t);
    t->id           = next_id++;
    t->state        = TASK_READY;
    t->ring         = TASK_RING3;
    t->time_slice   = TASK_DEFAULT_SLICE;
    t->ticks_left   = TASK_DEFAULT_SLICE;

    proc = kprocess_alloc();
    if (!proc) goto fail;

    proc->cr3 = paging_create_user_space();
    if (proc->cr3 == 0) goto fail;

    /* Map the entire shared user image slice preserving original offsets so
     * RIP-relative references into .rodata keep working after relocation. */
    for (uint64_t off = 0; off < USER_IMAGE_MAP_SIZE; off += 0x1000ULL) {
        uint64_t vp = USER_TEXT_BASE + off;
        uint64_t pp = USER_IMAGE_SOURCE_BASE + off;
        if (paging_map_checked_in(proc->cr3, vp, pp, PAGE_PRESENT | PAGE_USER) != 0)
            goto fail;
    }
    uint64_t user_entry = USER_TEXT_BASE + (entry - USER_IMAGE_SOURCE_BASE);
    t->user_entry = user_entry;

    /* allocate and map user stack at USER_STACK_TOP */
    ustack_phys = pmm_alloc_pages(ustack_pages);
    if (ustack_phys == 0) goto fail;

    for (uint32_t pg = 0; pg < ustack_pages; pg++) {
        uint64_t virt = USER_STACK_BASE + (uint64_t)pg * 4096ULL;
        uint64_t phys = ustack_phys     + (uint64_t)pg * 4096ULL;
        if (paging_map_checked_in(proc->cr3, virt, phys,
                                  PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0)
            goto fail;
    }

    t->user_stack_base  = USER_STACK_BASE;
    t->user_stack_top   = USER_STACK_TOP;
    t->user_stack_pages = ustack_pages;
    t->ustack_phys      = ustack_phys;          /* physical base — identity-mapped */
    t->user_rsp         = USER_STACK_TOP - 8;   /* points to arg0 */
    t->process          = proc;
    proc->main_thread   = t;

    /* kernel stack: iretq frame for user_entry_trampoline */
    int idx = (int)(t - tasks);
    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;
    kstack_top -= 8; *(uint64_t *)kstack_top = t->user_rsp;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;
    kstack_top -= 8; *(uint64_t *)kstack_top = user_entry;
    kstack_top -= 8; *(uint64_t *)kstack_top = (uint64_t)(uintptr_t)user_entry_trampoline;

    task_rsp[idx] = kstack_top;

    t->ctx.r15    = 0; t->ctx.r14 = 0; t->ctx.r13 = 0; t->ctx.r12 = 0;
    t->ctx.rbx    = 0; t->ctx.rbp = 0;
    t->ctx.rip    = (uint64_t)(uintptr_t)user_entry_trampoline;
    t->ctx.rflags = 0x202ULL;
    task_set_bootstrap_arg0(t, arg0);

    /* Commit point: only publish the task after slot, process, private
     * address space, stack, and bootstrap state are all coherent. */
    struct task *tail = task_list_head;
    while (tail->next != task_list_head)
        tail = tail->next;
    tail->next = t;
    t->next    = task_list_head;

    return t;

fail:
    if (proc)
        proc->main_thread = 0;
    if (t)
        t->process = 0;
    free_phys_pages_range(ustack_phys, ustack_pages);
    if (proc) {
        kprocess_reap_address_space(proc);
        kprocess_free(proc);
    }
    if (t)
        task_reset_slot(t);
    return 0;
}

struct task *task_create_user(uint64_t entry) {
    return task_create_user_impl(entry, 0);
}

/* Spawn a ring-3 process with arg0 installed via the bootstrap contract. */
struct task *task_spawn_user(uint64_t entry, uint64_t arg0) {
    return task_create_user_impl(entry, arg0);
}

void task_abort_spawned_user(struct task *t) {
    if (!t) return;

    struct KProcess *proc = t->process;
    if (proc) {
        kprocess_teardown(proc, t);
    }

    free_user_stack_pages(t);
    if (proc) {
        kprocess_reap_address_space(proc);
    }

    unlink_task(t);
    task_reset_slot(t);

    if (proc) {
        kprocess_free(proc);
    }
}





struct task *task_current(void) {
    return current_task;
}

void task_exit_current(void) {
    struct task *t = task_current();
    if (!t) return;

    struct KProcess *proc = t->process;
    /* Pre-switch exit phase: logical process teardown while task context is
     * still available, then release task-local stack pages. */
    if (proc) {
        kprocess_teardown(proc, t);
    }

    free_user_stack_pages(t);
    t->state = TASK_DEAD;

    for (;;) task_yield();
}

void task_yield(void) {
    struct task *old = current_task;
    struct task *idle = task_list_head;
    struct task *candidate = old->next;
    struct task *chosen = 0;

    /* Preferir cualquier tarea runnable que NO sea idle */
    for (int i = 0; i < TASK_MAX; i++) {
        if (candidate != idle && task_is_runnable(candidate->state)) {
            chosen = candidate;
            break;
        }
        candidate = candidate->next;
    }

    /* Si no hay ninguna tarea normal runnable, usar idle */
    if (!chosen) {
        if (idle != old && task_is_runnable(idle->state)) {
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
    if (chosen->process && chosen->process->cr3 != 0)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(chosen->process->cr3) : "memory");
    else
        __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");

    reap_dead_task_after_switch_prep(old);

    context_switch(&old->ctx, &chosen->ctx,
                   &task_rsp[old_idx], task_rsp[new_idx]);
}

/* ── scheduler ────────────────────────────────────────────────────── */

void scheduler_init(void) {
    task_init();
}

void scheduler_tick(void) {
    scheduler_ticks++;
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].wake_tick <= scheduler_ticks) {
            tasks[i].state = TASK_READY;
            tasks[i].wake_tick = 0;
        }
    }
    if (!current_task) return;
    if (current_task->ticks_left > 0)
        current_task->ticks_left--;
    if (current_task->ticks_left == 0)
        current_task->need_resched = 1;
}

void scheduler_add_task(void (*entry)(void)) {
    task_create(entry);
}

void scheduler_sleep_current(uint64_t ticks) {
    struct task *t = task_current();
    if (!t || ticks == 0) return;

    t->wake_tick = scheduler_ticks + ticks;
    if (t->wake_tick < scheduler_ticks)
        t->wake_tick = UINT64_MAX;
    t->state = TASK_SLEEPING;
    t->need_resched = 1;
    task_yield();
}
