#include "scheduler_priv.h"
#include <iris/lapic.h>
#include <iris/pmm.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kschedctx.h>
#include <iris/nc/kreply.h>
#include <iris/nc/ktcb.h>
#include <iris/nc/rights.h>
#include <iris/nc/handle_table.h>
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

struct task         tasks[TASK_MAX];
struct task        *current_task    = 0;
struct task        *task_list_head  = 0;
struct task        *task_list_tail  = 0;
uint32_t            next_id         = 0;
uint64_t            task_rsp[TASK_MAX];
uint64_t            kernel_cr3      = 0;

/*
 * Dead-task reap queue — replaces the old single-pointer pending_reap_task.
 *
 * A dying task cannot free its own stack (it's still executing on it).
 * Instead it sets TASK_DEAD and loops on task_yield(), which enqueues the
 * task here after context-switching away.  reap_pending_dead_task() dequeues
 * and frees one entry per call; it is invoked at the top of every task_yield()
 * and every scheduler_tick().
 *
 * Single-CPU correctness: only one task runs at a time, so only one task can
 * be dying between two reap calls.  REAP_QUEUE_SIZE > 1 provides headroom and
 * correctness under SMP where multiple CPUs can each have a dying task.
 *
 * SMP TODO (Phase 1): replace with per-CPU dead lists drained on each CPU's
 * scheduler tick; cross-CPU reap then requires an IPI or a work queue.
 */
#define REAP_QUEUE_SIZE 8  /* power of two; 8 > realistic concurrent deaths */
static struct task    *reap_queue[REAP_QUEUE_SIZE];
static unsigned int    reap_queue_head = 0;   /* producer index (write) */
static unsigned int    reap_queue_tail = 0;   /* consumer index (read)  */
static irq_spinlock_t  reap_queue_lock;

/* ── O(1) per-CPU run queue ──────────────────────────────────────────────── */

static struct CpuRunQueue cpu_rqs[MAX_CPUS];
_Atomic uint32_t          sched_live_count;

void rq_enqueue(struct task *t) {
    struct CpuRunQueue *rq = cpu_local[t->home_cpu].rq;
    if (!rq) return;
    int idx  = (int)(t - tasks);
    int prio = (int)(uint8_t)t->priority;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    if (rq->queued[idx]) { irq_spinlock_unlock(&rq->lock, flags); return; }
    rq->queued[idx] = 1;
    rq->next[idx]   = -1;
    if (rq->head[prio] == -1) {
        rq->head[prio] = idx;
        rq->tail[prio] = idx;
        rq->mask[prio >> 6] |= (1ULL << (prio & 63));
    } else {
        rq->next[rq->tail[prio]] = idx;
        rq->tail[prio]           = idx;
    }
    irq_spinlock_unlock(&rq->lock, flags);
}

void rq_remove(struct task *t) {
    struct CpuRunQueue *rq = cpu_local[t->home_cpu].rq;
    if (!rq) return;
    int idx = (int)(t - tasks);
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    if (!rq->queued[idx]) { irq_spinlock_unlock(&rq->lock, flags); return; }
    int prio = (int)(uint8_t)t->priority;
    int prev = -1, cur = rq->head[prio];
    while (cur != -1 && cur != idx) { prev = cur; cur = rq->next[cur]; }
    if (cur == -1) { rq->queued[idx] = 0; irq_spinlock_unlock(&rq->lock, flags); return; }
    int nxt = rq->next[idx];
    if (prev == -1)              rq->head[prio] = nxt;
    else                         rq->next[prev]  = nxt;
    if (rq->tail[prio] == idx)   rq->tail[prio]  = prev;
    if (rq->head[prio] == -1)
        rq->mask[prio >> 6] &= ~(1ULL << (prio & 63));
    rq->queued[idx] = 0;
    rq->next[idx]   = -1;
    irq_spinlock_unlock(&rq->lock, flags);
}

struct task *rq_dequeue_best(void) {
    struct CpuRunQueue *rq = cpu_self()->rq;
    if (!rq) return 0;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    for (int w = 3; w >= 0; w--) {
        if (!rq->mask[w]) continue;
        int bit  = 63 - __builtin_clzll(rq->mask[w]);
        int prio = w * 64 + bit;
        int idx  = rq->head[prio];
        int nxt  = rq->next[idx];
        rq->head[prio] = nxt;
        if (nxt == -1) {
            rq->tail[prio] = -1;
            rq->mask[w] &= ~(1ULL << bit);
        }
        rq->queued[idx] = 0;
        rq->next[idx]   = -1;
        irq_spinlock_unlock(&rq->lock, flags);
        return &tasks[idx];
    }
    irq_spinlock_unlock(&rq->lock, flags);
    return 0;
}

int rq_top_priority(void) {
    struct CpuRunQueue *rq = cpu_self()->rq;
    if (!rq) return -1;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    int result = -1;
    for (int w = 3; w >= 0; w--) {
        if (rq->mask[w]) {
            result = w * 64 + (63 - __builtin_clzll(rq->mask[w]));
            break;
        }
    }
    irq_spinlock_unlock(&rq->lock, flags);
    return result;
}

void task_wakeup(struct task *t) {
    if (!t || t->state == TASK_DEAD) return;
    t->state = TASK_READY;
    if (t != task_list_head) {
        rq_enqueue(t);
        if (t->home_cpu != cpu_self()->cpu_id)
            lapic_send_ipi(cpu_local[t->home_cpu].lapic_id, RESCHEDULE_IPI_VECTOR);
    }
}

void task_suspend(struct task *t) {
    if (!t || t->state == TASK_DEAD) return;
    t->state = TASK_SUSPENDED;
    rq_remove(t);
}

/* Initial FPU state captured at boot; copied into every new task. */
uint8_t initial_fpu_state[512] __attribute__((aligned(16)));

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void idle_task(void) {
    for (;;) {
        __asm__ volatile ("sti");
        task_yield();
    }
}

struct task *task_find_by_id(uint32_t id) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_DEAD && tasks[i].id == id)
            return &tasks[i];
    }
    return 0;
}

void task_init_fpu_state(struct task *t) {
    uint8_t *dst       = t->fpu_state;
    const uint8_t *src = initial_fpu_state;
    for (uint32_t i = 0; i < 512; i++) dst[i] = src[i];
}

void task_reset_slot(struct task *t) {
    if (!t) return;
    int idx = (int)(t - tasks);
    rq_remove(t);  /* ensure task is dequeued before slot is zeroed */
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
    kendpoint_cancel_waiter(t);
    /* Ph85: cancel pending KReply (task was in TASK_BLOCKED_REPLY). */
    if (t->pending_kreply) {
        struct KReply *r = t->pending_kreply;
        t->pending_kreply = 0;
        kreply_cancel_caller(r); /* clears r->caller; sets caller READY (no-op here) */
        kobject_release(&r->base);
    }
}

/* Release the sched_ctx retained ref and clear the pointer. */
static void task_release_sched_ctx(struct task *t) {
    if (t->sched_ctx) {
        kobject_release(&t->sched_ctx->base);
        t->sched_ctx = 0;
    }
}

/* Nullify the KTcb task pointer and release the task's lifetime ref. */
static void task_release_ktcb(struct task *t) {
    if (!t->ktcb) return;
    ktcb_nullify(t->ktcb);
    kobject_release(&t->ktcb->base);
    t->ktcb = 0;
}

/* Called from reap_pending_dead_task, which runs on the next scheduler tick
 * after task_exit_current sets TASK_DEAD.  The reaped task is always off-CPU
 * at that point, so task_reset_slot needs no additional lock. */
static void reap_dead_task_off_cpu(struct task *t) {
    if (!t || t->state != TASK_DEAD) return;

    struct KProcess *proc = t->process;
    int is_last = proc && (proc->thread_count == 0);

    if (is_last)
        kprocess_reap_address_space(proc);

    atomic_fetch_sub_explicit(&sched_live_count, 1u, memory_order_relaxed);
    unlink_task(t);
    task_release_sched_ctx(t);
    task_reset_slot(t);

    if (is_last)
        kprocess_free(proc);
}

void reap_enqueue_dead(struct task *t) {
    uint64_t flags = irq_spinlock_lock(&reap_queue_lock);
    unsigned int next = (reap_queue_head + 1u) & (REAP_QUEUE_SIZE - 1u);
    if (next != reap_queue_tail) {
        reap_queue[reap_queue_head] = t;
        reap_queue_head = next;
    }
    /* Queue full: slot leaks until a subsequent reap drains it.
     * Cannot occur on single-CPU (one death per yield interval). */
    irq_spinlock_unlock(&reap_queue_lock, flags);
}

void reap_pending_dead_task(void) {
    uint64_t flags = irq_spinlock_lock(&reap_queue_lock);
    if (reap_queue_tail == reap_queue_head) {
        irq_spinlock_unlock(&reap_queue_lock, flags);
        return;
    }
    struct task *t = reap_queue[reap_queue_tail];
    reap_queue[reap_queue_tail] = 0;
    reap_queue_tail = (reap_queue_tail + 1u) & (REAP_QUEUE_SIZE - 1u);
    irq_spinlock_unlock(&reap_queue_lock, flags);

    if (!t) return;
    if (t == current_task) {
        /* Task hasn't context-switched off-CPU yet; re-enqueue for next call. */
        reap_enqueue_dead(t);
        return;
    }
    reap_dead_task_off_cpu(t);
}

static void free_user_stack_pages(struct task *t) {
    if (!t || t->ustack_phys == 0 || t->user_stack_pages == 0) return;
    pmm_free_contig(t->ustack_phys, t->user_stack_pages);
}

static void free_user_text_pages(struct task *t) {
    if (!t || t->utext_phys == 0 || t->utext_pages == 0) return;
    pmm_free_contig(t->utext_phys, t->utext_pages);
}

static void free_phys_pages_range(uint64_t base_phys, uint32_t page_count) {
    if (base_phys == 0 || page_count == 0) return;
    pmm_free_contig(base_phys, page_count);
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

    irq_spinlock_init(&reap_queue_lock);
    kernel_cr3 = pml4_get_current();

    /* Initialize CPU 0's run queue and wire it before any rq_* call. */
    struct CpuRunQueue *rq0 = &cpu_rqs[0];
    irq_spinlock_init(&rq0->lock);
    for (int i = 0; i < 256; i++) { rq0->head[i] = -1; rq0->tail[i] = -1; }
    for (int i = 0; i < TASK_MAX; i++) { rq0->next[i] = -1; rq0->queued[i] = 0; }
    rq0->mask[0] = rq0->mask[1] = rq0->mask[2] = rq0->mask[3] = 0;
    cpu_local[0].rq = rq0;

    atomic_store_explicit(&sched_live_count, 1u, memory_order_relaxed); /* idle */

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
    /* Boot-time BSP init: GS_BASE = 0 here (pre-SWAPGS), so cpu_self() is not
     * yet safe.  Set both the global and the BSP cpu_local slot directly. */
    current_task = idle;
    cpu_local[0].current_task = idle;
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
    t->priority   = TASK_PRIORITY_DEFAULT;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->home_cpu   = 0;

    setup_initial_context(t, entry);

    rq_enqueue(t);
    atomic_fetch_add_explicit(&sched_live_count, 1u, memory_order_relaxed);

    task_list_tail->next = t;
    t->next              = task_list_head;
    task_list_tail       = t;

    return t;
}

void task_set_bootstrap_arg0(struct task *t, uint64_t arg0) {
    if (!t || t->ring != TASK_RING3 || !t->process || !t->process->cr3) return;
    t->ctx.rbx = arg0;
    /* Write arg0 at the physical page corresponding to t->user_rsp.
     * RSP entropy may have shifted user_rsp below the original stack top, so
     * we compute the physical address from user_rsp rather than from
     * user_stack_pages*4096-8 (which would target the wrong location). */
    if (t->ustack_phys != 0 && t->user_rsp >= t->user_stack_base) {
        uint64_t offset = t->user_rsp - t->user_stack_base;
        uint64_t *kptr  = (uint64_t *)(uintptr_t)PHYS_TO_VIRT(t->ustack_phys + offset);
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
    t->priority   = TASK_PRIORITY_DEFAULT;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->home_cpu   = 0;

    proc = kprocess_alloc();
    if (!proc) goto fail;

    proc->cr3 = paging_create_user_space();
    if (proc->cr3 == 0) goto fail;
    proc->user_cr3 = paging_make_user_cr3(proc->cr3, proc->pcid);

    /* Fase 6.2: create KVSpace before bootstrap maps so bootstrap_kframe_map
     * can register mapping back-refs via kframe_map_page. */
    {
        struct KVSpace *vs = kvspace_alloc(proc->cr3);
        if (!vs) goto fail;
        kobject_retain(&vs->base);
        proc->vspace = vs;
        kobject_release(&vs->base);
    }

    /* Copy userboot binary to page-aligned PMM pages. The binary symbol is in
     * kernel .rodata at an unaligned offset; paging_map_checked_in requires
     * page-aligned physical addresses, so a fresh aligned copy is mandatory. */
    ub_pages = (uint32_t)((ub_size + 0xFFFU) >> 12);

    /* Guard: verify the total bootstrap page count fits in KProcess.bootstrap_frames[]. */
    if (ub_pages + ustack_pages > KPROCESS_BOOTSTRAP_FRAME_MAX) goto fail;

    ub_copy_phys = pmm_alloc_pages(ub_pages);
    if (ub_copy_phys == 0) goto fail;
    {
        uint8_t       *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(ub_copy_phys);
        const uint8_t *src = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT((uint64_t)(uintptr_t)ub_data);
        for (uint32_t b = 0; b < ub_size; b++) dst[b] = src[b];
        for (uint32_t b = ub_size; b < (uint32_t)(ub_pages << 12); b++) dst[b] = 0;
    }
    /* Fase 6.2: Bootstrap Frame-backed mapping: userboot text (r--x).
     * Each page gets a KFrame (alloc_parent=NULL) mapped via kframe_map_page.
     * The alloc retain is stored in proc->bootstrap_frames[] and released by
     * kprocess_release_bootstrap_frames inside kprocess_reap_address_space,
     * after kvspace_invalidate has decremented mapped_count to 0.
     * Physical memory lifetime tracked by t->utext_phys; freed by
     * free_user_text_pages on the teardown paths that precede reap. */
    for (uint32_t pg = 0; pg < ub_pages; pg++) {
        uint64_t va   = USER_TEXT_BASE + (uint64_t)pg * 0x1000ULL;
        uint64_t phys = ub_copy_phys   + (uint64_t)pg * 0x1000ULL;
        struct KFrame *f = bootstrap_kframe_map(proc->vspace, phys, va, 2ULL /* MAP_EXEC */);
        if (!f) goto fail_copy;
        if (kprocess_register_bootstrap_frame(proc, f) != IRIS_OK) {
            kframe_unmap_page(f, proc->vspace, va);
            kobject_release(&f->base);
            goto fail_copy;
        }
    }
    t->user_entry = USER_TEXT_BASE;

    ustack_phys = pmm_alloc_pages(ustack_pages);
    if (ustack_phys == 0) goto fail;

    /* Fase 6.2: Bootstrap Frame-backed mapping: initial user stack (rw-nx).
     * Same KFrame-backed pattern as the text mapping above.
     * Physical memory tracked by t->ustack_phys. */
    for (uint32_t pg = 0; pg < ustack_pages; pg++) {
        uint64_t va   = USER_STACK_BASE + 4096ULL * USER_STACK_GUARD_PAGES +
                        (uint64_t)pg * 4096ULL;
        uint64_t phys = ustack_phys + (uint64_t)pg * 4096ULL;
        struct KFrame *f = bootstrap_kframe_map(proc->vspace, phys, va, 1ULL /* MAP_WRITABLE */);
        if (!f) goto fail;
        if (kprocess_register_bootstrap_frame(proc, f) != IRIS_OK) {
            kframe_unmap_page(f, proc->vspace, va);
            kobject_release(&f->base);
            goto fail;
        }
    }

    t->user_stack_base  = USER_STACK_BASE + 4096ULL * USER_STACK_GUARD_PAGES;
    t->user_stack_top   = USER_STACK_TOP;
    t->user_stack_pages = ustack_pages;
    t->ustack_phys      = ustack_phys;

    /* Stack RSP ASLR: randomize starting offset by 0..15 * 16 bytes (0..240).
     * Uses RDTSC as an entropy source; the low bits vary per boot and per task.
     * The offset is 16-byte aligned so the ABI requirement (RSP mod 16 == 8
     * before the call instruction) is preserved.  The entropy range (≤240 bytes)
     * keeps user_rsp well within the allocated stack region. */
    {
        uint32_t tsc_lo, tsc_hi;
        __asm__ volatile ("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        uint64_t entropy = (uint64_t)tsc_lo & 0xFULL; /* 0..15 */
        t->user_rsp = USER_STACK_TOP - 8 - (entropy << 4);
    }
    t->process          = proc;
    proc->thread_count  = 1;

    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;  /* SS: user data (sysretq) */
    kstack_top -= 8; *(uint64_t *)kstack_top = t->user_rsp;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;  /* CS: user code (sysretq) */
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

    /* Ph96: create KTcb and hand a handle to the process; keep task's own ref. */
    {
        struct KTcb *tcb = ktcb_alloc(t);
        if (tcb) {
            handle_id_t tcb_h = handle_table_insert(
                &proc->handle_table, &tcb->base,
                RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
            kobject_release(&tcb->base); /* drop alloc ref; HT holds its own */
            if (tcb_h != HANDLE_INVALID) {
                kobject_retain(&tcb->base); /* task's own lifetime ref */
                t->ktcb = tcb;
            }
        }
    }

    rq_enqueue(t);
    atomic_fetch_add_explicit(&sched_live_count, 1u, memory_order_relaxed);

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

    atomic_fetch_sub_explicit(&sched_live_count, 1u, memory_order_relaxed);
    unlink_task(t);
    task_release_sched_ctx(t);
    task_release_ktcb(t);
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
    t->priority   = TASK_PRIORITY_DEFAULT;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->home_cpu   = 0;
    t->user_entry = entry_vaddr;
    t->user_rsp   = user_rsp;
    t->process    = proc;

    uint64_t kstack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    kstack_top &= ~0xFULL;

    kstack_top -= 8; *(uint64_t *)kstack_top = 0x1B;  /* SS: user data (sysretq) */
    kstack_top -= 8; *(uint64_t *)kstack_top = user_rsp;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x0202;
    kstack_top -= 8; *(uint64_t *)kstack_top = 0x23;  /* CS: user code (sysretq) */
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

    /* Ph96: create KTcb and hand a handle to the process; keep task's own ref. */
    {
        struct KTcb *tcb = ktcb_alloc(t);
        if (tcb) {
            handle_id_t tcb_h = handle_table_insert(
                &proc->handle_table, &tcb->base,
                RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
            kobject_release(&tcb->base);
            if (tcb_h != HANDLE_INVALID) {
                kobject_retain(&tcb->base);
                t->ktcb = tcb;
            }
        }
    }

    rq_enqueue(t);
    atomic_fetch_add_explicit(&sched_live_count, 1u, memory_order_relaxed);

    task_list_tail->next = t;
    t->next              = task_list_head;
    task_list_tail       = t;

    return t;
}

/* ── Task termination ────────────────────────────────────────────────────── */

struct task *task_current(void) {
    return current_task;
}

/* Must not be called on current_task: this function frees resources that the
 * calling stack may still reference.  task_kill_process skips current_task
 * automatically when iterating tasks[]. */
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

    atomic_fetch_sub_explicit(&sched_live_count, 1u, memory_order_relaxed);
    unlink_task(t);
    task_release_sched_ctx(t);
    task_release_ktcb(t);
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
    task_release_ktcb(t);

    /* thread_count is mutated only from single-CPU scheduler context on this
     * arch.  The TASK_DEAD spin below ensures the slot is not reused before
     * the scheduler calls reap_dead_task_off_cpu on the next tick. */
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
