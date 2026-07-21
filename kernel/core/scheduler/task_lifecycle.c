#include "scheduler_priv.h"
#include <iris/lapic.h>
#include <iris/pmm.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
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

/*
 * Fase S2 D2 — two independent pools with separated lifetimes:
 *   ktcb_backing[]  — the KTCB objects (backing storage).  A slot is free when
 *                     state == TASK_DEAD; it is freed by the OBJECT destructor
 *                     (last reference), NOT at thread termination.  A
 *                     TERMINATED-but-cap-held TCB pins its backing slot.
 *   ktcb_registry[] — scheduler identity (pointer+generation+occupied).  A slot
 *                     is held only while the thread is registered (runnable/
 *                     alive) and RELEASED AT TERMINATION, so a surviving cap to
 *                     a terminated TCB never consumes scheduler capacity.
 * Every other translation unit now goes through ktcb_registry[i].tcb (see
 * scheduler.c) — there is no remaining external reference to the raw backing
 * array by position, so ktcb_backing[] is the sole name; it is scaffolding
 * removed in D/E when backing moves to Untyped.
 */
struct task         ktcb_backing[TASK_MAX];  /* backing pool (D/E → Untyped) */
KTcbRegistrySlot    ktcb_registry[TASK_MAX];
struct task        *current_task    = 0;

/* ── Fase S2 D2 — registry + backing instrumentation (QUERY kind 4) ── */
static _Atomic uint32_t reg_active;
static _Atomic uint32_t reg_hwm;
static _Atomic uint32_t reg_exhaustions;
static _Atomic uint32_t reg_generation_mismatch;

void task_registry_stats(uint32_t *active, uint32_t *hwm,
                         uint32_t *exhaustions, uint32_t *gen_mismatch) {
    if (active)       *active       = atomic_load_explicit(&reg_active, memory_order_relaxed);
    if (hwm)          *hwm          = atomic_load_explicit(&reg_hwm, memory_order_relaxed);
    if (exhaustions)  *exhaustions  = atomic_load_explicit(&reg_exhaustions, memory_order_relaxed);
    if (gen_mismatch) *gen_mismatch = atomic_load_explicit(&reg_generation_mismatch, memory_order_relaxed);
}

/*
 * Registry lifetime — allocate a scheduler-identity slot for t.  Independent
 * of the backing slot: a new task's registry index need not equal its backing
 * index, so a reused registry slot can point at a fresh backing while an old
 * terminated TCB still pins a different backing slot (T287).  Records the slot
 * in t->reg_slot.  Returns 0 on success, -1 (exhaustion) otherwise.
 */
static int task_registry_alloc(struct task *t) {
    for (int i = 1; i < TASK_MAX; i++) {   /* slot 0 = idle bootstrap */
        if (!ktcb_registry[i].occupied) {
            ktcb_registry[i].tcb      = t;
            ktcb_registry[i].occupied = 1;
            t->reg_slot = i;
            uint32_t n = atomic_fetch_add_explicit(&reg_active, 1u, memory_order_relaxed) + 1u;
            uint32_t hw = atomic_load_explicit(&reg_hwm, memory_order_relaxed);
            while (n > hw &&
                   !atomic_compare_exchange_weak_explicit(&reg_hwm, &hw, n,
                                                          memory_order_relaxed,
                                                          memory_order_relaxed)) { }
            return 0;
        }
    }
    atomic_fetch_add_explicit(&reg_exhaustions, 1u, memory_order_relaxed);
    return -1;
}

/* Registry lifetime end — release t's scheduler-identity slot (at termination).
 * Bumps generation so a stale registry token is detectable.  The object stays
 * alive if caps reference it; only its scheduler identity is gone. */
static void task_registry_release(struct task *t) {
    int i = t ? t->reg_slot : -1;
    if (i < 0 || i >= TASK_MAX) return;
    if (ktcb_registry[i].tcb == t && ktcb_registry[i].occupied) {
        ktcb_registry[i].occupied = 0;
        ktcb_registry[i].tcb      = 0;
        ktcb_registry[i].generation++;
        atomic_fetch_sub_explicit(&reg_active, 1u, memory_order_relaxed);
    }
    t->reg_slot = -1;
}

/* Bootstrap: bind registry slot 0 to the idle task (never reused).  Idle is
 * the single BOOTSTRAP_EXCEPTION of the ledger: static backing, no cap-visible
 * object, never retyped; marked configured so no execution gate can ever
 * misread it as an inactive retyped TCB. */
static void task_registry_bind_idle(struct task *idle) {
    ktcb_registry[0].tcb       = idle;
    ktcb_registry[0].occupied  = 1;
    ktcb_registry[0].bootstrap = 1;
    idle->reg_slot   = 0;
    idle->configured = 1;
    atomic_fetch_add_explicit(&reg_active, 1u, memory_order_relaxed);
}

/* Storage lifetime — find a free backing slot (state == TASK_DEAD).  Slot 0 is
 * the idle bootstrap exception.  awaiting_reap excludes a slot that a
 * self-exited task has already marked TASK_DEAD but the deferred reaper has
 * not yet torn down (A1.11: reusing it early would wipe a still-referenced
 * t->process out from under the pending reap).  Returns NULL when the
 * backing pool is full. */
static struct task *task_backing_find_free(void) {
    for (int i = 1; i < TASK_MAX; i++) {
        if (ktcb_backing[i].state == TASK_DEAD && !ktcb_backing[i].awaiting_reap)
            return &ktcb_backing[i];
    }
    return 0;
}

/*
 * Allocate a fresh TCB: a free backing slot (storage) plus a free registry
 * slot (scheduler identity).  *idx_out receives the BACKING array index (used
 * to address the per-slot kstack pool) — independent of the registry slot
 * chosen for t (t->reg_slot); see the Etapa C/D split above.  Returns NULL on
 * either exhaustion (backing pool full, or registry full).
 */
static struct task *task_registry_find_free(int *idx_out) {
    struct task *t = task_backing_find_free();
    if (!t) return 0;
    if (task_registry_alloc(t) != 0) return 0;
    *idx_out = (int)(t - ktcb_backing);
    return t;
}

struct task        *task_list_head  = 0;
struct task        *task_list_tail  = 0;
uint32_t            next_id         = 0;
/* Fase S2: task_rsp[TASK_MAX] retired — kernel RSP moved into struct task.saved_krsp */
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

/*
 * Fase 17 — additive scheduler instrumentation (silent, ABI-safe, exposed
 * only through SYS_SCHED_INFO's ext2 tier).  None of this changes scheduling
 * decisions; it only makes run-queue invariants observable to the T119–T124
 * selftests.
 *
 *   rq_live_count  — tasks currently enqueued across all run queues.  Bumped on
 *                    a successful enqueue, dropped on dequeue/remove.  Its
 *                    high-water (rq_live_hwm) proves the queue depth stays
 *                    bounded under churn (T120).
 *   sched_dup_enq  — number of times rq_enqueue's queued[] guard rejected a
 *                    re-enqueue of an already-queued task.  This is the counter
 *                    behind invariant S4 (no task twice in the run queue): the
 *                    guard is the mechanism, this is the evidence it engaged.
 *                    It is a benign, expected event (e.g. a rendezvous racing a
 *                    timeout wakeup), so tests assert it stays bounded, never
 *                    that it is zero.
 */
static _Atomic uint32_t   rq_live_count;
static _Atomic uint32_t   rq_live_hwm;
static _Atomic uint32_t   sched_dup_enq;

static inline void rq_live_inc(void) {
    uint32_t n = atomic_fetch_add_explicit(&rq_live_count, 1u,
                                           memory_order_relaxed) + 1u;
    uint32_t hwm = atomic_load_explicit(&rq_live_hwm, memory_order_relaxed);
    if (n > hwm)
        atomic_store_explicit(&rq_live_hwm, n, memory_order_relaxed);
}

static inline void rq_live_dec(void) {
    atomic_fetch_sub_explicit(&rq_live_count, 1u, memory_order_relaxed);
}

uint32_t sched_run_queue_hwm(void) {
    return atomic_load_explicit(&rq_live_hwm, memory_order_relaxed);
}

uint32_t sched_run_queue_live(void) {
    return atomic_load_explicit(&rq_live_count, memory_order_relaxed);
}

uint32_t sched_duplicate_enqueue_count(void) {
    return atomic_load_explicit(&sched_dup_enq, memory_order_relaxed);
}

void rq_enqueue(struct task *t) {
    struct CpuRunQueue *rq = cpu_local[t->home_cpu].rq;
    if (!rq) return;
    int prio = (int)(uint8_t)t->priority;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    if (t->rq_queued) {
        /* S4 guard engaged: task already queued — reject the duplicate. */
        atomic_fetch_add_explicit(&sched_dup_enq, 1u, memory_order_relaxed);
        irq_spinlock_unlock(&rq->lock, flags);
        return;
    }
    t->rq_queued = 1;
    t->rq_next   = 0;
    if (rq->head[prio] == 0) {
        rq->head[prio] = t;
        rq->tail[prio] = t;
        rq->mask[prio >> 6] |= (1ULL << (prio & 63));
    } else {
        rq->tail[prio]->rq_next = t;
        rq->tail[prio]          = t;
    }
    rq_live_inc();
    irq_spinlock_unlock(&rq->lock, flags);
}

void rq_remove(struct task *t) {
    struct CpuRunQueue *rq = cpu_local[t->home_cpu].rq;
    if (!rq) return;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    if (!t->rq_queued) { irq_spinlock_unlock(&rq->lock, flags); return; }
    int prio = (int)(uint8_t)t->priority;
    struct task *prev = 0, *cur = rq->head[prio];
    while (cur && cur != t) { prev = cur; cur = cur->rq_next; }
    if (!cur) { t->rq_queued = 0; rq_live_dec(); irq_spinlock_unlock(&rq->lock, flags); return; }
    struct task *nxt = t->rq_next;
    if (!prev)                   rq->head[prio] = nxt;
    else                         prev->rq_next  = nxt;
    if (rq->tail[prio] == t)     rq->tail[prio]  = prev;
    if (rq->head[prio] == 0)
        rq->mask[prio >> 6] &= ~(1ULL << (prio & 63));
    t->rq_queued = 0;
    t->rq_next   = 0;
    rq_live_dec();
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
        struct task *t   = rq->head[prio];
        struct task *nxt = t->rq_next;
        rq->head[prio] = nxt;
        if (nxt == 0) {
            rq->tail[prio] = 0;
            rq->mask[w] &= ~(1ULL << bit);
        }
        t->rq_queued = 0;
        t->rq_next   = 0;
        rq_live_dec();
        irq_spinlock_unlock(&rq->lock, flags);
        return t;
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
    /* Fase S2 D2: t->terminal guards against a wakeup arriving mid-teardown
     * (e.g. kreply_cancel_caller waking its own caller when that caller is
     * the very task task_execution_teardown_off_cpu is unwinding) — state
     * alone is not enough once TASK_TERMINATED exists between READY/BLOCKED*
     * and TASK_DEAD; a terminal task must never re-enter the run queue. */
    if (!t || t->state == TASK_DEAD || t->terminal) return;
    t->state = TASK_READY;
    if (t != task_list_head) {
        rq_enqueue(t);
        if (t->home_cpu != cpu_self()->cpu_id)
            lapic_send_ipi(cpu_local[t->home_cpu].lapic_id, RESCHEDULE_IPI_VECTOR);
    }
}

void task_suspend(struct task *t) {
    if (!t || t->state == TASK_DEAD || t->terminal) return;
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
        if (!ktcb_registry[i].occupied) continue;
        struct task *t = ktcb_registry[i].tcb;
        if (t->state != TASK_DEAD && t->id == id)
            return t;
    }
    return 0;
}

void task_init_fpu_state(struct task *t) {
    uint8_t *dst       = t->fpu_state;
    const uint8_t *src = initial_fpu_state;
    for (uint32_t i = 0; i < 512; i++) dst[i] = src[i];
}

/*
 * Fase S2 D2 — initialize a backing slot to the free (TASK_DEAD) state.  Used
 * ONLY at boot (task_init).  Does NOT touch the registry (separate pool) and
 * does NOT free the kstack (fresh slots have none).  The DEATH path no longer
 * calls this: a terminated object's backing is freed by the destructor.
 */
void task_reset_slot(struct task *t) {
    if (!t) return;
    uint8_t *raw = (uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(*t); i++) raw[i] = 0;
    t->state    = TASK_DEAD;
    t->ring     = TASK_RING0;
    t->reg_slot = -1;
    t->saved_krsp = 0;
}

/*
 * Fase S2 D2 — the KTCB object destructor (called from ktcb.c when the last
 * reference drops).  This is the STORAGE lifetime end: the backing slot is
 * zeroed and returned to TASK_DEAD (reusable), the object generation bumped so
 * a stale cap/token can never alias the next object placed here.  By this
 * point execution teardown already freed the kstack/addrspace and released the
 * registry slot, so nothing here can be running or queued.  (D/E: this will
 * instead free the Untyped child.)
 */
void task_backing_free_on_destroy(struct task *t) {
    if (!t) return;
    uint32_t gen = t->object_generation + 1u;
    uint8_t *raw = (uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(*t); i++) raw[i] = 0;
    t->object_generation = gen;
    t->state    = TASK_DEAD;
    t->ring     = TASK_RING0;
    t->reg_slot = -1;
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
    /* Fase 13/Track G: kchannel_cancel_waiter retired — no task blocks on a
     * KChannel (the object is gone). */
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
    /* Fase S1: release a staged-but-unbound explicit reply object (task died
     * while blocked in EP_RECV with a reply CPtr staged).  The object returns
     * to its free state and stays owned by whoever holds its capability. */
    if (t->ep_reply_obj) {
        struct KReply *r = t->ep_reply_obj;
        t->ep_reply_obj = 0;
        t->ep_reply_val = 0;
        kreply_unstage(r);
        kobject_release(&r->base);
    }
}

/* Release the sched_ctx retained ref and clear the pointer. */
static void task_release_sched_ctx(struct task *t) {
    if (t->sched_ctx) {
        /* Fase S2: unbind first so the SC keeps no stale bound_task pointer
         * (S2.11); then drop this task's ref. */
        kschedctx_unbind(t->sched_ctx, t);
        kobject_release(&t->sched_ctx->base);
        t->sched_ctx = 0;
    }
}

static void free_user_stack_pages(struct task *t) {
    if (!t || t->ustack_phys == 0 || t->user_stack_pages == 0) return;
    pmm_free_contig(t->ustack_phys, t->user_stack_pages);
}

static void free_user_text_pages(struct task *t) {
    if (!t || t->utext_phys == 0 || t->utext_pages == 0) return;
    pmm_free_contig(t->utext_phys, t->utext_pages);
}

/*
 * Fase S2 D2 — execution teardown (shared by self-exit and external kill).
 * Ends the EXECUTION, REGISTRY and (frees) execution resources, but NOT the
 * object: it drops the scheduler's execution reference last, so the object is
 * destroyed here only if no capability references it.  A surviving cap keeps
 * the (TERMINATED) object — and its backing slot — alive until the cap closes.
 *
 * Preconditions: t is OFF-CPU (never the running task on its own kstack).
 */
static void task_execution_teardown_off_cpu(struct task *t) {
    if (!t || t->terminal) return;

    struct KProcess *proc = t->process;

    /*
     * Make t unreachable and unwakeable FIRST, atomically with respect to a
     * timer IRQ, before any of the slower calls below (kprocess_teardown,
     * kprocess_reap_address_space especially) that can run long enough to be
     * interrupted.  scheduler_tick and sched_handle_idle iterate ktcb_registry
     * for occupied slots and call task_wakeup() on whatever they find
     * sleeping, blocked, or budget-exhausted with an elapsed wake_tick; if
     * that scan still finds t occupied, with its pre-kill blocked state and
     * wake_tick intact, mid-teardown, task_wakeup() re-enqueues t into the
     * run queue out from under the impending destroy.  Releasing the
     * registry slot (so the scan skips t) and forcing TASK_TERMINATED (so
     * task_wakeup's own state check no longer matches) closes that window.
     * The old design routed every death path through task_reset_slot, which
     * called rq_remove as a side effect; that no longer happens here, so
     * rq_remove is explicit too (a no-op if t was never queued).
     */
    uint64_t irq_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(irq_flags) : : "memory");
    rq_remove(t);
    task_registry_release(t);
    t->awaiting_reap = 0;
    t->state    = TASK_TERMINATED;
    t->terminal = 1;
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory");

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

    /* Free the kernel stack (execution resource).  Slot index = backing index
     * (t is within ktcb_backing[]); kstack_free tolerates an unallocated one. */
    int idx = (int)(t - ktcb_backing);
    kstack_free(t, idx);
    t->kstack = 0; t->kstack_phys = 0;

    if (do_teardown)
        kprocess_free(proc);

    /* Drop the scheduler's execution reference LAST.  If no cap references the
     * object, this triggers task_backing_free_on_destroy (slot → TASK_DEAD).
     * A surviving cap keeps the TERMINATED object (and its backing) alive. */
    kobject_release(&t->base);
}

/* Fase 16: reap-queue depth high-water, for lifecycle-churn diagnostics
 * (exposed additively via SYS_SCHED_INFO).  Monotonic; proves the deferred
 * reaper drains under pressure (T114/T118) — if it ever approached
 * REAP_QUEUE_SIZE the "cannot occur on single-CPU" assumption would be
 * broken and dead tasks would leak their slots. */
static uint32_t reap_queue_hwm = 0u;

uint32_t sched_reap_queue_hwm(void) {
    return __atomic_load_n(&reap_queue_hwm, __ATOMIC_RELAXED);
}

void reap_enqueue_dead(struct task *t) {
    uint64_t flags = irq_spinlock_lock(&reap_queue_lock);
    unsigned int next = (reap_queue_head + 1u) & (REAP_QUEUE_SIZE - 1u);
    if (next != reap_queue_tail) {
        reap_queue[reap_queue_head] = t;
        reap_queue_head = next;
        unsigned int depth = (reap_queue_head - reap_queue_tail) &
                             (REAP_QUEUE_SIZE - 1u);
        if (depth > reap_queue_hwm) reap_queue_hwm = depth;
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
    task_execution_teardown_off_cpu(t);
}

static void free_phys_pages_range(uint64_t base_phys, uint32_t page_count) {
    if (base_phys == 0 || page_count == 0) return;
    pmm_free_contig(base_phys, page_count);
}

void setup_initial_context(struct task *t, void (*entry)(void)) {
    uint64_t stack_top = (uint64_t)(uintptr_t)(t->kstack + TASK_STACK_SIZE);
    stack_top &= ~0xFULL;

    /* Stack layout for ret-based entry: [rsp+0]=entry, [rsp+8]=dummy return */
    stack_top -= 16;
    ((uint64_t *)stack_top)[0] = (uint64_t)(uintptr_t)entry;
    ((uint64_t *)stack_top)[1] = 0;

    t->saved_krsp = stack_top;

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
    for (int i = 0; i < 256; i++) { rq0->head[i] = 0; rq0->tail[i] = 0; }
    rq0->mask[0] = rq0->mask[1] = rq0->mask[2] = rq0->mask[3] = 0;
    cpu_local[0].rq = rq0;

    atomic_store_explicit(&sched_live_count, 1u, memory_order_relaxed); /* idle */

    /* Etapa C: wire every registry slot to its backing before use.  In this
     * checkpoint tcb == &ktcb_backing[i]; Etapa D re-points these at Untyped
     * objects. */
    for (int i = 0; i < TASK_MAX; i++) {
        ktcb_registry[i].tcb        = &ktcb_backing[i];
        ktcb_registry[i].generation = 0;
        ktcb_registry[i].occupied   = 0;
        ktcb_registry[i].bootstrap  = 0;
        task_reset_slot(&ktcb_backing[i]);
    }

    struct task *idle = ktcb_registry[0].tcb;
    /* Idle is the single bootstrap exception: static backing, never retyped,
     * never reused, not built by the productive task builder. */
    task_registry_bind_idle(idle);
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
    int idx = -1;
    struct task *t = task_registry_find_free(&idx);
    if (!t) return 0;

    /* No task_reset_slot(t) here: task_backing_find_free only ever returns a
     * slot with state == TASK_DEAD, which is only reached already fully
     * zeroed (task_init at boot, or task_backing_free_on_destroy at death) —
     * re-zeroing now would wipe the t->reg_slot task_registry_alloc just set. */
    if (kstack_alloc(t, idx) != 0) return 0;

    task_init_fpu_state(t);
    t->id         = next_id++;
    t->state      = TASK_READY;
    t->priority   = TASK_PRIORITY_DEFAULT;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->home_cpu   = 0;

    setup_initial_context(t, entry);

    /* Fase S2 D2: object init — the scheduler's own execution reference,
     * dropped by task_execution_teardown_off_cpu at termination.  No process
     * handle table for a plain kernel task, so no separate handle is minted. */
    ktcb_object_init(t);

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

    t = task_registry_find_free(&idx);
    if (!t) return 0;

    /* No task_reset_slot(t) here — see task_create. */
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

    t->saved_krsp = kstack_top;

    t->ctx.r15    = 0; t->ctx.r14 = 0; t->ctx.r13 = 0; t->ctx.r12 = 0;
    t->ctx.rbx    = 0; t->ctx.rbp = 0;
    t->ctx.rip    = (uint64_t)(uintptr_t)user_entry_trampoline;
    t->ctx.rflags = 0x202ULL;
    task_set_bootstrap_arg0(t, arg0);

    t->utext_phys  = ub_copy_phys;
    t->utext_pages = ub_pages;

    /* Fase S2 D2: the KTCB IS t itself.  ktcb_object_init sets refcount = 1,
     * the scheduler's own execution reference (dropped at termination by
     * task_execution_teardown_off_cpu).  handle_table_insert takes its own
     * reference internally (handle_entry_init: lifecycle + active retain,
     * released by handle_table_close_all/handle_entry_reset on teardown) —
     * no separate retain needed here; on failure it takes none. */
    ktcb_object_init(t);
    handle_table_insert(&proc->handle_table, &t->base,
                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);

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
    if (t) {
        /* Every goto-fail above happens after kstack_alloc succeeded but
         * before ktcb_object_init/rq_enqueue — undo exactly the registry
         * claim + kstack alloc task_registry_find_free/kstack_alloc made
         * (task_reset_slot no longer frees the kstack or touches the
         * registry; see task_execution_teardown_off_cpu for the normal
         * teardown this mirrors). */
        kstack_free(t, (int)(t - ktcb_backing));
        t->kstack = 0; t->kstack_phys = 0;
        task_registry_release(t);
        task_reset_slot(t);
    }
    return 0;
}

struct task *task_spawn_user(uint64_t arg0) {
    return task_create_user_impl(arg0);
}

/* Abort a task that task_create_user_impl fully built (registered, ktcb
 * object, queued) but a later bootstrap step failed before it ever ran —
 * called from kernel_main, pre-scheduler-start, so t is guaranteed off-CPU. */
void task_abort_spawned_user(struct task *t) {
    task_execution_teardown_off_cpu(t);
}

struct task *task_thread_create(struct KProcess *proc, uint64_t entry_vaddr,
                                uint64_t user_rsp, uint64_t arg) {
    if (!proc || !proc->cr3 || proc->teardown_complete) return 0;

    struct task *t = 0;
    int idx = -1;
    t = task_registry_find_free(&idx);
    if (!t) return 0;

    /* No task_reset_slot(t) here — see task_create. */
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

    t->saved_krsp = kstack_top;

    t->ctx.r15    = 0; t->ctx.r14 = 0; t->ctx.r13 = 0; t->ctx.r12 = 0;
    t->ctx.rbp    = 0;
    t->ctx.rbx    = arg;
    t->ctx.rip    = (uint64_t)(uintptr_t)user_entry_trampoline;
    t->ctx.rflags = 0x202ULL;

    proc->thread_count++;

    /* Fase S2 D2: the KTCB IS t itself — see task_create_user_impl (insert
     * takes its own reference; no separate retain needed). */
    ktcb_object_init(t);
    handle_table_insert(&proc->handle_table, &t->base,
                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);

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
    if (!t || t == current_task || t->state == TASK_DEAD || t->terminal) return;
    task_execution_teardown_off_cpu(t);
}

/*
 * Fase S2 D2: task_exit_current runs ON the dying task's own kernel stack, so
 * it must do nothing that task_execution_teardown_off_cpu's OFF-CPU
 * precondition forbids (freeing that kstack) and nothing the off-CPU pass
 * duplicates (cancelling waits, freeing user pages, thread_count/process
 * teardown, the object's execution reference) — all of that is deferred to
 * the reaper, which calls task_execution_teardown_off_cpu(t) once this task
 * has actually context-switched away (scheduler.c: old->state == TASK_DEAD ->
 * reap_enqueue_dead -> reap_pending_dead_task).
 */
void task_exit_current(void) {
    struct task *t = task_current();
    if (!t) return;

    /* A1.11: the slot stays reserved until the deferred reaper runs.
     * Without this, an immediate task_create could recycle the slot, wipe
     * t->process, and the reaper's TASK_DEAD guard would silently skip the
     * stale queue entry — leaking the KProcess, its address space, the
     * sched_ctx ref and the live count (found by T112 spawn/exit churn). */
    t->awaiting_reap = 1;
    t->state = TASK_DEAD;
    for (;;) task_yield();
}

void task_kill_process(struct KProcess *proc) {
    if (!proc) return;
    for (int i = 0; i < TASK_MAX; i++) {
        if (!ktcb_registry[i].occupied) continue;
        struct task *t = ktcb_registry[i].tcb;
        if (t->state != TASK_DEAD && t->process == proc)
            task_kill_external(t);
    }
}
