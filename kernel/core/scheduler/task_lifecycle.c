#include "scheduler_priv.h"
#include <iris/pmm.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/futex.h>
#include <iris/initrd.h>
#include <stdint.h>

/*
 * task_lifecycle.c — task creation, teardown, kill, and related helpers.
 *
 * Owns all shared scheduler state (tasks[], current_task, etc.) that the
 * other scheduler translation units reference via externs in scheduler_priv.h.
 */

/* ── Shared state definitions ────────────────────────────────────────────── */

struct task      tasks[TASK_MAX];
struct task     *current_task    = 0;
struct task     *task_list_head  = 0;
struct task     *task_list_tail  = 0;
struct task     *pending_reap_task = 0;
uint32_t         next_id         = 0;
uint64_t         task_rsp[TASK_MAX];
uint64_t         kernel_cr3      = 0;

/* Initial FPU state captured at boot; copied into every new task. */
uint8_t initial_fpu_state[512] __attribute__((aligned(16)));

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void idle_task(void) {
    for (;;) {
        __asm__ volatile ("sti");
        task_yield();
    }
}

void task_init_fpu_state(struct task *t) {
    uint8_t *dst       = t->fpu_state;
    const uint8_t *src = initial_fpu_state;
    for (uint32_t i = 0; i < 512; i++) dst[i] = src[i];
}

void task_reset_slot(struct task *t) {
    if (!t) return;
    int idx = (int)(t - tasks);
    kstack_free(t, idx);
    uint8_t *raw = (uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(*t); i++) raw[i] = 0;
    t->state    = TASK_DEAD;
    t->ring     = TASK_RING0;
    task_rsp[idx] = 0;
}

void unlink_task(struct task *t) {
    if (!t || !task_list_head) return;

    if (task_list_head == t && t->next == t) {
        task_list_head = 0;
        task_list_tail = 0;
        t->next = 0;
        return;
    }

    struct task *pred = task_list_head;
    do {
        if (pred->next == t) {
            pred->next = t->next;
            if (task_list_head == t)
                task_list_head = t->next;
            if (task_list_tail == t)
                task_list_tail = pred;
            t->next = 0;
            return;
        }
        pred = pred->next;
    } while (pred && pred != task_list_head);
}

static void task_cancel_blocked_waits(struct task *t) {
    if (!t) return;
    kchannel_cancel_waiter(t);
    knotification_cancel_waiter(t);
    futex_cancel_waiter(t);
}

static void reap_dead_task_off_cpu(struct task *t) {
    if (!t || t->state != TASK_DEAD) return;

    struct KProcess *proc = t->process;
    int is_last = proc && (proc->thread_count == 0);

    if (is_last)
        kprocess_reap_address_space(proc);

    unlink_task(t);
    task_reset_slot(t);

    if (is_last)
        kprocess_free(proc);
}

void reap_pending_dead_task(void) {
    if (!pending_reap_task || pending_reap_task == current_task) return;

    struct task *t = pending_reap_task;
    pending_reap_task = 0;
    reap_dead_task_off_cpu(t);
}

static void free_user_stack_pages(struct task *t) {
    if (!t || t->ustack_phys == 0 || t->user_stack_pages == 0) return;
    for (uint32_t pg = 0; pg < t->user_stack_pages; pg++)
        pmm_free_page(t->ustack_phys + (uint64_t)pg * 4096ULL);
}

static void free_user_text_pages(struct task *t) {
    if (!t || t->utext_phys == 0 || t->utext_pages == 0) return;
    for (uint32_t pg = 0; pg < t->utext_pages; pg++)
        pmm_free_page(t->utext_phys + (uint64_t)pg * 4096ULL);
}

static void free_phys_pages_range(uint64_t base_phys, uint32_t page_count) {
    if (base_phys == 0) return;
    for (uint32_t pg = 0; pg < page_count; pg++)
        pmm_free_page(base_phys + (uint64_t)pg * 4096ULL);
}

void setup_initial_context(struct task *t, void (*entry)(void)) {
    int idx = (int)(t - tasks);

    uint64_t stack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    stack_top &= ~0xFULL;

    /* Stack layout for ret-based entry: [rsp+0]=entry, [rsp+8]=dummy return */
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
    t->ctx.rflags = 0x202ULL;
}

/* ── Task creation ───────────────────────────────────────────────────────── */

void task_init(void) {
    __asm__ volatile ("fxsaveq (%0)" : : "r"(initial_fpu_state) : "memory");

    kernel_cr3 = pml4_get_current();

    for (int i = 0; i < TASK_MAX; i++)
        task_reset_slot(&tasks[i]);

    struct task *idle = &tasks[0];
    idle->id    = next_id++;
    idle->state = TASK_RUNNING;
    idle->next  = idle;

    if (kstack_alloc(idle, 0) != 0)
        kstack_panic("cannot allocate idle task kstack");

    setup_initial_context(idle, idle_task);
    task_init_fpu_state(idle);

    task_list_head = idle;
    task_list_tail = idle;
    current_task   = idle;
}

struct task *task_create(void (*entry)(void)) {
    struct task *t = 0;
    int idx = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_DEAD) { t = &tasks[i]; idx = i; break; }
    }
    if (!t) return 0;

    task_reset_slot(t);
    if (kstack_alloc(t, idx) != 0) return 0;

    task_init_fpu_state(t);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;

    setup_initial_context(t, entry);

    task_list_tail->next = t;
    t->next              = task_list_head;
    task_list_tail       = t;

    return t;
}

void task_set_bootstrap_arg0(struct task *t, uint64_t arg0) {
    if (!t || t->ring != TASK_RING3 || !t->process || !t->process->cr3) return;
    t->ctx.rbx = arg0;
    if (t->ustack_phys != 0) {
        uint64_t *kptr = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(
                             t->ustack_phys + (uint64_t)t->user_stack_pages * 4096ULL - 8);
        *kptr = arg0;
    }
}

static struct task *task_create_user_impl(uint64_t arg0) {
    struct task *t = 0;
    int idx = -1;
    struct KProcess *proc = 0;
    uint64_t ustack_phys = 0;
    uint32_t ustack_pages = (uint32_t)((USER_STACK_SIZE / 4096ULL) - USER_STACK_GUARD_PAGES);
    uint64_t ub_copy_phys = 0;
    uint32_t ub_pages = 0;
    const void *ub_data = 0;
    uint32_t    ub_size = 0;

    if (!initrd_bootstrap_image(&ub_data, &ub_size) || ub_size == 0) return 0;

    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_DEAD) { t = &tasks[i]; idx = i; break; }
    }
    if (!t) return 0;

    task_reset_slot(t);
    if (kstack_alloc(t, idx) != 0) return 0;

    task_init_fpu_state(t);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->ring       = TASK_RING3;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;

    proc = kprocess_alloc();
    if (!proc) goto fail;

    proc->cr3 = paging_create_user_space();
    if (proc->cr3 == 0) goto fail;

    /* Copy userboot binary to page-aligned PMM pages. The binary symbol is in
     * kernel .rodata at an unaligned offset; paging_map_checked_in requires
     * page-aligned physical addresses, so a fresh aligned copy is mandatory. */
    ub_pages = (uint32_t)((ub_size + 0xFFFU) >> 12);
    ub_copy_phys = pmm_alloc_pages(ub_pages);
    if (ub_copy_phys == 0) goto fail;
    {
        uint8_t       *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(ub_copy_phys);
        const uint8_t *src = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT((uint64_t)(uintptr_t)ub_data);
        for (uint32_t b = 0; b < ub_size; b++) dst[b] = src[b];
        for (uint32_t b = ub_size; b < (uint32_t)(ub_pages << 12); b++) dst[b] = 0;
    }
    for (uint32_t pg = 0; pg < ub_pages; pg++) {
        if (paging_map_checked_in(proc->cr3,
                                  USER_TEXT_BASE + (uint64_t)pg * 0x1000ULL,
                                  ub_copy_phys   + (uint64_t)pg * 0x1000ULL,
                                  PAGE_PRESENT | PAGE_USER) != 0)
            goto fail_copy;
    }
    t->user_entry = USER_TEXT_BASE;

    ustack_phys = pmm_alloc_pages(ustack_pages);
    if (ustack_phys == 0) goto fail;

    for (uint32_t pg = 0; pg < ustack_pages; pg++) {
        uint64_t virt = USER_STACK_BASE + 4096ULL * USER_STACK_GUARD_PAGES +
                        (uint64_t)pg * 4096ULL;
        uint64_t phys = ustack_phys     + (uint64_t)pg * 4096ULL;
        if (paging_map_checked_in(proc->cr3, virt, phys,
                                  PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX) != 0)
            goto fail;
    }

    t->user_stack_base  = USER_STACK_BASE + 4096ULL * USER_STACK_GUARD_PAGES;
    t->user_stack_top   = USER_STACK_TOP;
    t->user_stack_pages = ustack_pages;
    t->ustack_phys      = ustack_phys;
    t->user_rsp         = USER_STACK_TOP - 8;
    t->process          = proc;
    proc->thread_count  = 1;

    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;
    kstack_top -= 8; *(uint64_t *)kstack_top = t->user_rsp;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;
    kstack_top -= 8; *(uint64_t *)kstack_top = USER_TEXT_BASE;
    kstack_top -= 8; *(uint64_t *)kstack_top = (uint64_t)(uintptr_t)user_entry_trampoline;

    task_rsp[idx] = kstack_top;

    t->ctx.r15    = 0; t->ctx.r14 = 0; t->ctx.r13 = 0; t->ctx.r12 = 0;
    t->ctx.rbx    = 0; t->ctx.rbp = 0;
    t->ctx.rip    = (uint64_t)(uintptr_t)user_entry_trampoline;
    t->ctx.rflags = 0x202ULL;
    task_set_bootstrap_arg0(t, arg0);

    t->utext_phys  = ub_copy_phys;
    t->utext_pages = ub_pages;

    task_list_tail->next = t;
    t->next              = task_list_head;
    task_list_tail       = t;
    return t;

fail_copy:
    free_phys_pages_range(ub_copy_phys, ub_pages);
fail:
    if (t) t->process = 0;
    free_phys_pages_range(ustack_phys, ustack_pages);
    if (proc) {
        kprocess_reap_address_space(proc);
        kprocess_free(proc);
    }
    if (t) task_reset_slot(t);
    return 0;
}

struct task *task_spawn_user(uint64_t arg0) {
    return task_create_user_impl(arg0);
}

void task_abort_spawned_user(struct task *t) {
    if (!t) return;

    struct KProcess *proc = t->process;
    if (proc) {
        if (proc->thread_count > 0) proc->thread_count--;
        kprocess_teardown(proc, t);
    }

    task_cancel_blocked_waits(t);
    free_user_stack_pages(t);
    free_user_text_pages(t);
    if (proc)
        kprocess_reap_address_space(proc);

    unlink_task(t);
    task_reset_slot(t);

    if (proc)
        kprocess_free(proc);
}

struct task *task_thread_create(struct KProcess *proc, uint64_t entry_vaddr,
                                uint64_t user_rsp, uint64_t arg) {
    if (!proc || !proc->cr3 || proc->teardown_complete) return 0;

    struct task *t = 0;
    int idx = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_DEAD) { t = &tasks[i]; idx = i; break; }
    }
    if (!t) return 0;

    task_reset_slot(t);
    if (kstack_alloc(t, idx) != 0) return 0;
    task_init_fpu_state(t);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->ring       = TASK_RING3;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->user_entry = entry_vaddr;
    t->user_rsp   = user_rsp;
    t->process    = proc;

    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;
    kstack_top -= 8; *(uint64_t *)kstack_top = user_rsp;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;
    kstack_top -= 8; *(uint64_t *)kstack_top = entry_vaddr;
    kstack_top -= 8; *(uint64_t *)kstack_top =
        (uint64_t)(uintptr_t)user_entry_trampoline;

    task_rsp[idx] = kstack_top;

    t->ctx.r15    = 0; t->ctx.r14 = 0; t->ctx.r13 = 0; t->ctx.r12 = 0;
    t->ctx.rbp    = 0;
    t->ctx.rbx    = arg;
    t->ctx.rip    = (uint64_t)(uintptr_t)user_entry_trampoline;
    t->ctx.rflags = 0x202ULL;

    proc->thread_count++;

    task_list_tail->next = t;
    t->next              = task_list_head;
    task_list_tail       = t;

    return t;
}

/* ── Task termination ────────────────────────────────────────────────────── */

struct task *task_current(void) {
    return current_task;
}

void task_kill_external(struct task *t) {
    if (!t || t == current_task) return;
    if (t->state == TASK_DEAD) return;

    struct KProcess *proc = t->process;

    task_cancel_blocked_waits(t);
    free_user_stack_pages(t);
    free_user_text_pages(t);

    int do_teardown = 0;
    if (proc && proc->thread_count > 0) {
        proc->thread_count--;
        if (proc->thread_count == 0) do_teardown = 1;
    }

    if (do_teardown) {
        kprocess_teardown(proc, t);
        kprocess_reap_address_space(proc);
    }

    unlink_task(t);
    task_reset_slot(t);

    if (do_teardown)
        kprocess_free(proc);
}

void task_exit_current(void) {
    struct task *t = task_current();
    if (!t) return;

    struct KProcess *proc = t->process;

    task_cancel_blocked_waits(t);
    free_user_stack_pages(t);
    free_user_text_pages(t);

    if (proc && proc->thread_count > 0) {
        proc->thread_count--;
        if (proc->thread_count == 0)
            kprocess_teardown(proc, t);
    }

    t->state = TASK_DEAD;
    for (;;) task_yield();
}

void task_kill_process(struct KProcess *proc) {
    if (!proc) return;
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].process == proc)
            task_kill_external(&tasks[i]);
    }
}
