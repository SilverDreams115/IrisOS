#include "scheduler_priv.h"
#include <iris/smp.h>
#include <iris/lapic.h>
#include <iris/pmm.h>
#include <iris/tss.h>
#include <iris/paging.h>
#include <iris/syscall.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kfault.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kschedctx.h>
#include <iris/nc/kreply.h>
#include <iris/nc/ktcb.h>
#include <iris/nc/rights.h>
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
 * Ledger A-19 — no ceiling, and one static pair.
 *
 * `ktcb_registry[TASK_MAX]` is gone.  It held scheduler identity, it filled,
 * and `task_registry_alloc` then told a holder with memory and a capability
 * that it could not have another thread — a limit the kernel invented, which
 * seL4 does not have.  Everything that read it was WALKING it, so it is a list
 * now (`sched_thread_list`, linked through the TCB) and removal is O(1).
 *
 * `ktcb_backing[]` survives at TASK_BOOTSTRAP_MAX = 2: the idle thread and the
 * root task, built by boot code out of memory no Untyped exists for yet.  That
 * is the same exception seL4's root task is.  Every other thread is retyped
 * from an Untyped somebody holds, and its storage is that region's.
 */
struct task         ktcb_backing[TASK_BOOTSTRAP_MAX];  /* idle + root task */
struct task        *sched_thread_list = 0;

/*
 * The list's lock (SMP roadmap §9.1, rank 5).
 *
 * `sched_thread_list` is the one piece of scheduler state every CPU touches:
 * thread creation pushes onto it, termination unlinks from it, and the
 * DISPATCHER walks it twice on every idle looking for a deadline to fast
 * forward to or a budget that has come back.  A walk racing an unlink reads a
 * `sched_next` out of a TCB that is being reset — which on one core cannot
 * happen, because neither the dispatcher nor a syscall can be interrupted into
 * the other.
 *
 * IRQ-OFF, and that is not a default.  The TICK walks this list too
 * (`scheduler_tick`, in interrupt context).  A plain spinlock would let the
 * tick interrupt a CPU that is halfway through the dispatcher's walk and then
 * spin for a lock its own CPU holds, which is a deadlock with one core, never
 * mind two.
 */
irq_spinlock_t      sched_list_lock;

/*
 * Is a processor still standing on this thread?  (SMP roadmap §9.3 step 4.)
 *
 * This used to be `t == current_task` — one pointer for the whole machine, so
 * the question could only ever be asked about the CPU doing the asking.  With
 * one core that was the same question.  With four it is not: tearing a thread
 * down frees the stack and the address space an instruction pointer on another
 * core may still be inside, and "not me" is not "nobody".
 *
 * The answer is the thread's own `on_cpu` and not a scan of
 * `cpu_local[].current_task`, because the scan answers one instruction too
 * early: a dispatcher points its CPU at the incoming thread before it has
 * finished releasing the outgoing one.  See `on_cpu` in task.h.
 *
 * No lock, and it does not need one.  The callers ask about a thread that is
 * already DEAD and already out of every run queue, and the only thing that
 * raises `on_cpu` is a dispatch — which takes its thread from a run queue.  So
 * the answer can only go from yes to no, which is the direction that makes an
 * unlocked read safe.
 */
static int task_is_on_some_cpu(const struct task *t) {
    return atomic_load_explicit(&t->on_cpu, memory_order_acquire) != 0u;
}

/* ── Phase S2 D2 — registry + backing instrumentation (QUERY kind 4) ── */
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
    /* Cannot fail.  It used to scan ktcb_registry[TASK_MAX] and return -1 when
     * the array was full — the kernel telling a holder with memory and a
     * capability that it may not have another thread. */
    if (t->reg_slot >= 0) return 0;              /* already listed */
    uint64_t lf = irq_spinlock_lock(&sched_list_lock);
    t->sched_prev = 0;
    t->sched_next = sched_thread_list;
    if (sched_thread_list) sched_thread_list->sched_prev = t;
    sched_thread_list = t;
    t->reg_slot = 1;
    irq_spinlock_unlock(&sched_list_lock, lf);
    uint32_t n = atomic_fetch_add_explicit(&reg_active, 1u, memory_order_relaxed) + 1u;
    uint32_t hw = atomic_load_explicit(&reg_hwm, memory_order_relaxed);
    while (n > hw &&
           !atomic_compare_exchange_weak_explicit(&reg_hwm, &hw, n,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
    return 0;
}

/* Scheduler membership ends at TERMINATION, not at destruction: a surviving
 * capability to a terminated TCB keeps the object, never a place in the walk. */
static void task_registry_release(struct task *t) {
    if (!t || t->reg_slot < 0) return;
    uint64_t lf = irq_spinlock_lock(&sched_list_lock);
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else               sched_thread_list         = t->sched_next;
    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    t->sched_prev = 0;
    t->sched_next = 0;
    t->reg_slot   = -1;
    irq_spinlock_unlock(&sched_list_lock, lf);
    atomic_fetch_sub_explicit(&reg_active, 1u, memory_order_relaxed);
}

/* Bootstrap: the idle thread joins the list like anything else.  It is the
 * single BOOTSTRAP_EXCEPTION of the ledger: static backing, no cap-visible
 * object, never retyped; marked configured so no execution gate can misread it
 * as an inactive retyped TCB. */
static void task_registry_bind_idle(struct task *idle) {
    idle->reg_slot = -1;
    (void)task_registry_alloc(idle);
    idle->configured = 1;
}

/* Storage lifetime — find a free backing slot (state == TASK_DEAD).  Slot 0 is
 * the idle bootstrap exception.  awaiting_reap excludes a slot that a
 * self-exited task has already marked TASK_DEAD but the deferred reaper has
 * not yet torn down (A1.11: reusing it early would wipe a still-referenced
 * t->process out from under the pending reap).  Returns NULL when the
 * backing pool is full. */
static struct task *task_backing_find_free(void) {
    for (int i = 1; i < TASK_BOOTSTRAP_MAX; i++) {
        if (ktcb_backing[i].state == TASK_DEAD && !ktcb_backing[i].awaiting_reap)
            return &ktcb_backing[i];
    }
    return 0;
}

/*
 * Allocate a fresh TCB: a free backing slot (storage) plus a free registry
 * slot (scheduler identity).  Returns NULL on either exhaustion.
 *
 * Stage 5 Step 4: it no longer reports the BACKING index.  That index was
 * used to address the per-slot kernel-stack region, which tied a thread's
 * kernel stack to the fact that its storage came from the static pool — a
 * TCB retyped from an Untyped has no such index.  The kstack is keyed by the
 * registry slot now, which every executing thread has by definition.
 */
/* The bootstrap pair's allocator: the root task takes the one slot idle does
 * not.  Every other thread is retyped and joins the list at configure time. */
static struct task *task_registry_find_free(void) {
    struct task *t = task_backing_find_free();
    if (!t) return 0;
    (void)task_registry_alloc(t);
    return t;
}

/*
 * The idle thread, named (SMP roadmap §9.3 step 4).
 *
 * It was `task_list_head`, the head of a CIRCULAR LIST threaded through
 * `task->next` — and that list had exactly two uses left: this pointer, which
 * three places compare against to mean "the idle thread", and the walk that
 * removed a dying thread from it.  Nothing else read it, so
 * it was a list maintained in order to be maintained.
 *
 * On one core that was merely redundant.  With four it was an unlocked shared
 * mutable structure on the thread create and destroy paths — two writes to
 * `tail->next` and `tail` with nothing between them, and a walk that follows
 * `next` while another core is splicing it — which is the one thing §9.2's
 * catalog is for.  The choice was to give it the ninth lock or to notice that
 * the list itself was the only thing that needed one.
 *
 * The threads still have a list, and it is the one the kernel actually uses:
 * `sched_thread_list` under `sched_list_lock`, walked by the tick and by the
 * idle fast-forward.  This is a pointer to one thread.
 */
struct task        *sched_idle_thread = 0;
/* Thread ids are a DIAGNOSTIC (charter: nothing selects an object by number),
 * but a diagnostic that hands two threads the same id is a diagnostic that
 * lies.  Three call sites increment it and none held a lock. */
_Atomic uint32_t    next_id         = 0;
/* Phase S2: task_rsp[TASK_MAX] retired — kernel RSP moved into struct task.saved_krsp */
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
 * CAPACITY IS DERIVED, not guessed.  A task enters this queue by dying, and a
 * task dies on the CPU it was running on — so between two reap calls there can
 * be at most one new entry per CPU.  `MAX_CPUS` entries is therefore not a
 * headroom estimate but a bound, and a ring needs one slot more than it holds.
 *
 * It used to be 8 with the comment "8 > realistic concurrent deaths", which is
 * the shape of reasoning that is fine until it is not: `MAX_CPUS` is also 8, a
 * ring of 8 holds 7, and eight CPUs each with a dying task overflow it by one.
 * The overflow path leaks the task's slot silently, which is the kind of bug
 * that shows up as a slow drift in an object count months later.
 *
 * The per-CPU dead lists this TODO used to name are still the better shape for
 * a different reason — they remove the cross-CPU cache line, not a correctness
 * problem — and they belong with the per-CPU timer in §9.3 step 4.
 */
#define REAP_QUEUE_SIZE 16u   /* power of two, > MAX_CPUS: see above */
_Static_assert(REAP_QUEUE_SIZE > MAX_CPUS,
               "the reap ring must hold one dying task per CPU, plus the "
               "empty slot a ring needs to tell full from empty");
_Static_assert((REAP_QUEUE_SIZE & (REAP_QUEUE_SIZE - 1u)) == 0u,
               "REAP_QUEUE_SIZE is used as a mask");
static struct task    *reap_queue[REAP_QUEUE_SIZE];
static unsigned int    reap_queue_head = 0;   /* producer index (write) */
static unsigned int    reap_queue_tail = 0;   /* consumer index (read)  */
static irq_spinlock_t  reap_queue_lock;

/* ── O(1) per-CPU run queue ──────────────────────────────────────────────── */

static struct CpuRunQueue cpu_rqs[MAX_CPUS];
_Atomic uint32_t          sched_live_count;

/*
 * Phase 17 — additive scheduler instrumentation (silent, ABI-safe, exposed
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

/*
 * How many deaths have not finished.
 *
 * Ring 3 needs this because "has the reaper caught up" stopped being
 * answerable by yielding a fixed number of times the moment a thread could die
 * on a processor other than the one asking (SMP roadmap §9.3 step 4).  The
 * reap ring's high-water mark beside it says how deep the ring has ever been;
 * only a CURRENT depth can be waited on.
 *
 * It is the ring's depth PLUS the threads that are dead and not in it yet, and
 * the second half is the half that matters.  A thread killed from another
 * processor is marked DEAD where it stands and reaches the ring only when its
 * own core next dispatches — so the ring can be empty while a death is very
 * much still in flight, and a waiter that looked only at the ring would
 * declare the system settled and then measure a thread that is about to
 * disappear.
 *
 * Derived by walking, not maintained in a variable: a counter would be a third
 * place every death path has to remember to touch, and this is asked only when
 * somebody is already waiting.  The two locks are taken one after the other
 * rather than nested — there is no moment the sum has to be consistent, since
 * the caller is waiting for it to reach zero and stay there.
 */
uint32_t sched_deaths_pending(void) {
    uint64_t f = irq_spinlock_lock(&reap_queue_lock);
    unsigned int ring = (reap_queue_head - reap_queue_tail) & (REAP_QUEUE_SIZE - 1u);
    irq_spinlock_unlock(&reap_queue_lock, f);

    uint32_t marked = 0;
    uint64_t lf = irq_spinlock_lock(&sched_list_lock);
    for (struct task *t = sched_thread_list; t; t = t->sched_next)
        if (t->state == TASK_DEAD) marked++;
    irq_spinlock_unlock(&sched_list_lock, lf);

    return (uint32_t)ring + marked;
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

/* A thread's queue is (its domain, its priority).  The domain is read here
 * rather than passed in, so every enqueue path gets it without knowing it
 * exists — which is what makes adding domains a change to three functions
 * instead of to every caller. */
static inline uint32_t rq_dom_of(const struct task *t) {
    uint32_t d = (uint32_t)t->domain;
    return (d < IRIS_NUM_DOMAINS) ? d : 0u;
}

void rq_enqueue(struct task *t) {
    struct CpuRunQueue *rq = cpu_local[t->home_cpu].rq;
    if (!rq) return;
    int prio = (int)(uint8_t)t->priority;
    uint32_t dom = rq_dom_of(t);
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    if (t->rq_queued) {
        /* S4 guard engaged: task already queued — reject the duplicate. */
        atomic_fetch_add_explicit(&sched_dup_enq, 1u, memory_order_relaxed);
        irq_spinlock_unlock(&rq->lock, flags);
        return;
    }
    t->rq_queued = 1;
    t->rq_next   = 0;
    if (rq->head[dom][prio] == 0) {
        rq->head[dom][prio] = t;
        rq->tail[dom][prio] = t;
        rq->mask[dom][prio >> 6] |= (1ULL << (prio & 63));
    } else {
        rq->tail[dom][prio]->rq_next = t;
        rq->tail[dom][prio]          = t;
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
    uint32_t dom = rq_dom_of(t);
    struct task *prev = 0, *cur = rq->head[dom][prio];
    while (cur && cur != t) { prev = cur; cur = cur->rq_next; }
    if (!cur) { t->rq_queued = 0; rq_live_dec(); irq_spinlock_unlock(&rq->lock, flags); return; }
    struct task *nxt = t->rq_next;
    if (!prev)                        rq->head[dom][prio] = nxt;
    else                              prev->rq_next       = nxt;
    if (rq->tail[dom][prio] == t)     rq->tail[dom][prio]  = prev;
    if (rq->head[dom][prio] == 0)
        rq->mask[dom][prio >> 6] &= ~(1ULL << (prio & 63));
    t->rq_queued = 0;
    t->rq_next   = 0;
    rq_live_dec();
    irq_spinlock_unlock(&rq->lock, flags);
}

/*
 * The highest-priority runnable thread OF THE CURRENT DOMAIN.
 *
 * This is where a time partition actually happens.  A priority-255 thread in
 * domain 1 is invisible here while domain 0 holds the CPU — not deprioritised,
 * not skipped over, but in a set of queues this function does not read.  That
 * is what makes the cost of dispatching independent of what other domains
 * hold, and the cost is the channel: a search that had to step over other
 * domains' threads would take longer when they had more, which is a fact about
 * them measurable from here.
 */
struct task *rq_dequeue_best(void) {
    struct CpuRunQueue *rq = cpu_self()->rq;
    if (!rq) return 0;
    uint32_t dom = (uint32_t)atomic_load_explicit(&iris_cur_domain,
                                                  memory_order_relaxed);
    if (dom >= IRIS_NUM_DOMAINS) dom = 0u;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    for (int w = 3; w >= 0; w--) {
        if (!rq->mask[dom][w]) continue;
        int bit  = 63 - __builtin_clzll(rq->mask[dom][w]);
        int prio = w * 64 + bit;
        struct task *t   = rq->head[dom][prio];
        struct task *nxt = t->rq_next;
        rq->head[dom][prio] = nxt;
        if (nxt == 0) {
            rq->tail[dom][prio] = 0;
            rq->mask[dom][w] &= ~(1ULL << bit);
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
    uint32_t dom = (uint32_t)atomic_load_explicit(&iris_cur_domain,
                                                  memory_order_relaxed);
    if (dom >= IRIS_NUM_DOMAINS) dom = 0u;
    uint64_t flags = irq_spinlock_lock(&rq->lock);
    int result = -1;
    for (int w = 3; w >= 0; w--) {
        if (rq->mask[dom][w]) {
            result = w * 64 + (63 - __builtin_clzll(rq->mask[dom][w]));
            break;
        }
    }
    irq_spinlock_unlock(&rq->lock, flags);
    return result;
}

/*
 * Move a thread between scheduling domains, requeueing it if it was queued.
 *
 * The REQUEUE is the operation, not the field write.  A runnable thread sits
 * in the queue of (domain, priority); writing the new domain and leaving it
 * where it was would leave it dispatchable in its OLD domain and invisible in
 * its new one — the partition failing in both directions at once.
 *
 * It lives here rather than in the syscall layer because the run queues do,
 * and because `rq_remove`/`rq_enqueue` are scheduler-private: a syscall that
 * had to reach into them would be a syscall that knows how dispatch is
 * implemented.
 */
void sched_set_domain(struct task *t, uint8_t domain) {
    if (!t || t->domain == domain) return;
    int was_queued = t->rq_queued;
    if (was_queued) rq_remove(t);
    t->domain = domain;
    if (was_queued) rq_enqueue(t);
}

/*
 * Which processor does a new thread belong to?  (SMP roadmap §9.3 step 4.)
 *
 * Round robin over the processors that are actually online, which is the
 * policy a kernel should have and not a placeholder for a better one: seL4
 * does not balance either.  A thread's core is chosen once, by whoever
 * configured it, and thereafter it is a property of the thread — because a
 * kernel that MOVES threads has to decide when, and "when" is a policy that
 * belongs to a scheduler in ring 3, not to the dispatcher.
 *
 * `smp_online_count()` and not MAX_CPUS: a processor that never arrived has a
 * run queue nothing dequeues from, and homing a thread there would be a thread
 * that is runnable and never runs.
 *
 * The counter is relaxed on purpose.  Two configures racing may both get the
 * same core, and the cost of that is one imbalanced thread — against a
 * read-modify-write on every thread creation to make a decision that is
 * already arbitrary.
 */
static _Atomic uint32_t home_cpu_rr;

static uint8_t sched_pick_home_cpu(void) {
    uint32_t n = smp_online_count();
    if (n > MAX_CPUS) n = MAX_CPUS;
    if (n <= 1u) return 0u;
    uint32_t i = atomic_fetch_add_explicit(&home_cpu_rr, 1u, memory_order_relaxed);
    return (uint8_t)(i % n);
}

void task_wakeup(struct task *t) {
    /* Phase S2 D2: t->terminal guards against a wakeup arriving mid-teardown
     * (e.g. kreply_cancel_caller waking its own caller when that caller is
     * the very task task_execution_teardown_off_cpu is unwinding) — state
     * alone is not enough once TASK_TERMINATED exists between READY/BLOCKED*
     * and TASK_DEAD; a terminal task must never re-enter the run queue. */
    if (!t || t->state == TASK_DEAD || t->terminal) return;
    t->state = TASK_READY;
    if (t != sched_idle_thread) {
        rq_enqueue(t);
        smp_send_reschedule(t->home_cpu);
    }
}

/*
 * Make the processor that is running `t` notice that `t` should stop.
 * (SMP roadmap §9.3 step 4.)
 *
 * Changing a thread's state from another core changes nothing about the core
 * executing it.  That core is in ring 3, or on its way back there; it will not
 * look at the thread's TCB again until something brings it into the kernel,
 * and nothing does.  So `Suspend` on a thread running elsewhere used to set
 * SUSPENDED, remove it from a run queue it was not in, and return success to a
 * caller whose thread went on running — which is what T083 caught the moment
 * threads began to spread across processors.
 *
 * The reschedule IPI is what brings that core in.  Its handler marks whatever
 * the core is running and the interrupt's own exit path runs the dispatcher,
 * which reads the new state and declines to put the thread back.
 *
 * It is not synchronous and does not need to be: the caller is told the thread
 * WILL stop, not that it already has, and every path that must not race a
 * still-running thread — teardown above all — waits on `on_cpu` instead, which
 * is the fact rather than a proxy for it.  A core caught in ring 0 acts on the
 * mark when it next leaves, and at the latest on its next tick.
 */
static void sched_kick(struct task *t) {
    if (!t) return;
    t->need_resched = 1;
    if (!atomic_load_explicit(&t->on_cpu, memory_order_acquire)) return;
    smp_send_reschedule(t->home_cpu);
}

void task_suspend(struct task *t) {
    if (!t || t->state == TASK_DEAD || t->terminal) return;
    t->state = TASK_SUSPENDED;
    rq_remove(t);
    sched_kick(t);

    /*
     * And WAIT for it to actually stop — seL4 calls this stalling the remote
     * thread, and it is the difference between an answer and a promise.
     *
     * Marking a thread SUSPENDED and sending its core an IPI says it WILL
     * stop.  The caller was told it HAS.  Between the two, the thread keeps
     * executing ring-3 instructions on another processor — which is not a
     * theoretical window: T083 suspends a helper, reads its counter, and
     * requires the counter to be frozen, and the helper incremented it once
     * more after Suspend returned.  Every caller of Suspend is in that
     * position; most of them just have no counter to notice with.
     *
     * Bounded, and by something real.  The IPI is taken as soon as the target
     * core has interrupts on, which is immediately in ring 3 and at the end of
     * the current syscall in ring 0 (SFMASK clears IF on entry) — and the
     * dispatch it forces clears `on_cpu` before it does anything else with the
     * thread.  Nothing can deadlock against it: clearing `on_cpu` is a store on
     * a straight-line path that waits for nothing, so there is no cycle to
     * close, and this spin holds no lock — the syscall layer holds a reference
     * to the target, not a lock on it.
     *
     * No timeout, for the reason the TLB shootdown has none: a core that never
     * answers is a core that is wedged, and continuing past it would mean
     * telling a caller its thread has stopped when it has not.
     *
     * PRECONDITION, and it is why the external KILL below does not do this:
     * the caller must hold no lock that the target core's dispatch can need.
     * `Suspend`'s one caller holds a reference to the target and nothing else.
     * A killer, by contrast, is often already inside an endpoint — and the
     * dispatch it would be waiting for reaps dead threads, which cancels their
     * blocked waits, which takes endpoint locks.  That is a cycle, so a kill
     * marks and leaves rather than stalls.
     */
    if (t == task_current()) return;    /* suspending ourselves; we stop on the way out */
    while (atomic_load_explicit(&t->on_cpu, memory_order_acquire))
        __asm__ volatile ("pause");
}

/* Initial FPU state captured at boot; copied into every new task. */
uint8_t initial_fpu_state[512] __attribute__((aligned(16)));

/* ── Internal helpers ────────────────────────────────────────────────────── */

/*
 * The boot thread's entry, kept only because `sched_idle_thread` is still the
 * object the run queue excludes and the fast-forward skips.
 *
 * It used to be the IDLE LOOP, and that is why IRIS had an idle task at all:
 * every "nobody else can run" answer had to be a switch to a thread, and a
 * thread needs a stack.  Stage 9-evt step 3 made the answer a `hlt` on the
 * core's own stack, so nothing schedules this and nothing ever enters it —
 * `kernel_main` enters the dispatcher directly instead of falling into here.
 * Reaching it would mean the run queue handed out a task it excludes.
 */
static void idle_task(void) {
    for (;;) __asm__ volatile ("sti; hlt; cli");
}

/*
 * task_find_by_id — REMOVED (Stage 7 Step 7).
 *
 * It scanned the registry for a thread with a given id, and its last caller
 * was SYS_EXCEPTION_RESUME: the one place a global identifier still SELECTED
 * a kernel object.  A fault now hands the handler the faulting thread as a
 * CAPABILITY, so there is nothing left to look up — and nothing left that can
 * turn a number into a thread, which is the property charter §3.4/§3.5 ask for
 * rather than the absence of one particular caller.
 */

void task_init_fpu_state(struct task *t) {
    uint8_t *dst       = t->fpu_state;
    const uint8_t *src = initial_fpu_state;
    for (uint32_t i = 0; i < 512; i++) dst[i] = src[i];
}

/*
 * Phase S2 D2 — initialize a backing slot to the free (TASK_DEAD) state.  Used
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
 * Phase S2 D2 — the KTCB object destructor (called from ktcb.c when the last
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

/*
 * unlink_task is DELETED (SMP roadmap §9.3 step 4).
 *
 * It removed a dying thread from the circular `task->next` list by walking it
 * for the predecessor.  That list is gone — see `sched_idle_thread` above for
 * why — and the list a dying thread must actually leave is `sched_thread_list`,
 * which `task_registry_release` already removes it from, under the lock, in
 * O(1), because those links are doubly threaded.
 */

static void task_cancel_blocked_waits(struct task *t) {
    if (!t) return;
    /* Phase 13/Track G: kchannel_cancel_waiter retired — no task blocks on a
     * KChannel (the object is gone). */
    knotification_cancel_waiter(t);
    kendpoint_cancel_waiter(t);
    /* Ph85: cancel pending KReply (task was in TASK_BLOCKED_REPLY). */
    if (t->pending_kreply) {
        struct KReply *r = t->pending_kreply;
        t->pending_kreply = 0;
        kreply_cancel_caller(r); /* clears r->caller; sets caller READY (no-op here) */
        kobject_release(&r->base);
    }
    /* Phase S1: release a staged-but-unbound explicit reply object (task died
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
        /* Phase S2: unbind first so the SC keeps no stale bound_task pointer
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
 * Phase S2 D2 — execution teardown (shared by self-exit and external kill).
 * Ends the EXECUTION, REGISTRY and (frees) execution resources, but NOT the
 * object: it drops the scheduler's execution reference last, so the object is
 * destroyed here only if no capability references it.  A surviving cap keeps
 * the (TERMINATED) object — and its backing slot — alive until the cap closes.
 *
 * Preconditions: t is OFF-CPU (never the running task on its own kstack).
 */
static void task_execution_teardown_off_cpu(struct task *t) {
    if (!t) return;

    /*
     * CLAIM the thread, exactly once, before anything is released.
     *
     * The test used to be `if (t->terminal) return;` — an unlocked read that
     * four processors calling Exit on the same thread all pass, so all four
     * ran this function on it.  Everything below releases a reference or a
     * slot, so four teardowns meant four releases of one reference: the
     * registry slot, the CSpace, the address space, the scheduling context.
     * The symptom was `kobject_retain: resurrect from refcount 0` on an object
     * whose type field read 0 — a type nothing creates, because the storage
     * had already gone back to its Untyped and been zero-filled.  T350 is the
     * test: four cores killing the same four threads.
     *
     * An exchange makes "am I the one" and "say so" a single act, which is the
     * whole of what was missing.
     */
    if (atomic_exchange_explicit(&t->terminal, (uint8_t)1u,
                                 memory_order_acq_rel) != 0u)
        return;

    /*
     * Make t unreachable and unwakeable FIRST, atomically with respect to a
     * timer IRQ, before any of the slower calls below (the CSpace and address
     * space releases especially) that can run long enough to be interrupted.  scheduler_tick and sched_handle_idle walk sched_thread_list
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
    /* Stage 5 Step 4: the kernel stack goes back WITH the registry slot, not
     * later.  The stack's virtual slot is keyed by the registry index, so
     * releasing the index first opens a window in which the next thread claims
     * that index and maps its stack over a range this task has not unmapped
     * yet — and then this teardown unmaps the stack the new thread is running
     * on.  t is off-CPU here (the function's precondition), so nothing is
     * standing on the stack being freed. */
    task_registry_release(t);
    t->awaiting_reap = 0;
    t->state    = TASK_TERMINATED;
    /* `terminal` was claimed at the top; this is where it used to be set, and
     * the ordering that mattered — unreachable and unwakeable before the slow
     * releases below — is still what this section establishes. */
    __asm__ volatile ("pushq %0; popfq" : : "r"(irq_flags) : "memory");

    task_cancel_blocked_waits(t);
    free_user_stack_pages(t);
    free_user_text_pages(t);

    /*
     * Stage 7-proc: no process to detach from, and nothing to count.
     *
     * A thread count reaching zero used to be what tore down an address space
     * and emptied a CSpace.  Both are driven by capabilities now — an address
     * space is invalidated by its own close hook and destroyed by its
     * destructor, a CSpace empties itself when its last holder goes — so the
     * releases below ARE the teardown, and they happen for every thread rather
     * than for a distinguished last one.
     */

    atomic_fetch_sub_explicit(&sched_live_count, 1u, memory_order_relaxed);
    task_release_sched_ctx(t);

    /*
     * Stage 7 Step 10: tell whoever is watching this thread that it is over.
     *
     * Fired here, at the end of EXECUTION teardown, and before the references
     * this thread holds are dropped — a watcher woken by it can immediately
     * read the exit code off the TCB it holds, because the object outlives the
     * execution for exactly as long as some capability names it.
     *
     * Once, and the flag is what makes it once: an external kill and a
     * self-exit both arrive here, and a watcher that learned of a death twice
     * would have no way to tell that from two deaths.
     */
    if (t->exit_notif && !t->exit_reported) {
        struct KNotification *n = t->exit_notif;
        t->exit_reported = 1;
        knotification_signal(n, t->exit_bits);
    }

    /* Stage 7 Step 4: the thread's own CSpace reference goes with its
     * execution.  Dropped BEFORE the TCB release below, so a process whose last
     * thread is exiting still has its root emptied by thread teardown and
     * not by this release racing it. */
    if (t->cspace_root) {
        struct KCNode *cs = t->cspace_root;
        t->cspace_root = 0;
        kobject_active_release(&cs->base);
        kobject_release(&cs->base);
    }
    /* ...and its address space, after the address-space reap above has
     * torn the walk down.  Releasing here is dropping a reference, not
     * reaping: the object survives while any capability to it does. */
    if (t->vspace) {
        struct KVSpace *vs = t->vspace;
        t->vspace = 0;
        kobject_active_release(&vs->base);
        kobject_release(&vs->base);
    }
    /*
     * Stage 7 Step 12: a dead thread has no pending fault, and its handler
     * registration goes with it.
     *
     * thread teardown used to clear the process's record so a late read
     * honestly answered WOULD_BLOCK; the record is the thread's now, so the
     * clearing is too — and the counter that made that observable
     * (kfault_cleanup) still counts exactly the records actually cleared.
     */
    {
        /* Under the thread's own obj_lock, which is what the other writer
         * (SYS_TCB_SET_FAULT_HANDLER) holds.  `terminal` is set above with
         * interrupts off and re-read there under this lock, so a registration
         * that passed the terminal check cannot install into a thread this
         * block has already emptied — the references it took would have had
         * nobody left to release them. */
        struct KEndpoint *fe;
        /* Stage 8-mcs: the TIMEOUT registration is a second, independent
         * reference and is emptied under the same lock hold.  Missing it would
         * leak an endpoint per thread that ever armed a timeout handler — the
         * exact shape of leak this block exists to prevent for the exception
         * handler. */
        struct KEndpoint *te;
        int               had_fault;
        uint64_t irqfl = irq_spinlock_lock(&t->obj_lock);
        had_fault      = t->fault_valid;
        t->fault_valid = 0;
        fe = t->fault_ep;   t->fault_ep   = 0;
        te = t->timeout_ep; t->timeout_ep = 0;
        t->timeout_pending = 0;
        irq_spinlock_unlock(&t->obj_lock, irqfl);
        if (had_fault) kfault_stat_cleanup();
        if (fe) { kobject_active_release(&fe->base); kobject_release(&fe->base); }
        if (te) { kobject_active_release(&te->base); kobject_release(&te->base); }
    }

    /* A-23: the BOUND notification.  Broken from the thread's side, because a
     * notification that outlives its bound thread would keep signalling into a
     * pointer that is about to be freed. */
    knotification_unbind_task(t);

    /* D-4: the registered IPC buffer.  Held with active+lifecycle refs, so a
     * thread that dies still owning its buffer gives the frame back and its
     * Untyped can be reset — the page was the user's, not the kernel's. */
    if (t->ipc_buffer) {
        struct KFrame *b = t->ipc_buffer;
        t->ipc_buffer = 0;
        t->ipc_buffer_uvaddr = 0u;
        kobject_active_release(&b->base);
        kobject_release(&b->base);
        ipc_buffer_gauge_drop();
    }

    /* The watch's own reference, dropped after it has fired. */
    if (t->exit_notif) {
        struct KNotification *n = t->exit_notif;
        t->exit_notif = 0;
        kobject_active_release(&n->base);
        kobject_release(&n->base);
    }

    /* The kernel stack was freed above, with the registry slot it is keyed
     * by; these fields are already clear. */


    /* Drop the scheduler's execution reference LAST.  If no cap references the
     * object, this triggers task_backing_free_on_destroy (slot → TASK_DEAD).
     * A surviving cap keeps the TERMINATED object (and its backing) alive. */
    kobject_release(&t->base);
}

/* Phase 16: reap-queue depth high-water, for lifecycle-churn diagnostics
 * (exposed additively via SYS_SCHED_INFO).  Monotonic; proves the deferred
 * reaper drains under pressure (T114/T118) — if it ever approached
 * REAP_QUEUE_SIZE the "cannot occur on single-CPU" assumption would be
 * broken and dead tasks would leak their slots. */
/* Written under reap_queue_lock, read without it — the type now says so. */
static _Atomic uint32_t reap_queue_hwm = 0u;
/* Entries the ring could not take.  Structurally zero (see REAP_QUEUE_SIZE);
 * kept because a leak and an un-drained queue look identical from outside. */
static _Atomic uint32_t reap_queue_drops = 0u;

uint32_t sched_reap_queue_drops(void) {
    return atomic_load_explicit(&reap_queue_drops, memory_order_relaxed);
}

uint32_t sched_reap_queue_hwm(void) {
    return __atomic_load_n(&reap_queue_hwm, __ATOMIC_RELAXED);
}

void reap_enqueue_dead(struct task *t) {
    uint64_t flags = irq_spinlock_lock(&reap_queue_lock);

    /*
     * Already waiting?  (SMP roadmap §9.3 step 4.)
     *
     * Two paths hand the same thread over now: the core it was running on, at
     * its next dispatch, and the core that KILLED it — which cannot tell
     * whether that dispatch has already happened.  A duplicate is harmless in
     * itself, because the second teardown sees `terminal` and returns; what it
     * is not harmless to is the ring's capacity, which is argued from one
     * entry per processor.  Sixteen slots and a scan of at most sixteen, under
     * a lock this path already takes.
     */
    for (unsigned int i = reap_queue_tail; i != reap_queue_head;
         i = (i + 1u) & (REAP_QUEUE_SIZE - 1u)) {
        if (reap_queue[i] == t) { irq_spinlock_unlock(&reap_queue_lock, flags); return; }
    }

    unsigned int next = (reap_queue_head + 1u) & (REAP_QUEUE_SIZE - 1u);
    if (next != reap_queue_tail) {
        reap_queue[reap_queue_head] = t;
        reap_queue_head = next;
        unsigned int depth = (reap_queue_head - reap_queue_tail) &
                             (REAP_QUEUE_SIZE - 1u);
        if (depth > atomic_load_explicit(&reap_queue_hwm, memory_order_relaxed))
            atomic_store_explicit(&reap_queue_hwm, depth, memory_order_relaxed);
    }
    else {
        /*
         * Unreachable by the capacity argument above, and counted anyway.
         *
         * If this ever moves, the argument is wrong and a dead task's slot is
         * leaking — which is otherwise invisible, because a leak looks exactly
         * like a system that has not reaped yet.  A counter turns "should not
         * happen" into something a test can assert is still zero.
         */
        atomic_fetch_add_explicit(&reap_queue_drops, 1u, memory_order_relaxed);
    }
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
    if (task_is_on_some_cpu(t)) {
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


/*
 * Stage 9-evt step 3 — describe a thread's FIRST entry into ring 3 in its TCB.
 *
 * This used to be a frame pushed onto the thread's own kernel stack at
 * creation — `[user_entry_trampoline, rip, cs, rflags, rsp, ss]` — which is
 * one of the two reasons every thread needed a stack before it had ever run.
 * The other was its saved ring-3 context, and that moved to `user_ctx` in the
 * first half of step 3.  Both live in the TCB now, so the dispatcher can start
 * a thread from the core's stack and a thread that has never run owns nothing.
 */
static void task_set_first_user_entry(struct task *t, uint64_t rip,
                                      uint64_t rsp, uint64_t arg) {
    for (unsigned i = 0; i < sizeof(t->user_ctx) / sizeof(uint64_t); i++)
        ((uint64_t *)&t->user_ctx)[i] = 0;
    t->user_ctx.rip    = rip;
    t->user_ctx.cs     = 0x23;      /* user code   */
    t->user_ctx.rflags = 0x202;     /* IF set      */
    t->user_ctx.rsp    = rsp;
    t->user_ctx.ss     = 0x1B;      /* user data   */
    t->user_ctx.rbx    = arg;       /* the thread argument, by ABI */
    t->resume_user     = TASK_RESUME_USER_FIRST;
    t->kentry          = 0;
}

/*
 * A KERNEL thread's entry.  There is no stack to build a frame on any more —
 * `kentry` is called by the dispatcher on the CORE's stack — so this records
 * where, and nothing else.
 */
void setup_initial_context(struct task *t, void (*entry)(void)) {
    t->ctx.r15    = 0;
    t->ctx.r14    = 0;
    t->ctx.r13    = 0;
    t->ctx.r12    = 0;
    t->ctx.rbx    = 0;
    t->ctx.rbp    = 0;
    t->ctx.rip    = (uint64_t)(uintptr_t)entry;
    t->ctx.rflags = 0x202ULL;
    /* Step 3: a KERNEL thread's resume is a call on the core's stack. */
    t->kentry      = entry;
    t->resume_user = TASK_RESUME_KERNEL;
}

/* ── Task creation ───────────────────────────────────────────────────────── */

void task_init(void) {
    __asm__ volatile ("fxsaveq (%0)" : : "r"(initial_fpu_state) : "memory");

    irq_spinlock_init(&reap_queue_lock);
    irq_spinlock_init(&sched_list_lock);
    kernel_cr3 = pml4_get_current();

    /*
     * Every processor's run queue, initialised and wired here — not when the
     * processor arrives (SMP roadmap §9.3 step 4).
     *
     * An AP's queue has to exist before the AP does, because a thread can be
     * homed to a CPU that has not started yet: `rq_enqueue` reads
     * `cpu_local[t->home_cpu].rq` on whatever core is doing the waking, and a
     * queue built by the arriving AP would leave a window where that read
     * returns NULL and the wakeup is silently dropped.  Building all of them
     * here costs a few kilobytes of zeroing at boot and removes the window
     * entirely.
     */
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        struct CpuRunQueue *rq = &cpu_rqs[c];
        irq_spinlock_init(&rq->lock);
        for (uint32_t d = 0; d < IRIS_NUM_DOMAINS; d++) {
            for (int i = 0; i < 256; i++) { rq->head[d][i] = 0; rq->tail[d][i] = 0; }
            rq->mask[d][0] = rq->mask[d][1] = rq->mask[d][2] = rq->mask[d][3] = 0;
        }
        cpu_local[c].rq = rq;
    }

    atomic_store_explicit(&sched_live_count, 1u, memory_order_relaxed); /* idle */

    /* The bootstrap pair: idle and the root task.  Every other thread is a TCB
     * retyped from an Untyped, and joins the scheduler's list when it is
     * configured — there is no array to wire. */
    for (int i = 0; i < TASK_BOOTSTRAP_MAX; i++)
        task_reset_slot(&ktcb_backing[i]);

    struct task *idle = &ktcb_backing[0];
    /* Idle is the single bootstrap exception: static backing, never retyped,
     * never reused, not built by the productive task builder. */
    task_registry_bind_idle(idle);
    idle->id    = atomic_fetch_add_explicit(&next_id, 1u, memory_order_relaxed);
    idle->state = TASK_RUNNING;

    setup_initial_context(idle, idle_task);
    task_init_fpu_state(idle);

    sched_idle_thread = idle;
    /* The BSP's slot, written directly rather than through set_current_task:
     * this runs on the BSP and is only ever about the BSP.  An AP arrives with
     * its own slot NULL, which is exactly right — it is running nothing until
     * its dispatcher picks something. */
    cpu_local[0].current_task = idle;
}

/*
 * task_create — DELETED (ledger A-19).
 *
 * It built a KERNEL thread out of the static backing pool, and nothing called
 * it: `scheduler_add_task` was its only caller and had none of its own.  It was
 * also the last way to make a thread that was not retyped from an Untyped
 * somebody holds, which is what let the pool shrink to the bootstrap pair.
 */

void task_set_bootstrap_arg0(struct task *t, uint64_t arg0) {
    if (!t || t->ring != TASK_RING3 || !t->vspace || !t->vspace->cr3) return;
    t->user_ctx.rbx = arg0;   /* the first-entry context, not a saved switch */
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
    struct KCNode  *root_cn = 0;
    struct KVSpace *root_vs = 0;
    uint64_t        root_cr3 = 0;
    uint64_t ustack_phys = 0;
    uint32_t ustack_pages = (uint32_t)((USER_STACK_SIZE / 4096ULL) - USER_STACK_GUARD_PAGES);
    uint64_t ub_copy_phys = 0;
    uint32_t ub_pages = 0;
    const void *ub_data = 0;
    uint32_t    ub_size = 0;

    if (!initrd_bootstrap_image(&ub_data, &ub_size) || ub_size == 0) return 0;

    t = task_registry_find_free();
    if (!t) return 0;

    /* No task_reset_slot(t) here — see task_create. */

    task_init_fpu_state(t);
    t->id         = atomic_fetch_add_explicit(&next_id, 1u, memory_order_relaxed);
    t->state      = TASK_READY;
    t->ring       = TASK_RING3;
    t->priority   = TASK_PRIORITY_DEFAULT;
    /* The root task is the one thread nobody configured, so its ceiling comes
     * from boot — the same place seL4's root task gets its MCP. */
    t->mcp        = (uint8_t)TASK_PRIORITY_MAX;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    /* The root task stays on the boot processor, and not for want of a policy:
     * it is created before `smp_start_aps` has finished counting, and it is the
     * thread the boot sequence hands the CPU to.  Every thread IT configures
     * gets a core from the round robin. */
    t->home_cpu   = 0;

    /*
     * Stage 7-proc: the root task is built from the two objects a thread runs
     * in, and nothing else.
     *
     * It used to allocate a KProcess to hold them — the one process the kernel
     * made rather than a spawner — and then read them back off it.  The thread
     * holds both directly (Steps 4 and 5), so the object in between was a
     * place to put them on the way.
     */
    root_cn = kcnode_alloc(KCNODE_DEFAULT_SLOTS);
    if (!root_cn) goto fail;

    root_cr3 = paging_create_user_space();
    if (root_cr3 == 0) goto fail;

    /* Phase 6.2: create KVSpace before bootstrap maps so bootstrap_kframe_map
     * can register mapping back-refs via kframe_map_page. */
    {
        struct KVSpace *vs = kvspace_alloc(root_cr3);
        if (!vs) goto fail;
        /* The root task's space predates every pool; boot stamps it (A-21). */
        kvspace_tag_bootstrap(vs);
        kobject_retain(&vs->base);
        root_vs = vs;
        kobject_release(&vs->base);
    }

    /* Copy userboot binary to page-aligned PMM pages. The binary symbol is in
     * kernel .rodata at an unaligned offset; paging_map_checked_in requires
     * page-aligned physical addresses, so a fresh aligned copy is mandatory. */
    ub_pages = (uint32_t)((ub_size + 0xFFFU) >> 12);

    /* Guard: verify the total bootstrap page count fits in KProcess.bootstrap_frames[]. */
    if (ub_pages + ustack_pages > KVSPACE_BOOTSTRAP_FRAME_MAX) goto fail;

    ub_copy_phys = pmm_alloc_pages(ub_pages);
    if (ub_copy_phys == 0) goto fail;
    {
        uint8_t       *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(ub_copy_phys);
        const uint8_t *src = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT((uint64_t)(uintptr_t)ub_data);
        for (uint32_t b = 0; b < ub_size; b++) dst[b] = src[b];
        for (uint32_t b = ub_size; b < (uint32_t)(ub_pages << 12); b++) dst[b] = 0;
    }
    /* Phase 6.2: Bootstrap Frame-backed mapping: userboot text (r--x).
     * Each page gets a KFrame (alloc_parent=NULL) mapped via kframe_map_page.
     * The alloc retain is stored in proc->bootstrap_frames[] and released by
     * the bootstrap-frame release inside the address-space reap,
     * after kvspace_invalidate has decremented mapped_count to 0.
     * Physical memory lifetime tracked by t->utext_phys; freed by
     * free_user_text_pages on the teardown paths that precede reap. */
    for (uint32_t pg = 0; pg < ub_pages; pg++) {
        uint64_t va   = USER_TEXT_BASE + (uint64_t)pg * 0x1000ULL;
        uint64_t phys = ub_copy_phys   + (uint64_t)pg * 0x1000ULL;
        struct KFrame *f = bootstrap_kframe_map(root_vs, phys, va, 2ULL /* MAP_EXEC */);
        if (!f) goto fail_copy;
        if (kvspace_register_bootstrap_frame(root_vs, f) != IRIS_OK) {
            kframe_unmap_page(f, root_vs, va);
            kobject_release(&f->base);
            goto fail_copy;
        }
    }
    t->user_entry = USER_TEXT_BASE;

    ustack_phys = pmm_alloc_pages(ustack_pages);
    if (ustack_phys == 0) goto fail;

    /* Phase 6.2: Bootstrap Frame-backed mapping: initial user stack (rw-nx).
     * Same KFrame-backed pattern as the text mapping above.
     * Physical memory tracked by t->ustack_phys. */
    for (uint32_t pg = 0; pg < ustack_pages; pg++) {
        uint64_t va   = USER_STACK_BASE + 4096ULL * USER_STACK_GUARD_PAGES +
                        (uint64_t)pg * 4096ULL;
        uint64_t phys = ustack_phys + (uint64_t)pg * 4096ULL;
        struct KFrame *f = bootstrap_kframe_map(root_vs, phys, va, 1ULL /* MAP_WRITABLE */);
        if (!f) goto fail;
        if (kvspace_register_bootstrap_frame(root_vs, f) != IRIS_OK) {
            kframe_unmap_page(f, root_vs, va);
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

    /* Stage 7 Step 4: the root task's thread holds its own CSpace too.  It is
     * read off the process here because there is no capability to pass — this
     * is the one thread whose CSpace the KERNEL fabricated, before anything
     * existed that could name it. */
    t->cspace_root = root_cn;
    if (t->cspace_root) {
        kobject_retain(&t->cspace_root->base);
        kobject_active_retain(&t->cspace_root->base);
    }
    t->vspace = root_vs;
    if (t->vspace) {
        kobject_retain(&t->vspace->base);
        kobject_active_retain(&t->vspace->base);
    }

    task_set_first_user_entry(t, USER_TEXT_BASE, t->user_rsp, 0);
    task_set_bootstrap_arg0(t, arg0);

    t->utext_phys  = ub_copy_phys;
    t->utext_pages = ub_pages;

    /* Phase S2 D2: the KTCB IS t itself.  ktcb_object_init sets refcount = 1,
     * the scheduler's own execution reference (dropped at termination by
     * task_execution_teardown_off_cpu).
     *
     * Step 4: creating a thread no longer publishes a KTcb HANDLE into the
     * owning process.  Nothing read it — the capability arrives on request
     * through SYS_TCB_SELF — so it was authority handed out by construction,
     * to a holder that never asked, in the namespace being retired. */
    ktcb_object_init(t);

    rq_enqueue(t);
    atomic_fetch_add_explicit(&sched_live_count, 1u, memory_order_relaxed);

    return t;

fail_copy:
    free_phys_pages_range(ub_copy_phys, ub_pages);
fail:
    free_phys_pages_range(ustack_phys, ustack_pages);
    /* Stage 7-proc: reclamation is the ADDRESS SPACE's and the CSPACE's own —
     * releasing the references is the whole of it, and their destructors run
     * when nobody holds them. */
    if (root_vs) kobject_release(&root_vs->base);
    if (root_cn) kobject_release(&root_cn->base);
    if (t) {
        /* Every goto-fail above happens after the registry claim but before
         * ktcb_object_init/rq_enqueue — undo exactly that.  There is no kernel
         * stack to give back: a thread stopped owning one at step 3. */
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

/*
 * task_thread_create — REMOVED (Stage 7).
 *
 * It carved a thread out of the static task pool for a process the caller
 * named, and SYS_THREAD_START was its only caller.  A spawned process's first
 * thread is RETYPED from the child's own budget now and configured with the
 * child's CSpace and VSpace, so no path remains by which a thread exists
 * because the kernel had a free slot.
 *
 * What still comes from the pool is the ROOT TASK and the idle task, both
 * built before any Untyped exists.
 */

/* ── Stage 5 Step 4: execution for a TCB born from an Untyped ──────────────
 *
 * RETYPE2(KOBJ_TCB) has produced cap-complete but INACTIVE threads since
 * Phase S2: a full capability citizen with no registry slot, no kernel stack
 * and no address space, refused by every execution syscall.  What was missing
 * was the operation that gives it those — and it was missing because its
 * arguments are capabilities (a CSpace root and a VSpace) that only became
 * addressable as capabilities in Stages 3-5.
 *
 * ktcb_configure builds exactly the execution state task_thread_create builds
 * for a pool-born thread, in the same order, minus the storage: the caller
 * already owns storage inside its Untyped.  The thread is left SUSPENDED —
 * configuring a thread does not start it, and TCB_WRITE_REGS still has to say
 * where it starts.
 */
/*
 * Stage 7-proc: a thread is configured with a CSpace and a VSpace, and that is
 * all there is to it.
 *
 * It used to take a KProcess as well and JOIN it — a count to increment and a
 * teardown flag to lose a race against.  A "process" is what you get when
 * several threads are configured with the same CSpace and the same VSpace, so
 * there is nothing to join and nothing to agree with: the pair IS the
 * membership.  This is seL4's seL4_TCB_Configure, which binds a CNode and a
 * VSpace to a thread and knows about no third object.
 */
iris_error_t ktcb_configure(struct task *t,
                            struct KCNode *cspace, struct KVSpace *vspace) {
    if (!t || !vspace || !vspace->cr3) return IRIS_ERR_INVALID_ARG;
    if (t->configured || t->terminal) return IRIS_ERR_ALREADY_EXISTS;
    /*
     * A-21: an address space with no identifier cannot be run in.  A holder
     * who retyped a VSpace has built one; making it runnable is a second grant
     * (an ASIDPool), and this is where the two meet.  seL4 answers the same
     * question at the same place.
     */
    if (!kvspace_has_asid(vspace)) return IRIS_ERR_ACCESS_DENIED;

    if (task_registry_alloc(t) != 0) return IRIS_ERR_NO_MEMORY;

    /*
     * The thread inherits the CEILING of whoever configured it (ledger A-20).
     *
     * This is what makes a priority bound travel with delegation instead of
     * being a number the kernel hands out: a supervisor given 100 configures
     * threads that can grant at most 100, and so on downward, for ever.  seL4
     * spells it `seL4_TCB_SetMCPriority` and requires an authority for that
     * too; inheriting at configure time is the same rule with the common case
     * built in.
     */
    {
        struct task *by = task_current();
        t->mcp = by ? by->mcp : (uint8_t)TASK_PRIORITY_MAX;
    }

    /*
     * Stage 7 Step 4: the thread takes the CSpace it was configured with.
     *
     * `cspace` is the capability the caller NAMED, passed down rather than
     * re-read from the process — sys_tcb_configure has already proved the two
     * are the same object, so reading it back off KProcess would produce an
     * identical pointer by a route that contradicts the claim this step makes.
     * The pair is lifecycle + active, the same one KProcess holds and for the
     * same reason: the lifecycle ref keeps the object, the active ref keeps
     * its SLOTS reachable, and dropping the active ref to zero is what empties
     * a CSpace.  Released in task_execution_teardown_off_cpu.
     */
    t->cspace_root = cspace;
    if (t->cspace_root) {
        kobject_retain(&t->cspace_root->base);
        kobject_active_retain(&t->cspace_root->base);
    }
    /*
     * Stage 7-proc: lifecycle AND active, the same pair as the CSpace above.
     *
     * The lifecycle ref keeps the object; the ACTIVE ref is what says the
     * address space is still in use, and dropping the last one is what
     * invalidates it (kvspace_obj_close).  A thread that held only the
     * lifecycle ref left the address space's usability in somebody else's
     * hands: the spawner deletes its own capability at the end of a spawn, and
     * that would invalidate the space the child is about to run in.
     */
    t->vspace = vspace;
    if (t->vspace) {
        kobject_retain(&t->vspace->base);
        kobject_active_retain(&t->vspace->base);
    }

    task_init_fpu_state(t);
    t->id         = atomic_fetch_add_explicit(&next_id, 1u, memory_order_relaxed);
    t->state      = TASK_SUSPENDED;   /* configured is not started */
    t->ring       = TASK_RING3;
    t->time_slice = TASK_DEFAULT_SLICE;
    t->ticks_left = TASK_DEFAULT_SLICE;
    t->home_cpu   = sched_pick_home_cpu();

    /* The list a thread must join is `sched_thread_list` — what the tick and
     * the idle fast-forward walk — and `task_registry_alloc` below is what
     * puts it there, under the lock.  There used to be a second, circular list
     * spliced in by hand here; it had no readers. */

    /* The scheduler's live-count is a creation-time fact, not a run-queue one:
     * teardown decrements it unconditionally, so a thread that skipped the
     * increment would underflow the counter the moment it exited. */
    atomic_fetch_add_explicit(&sched_live_count, 1u, memory_order_relaxed);

    /*
     * The EXECUTION reference — the one task_execution_teardown_off_cpu drops
     * last, and the reason a running thread outlives every capability to it.
     *
     * A pool-born thread gets it from ktcb_object_init, whose refcount=1 IS
     * the scheduler's.  A RETYPED thread's refcount=1 belongs to the CSpace
     * slot the retype published it into, so without this the slot is the only
     * owner: a holder that deletes its TCB capability after starting the
     * thread destroys the thread's storage underneath it, and the block is
     * zeroed while it is the thing running.  That was invisible while the only
     * caller kept its slot forever (init's S8 selftest); a spawner does not,
     * because the child's TCB is the spawner's to drop once the child runs.
     */
    kobject_retain(&t->base);

    t->configured = 1;
    return IRIS_OK;
}

/*
 * Set where a configured thread starts.  Same frame task_thread_create builds:
 * an iretq frame on the kernel stack under the ring-3 trampoline, so the first
 * context switch into this thread returns to user mode at `entry` with `sp`
 * and `arg` in the ABI's places.
 *
 * Refused once the thread has run: rewriting the entry frame of a live thread
 * would corrupt the kernel stack it is standing on.  Registers of a RUNNING
 * thread are a suspend-and-write operation, which IRIS does not have yet.
 */
iris_error_t ktcb_write_regs(struct task *t, uint64_t entry, uint64_t sp,
                             uint64_t arg) {
    if (!t) return IRIS_ERR_INVALID_ARG;
    if (!t->configured || t->terminal) return IRIS_ERR_NOT_SUPPORTED;
    if (t->state != TASK_SUSPENDED || t->started) return IRIS_ERR_BUSY;

    /*
     * Stage 7: the range checks live here now.
     *
     * They were SYS_THREAD_START's, and SYS_THREAD_START was the only way a
     * spawned process got its first thread.  With that retired this is the
     * only path, so the checks move with it rather than being lost — an entry
     * point outside the private user window, or a misaligned stack, is a
     * thread that faults on its first instruction and tells nobody why.
     */
    if (entry < USER_PRIVATE_BASE || entry >= USER_VMO_BASE)
        return IRIS_ERR_INVALID_ARG;
    if (sp < USER_PRIVATE_BASE || sp > USER_SPACE_TOP)
        return IRIS_ERR_INVALID_ARG;
    if (sp & 0x7ULL)
        return IRIS_ERR_INVALID_ARG;   /* the ABI's stack alignment */

    t->user_entry = entry;
    t->user_rsp   = sp;

    task_set_first_user_entry(t, entry, sp, arg);
    return IRIS_OK;
}

/* ── Task termination ────────────────────────────────────────────────────── */

struct task *task_current(void) {
    return cpu_self()->current_task;
}


/* Must not be called on a thread any processor is running: this function frees
 * resources that a live stack may still reference.  The one caller that could
 * pass its own (SYS_TCB_EXIT) checks for self and takes the exit path instead;
 * the check below covers the other three cores. */
void task_kill_external(struct task *t) {
    if (!t || t->state == TASK_DEAD || t->terminal) return;

    /*
     * A thread another processor is executing cannot be torn down from here —
     * this frees the address space it is running in — but it must not simply
     * be left alone either, which is what returning did (SMP roadmap §9.3
     * step 4).  `Suspend` on such a thread was a lie; a `kill` that returned
     * success and killed nothing was a worse one.
     *
     * So it is marked the way a thread marks ITSELF on the way out, and its
     * processor is told to look.  That core's next dispatch finds a DEAD
     * outgoing thread, hands it to the reap ring exactly as a self-exit is
     * handed over, and the teardown happens on a core that is no longer inside
     * it.  Nothing here is special-cased: the machinery is the one self-exit
     * already uses, and the only difference is who set the state.
     */
    if (task_is_on_some_cpu(t)) {
        t->awaiting_reap = 1;
        t->state         = TASK_DEAD;
        sched_kick(t);
        /*
         * And hand it over from HERE as well, rather than trusting that its
         * own core will.
         *
         * The test above and the mark below it are two instants, and a thread
         * can leave its processor in between.  Then that core's final dispatch
         * read a state that was not yet DEAD and handed nothing over, while
         * this core decided somebody else would — and the death is lost: a
         * thread marked DEAD, in no run queue, in no reap ring, that nothing
         * will ever tear down.  It is the reason T287 could kill a thread
         * through its capability and then watch it never reach TERMINATED.
         *
         * Enqueuing here closes it without having to know which happened.  If
         * the thread is still on a processor, the reaper finds it there and
         * puts it back; if it is not, this IS the hand-over.  The enqueue is
         * idempotent, so the core that owned it may safely do the same.
         */
        reap_enqueue_dead(t);
        return;
    }
    task_execution_teardown_off_cpu(t);
}

/*
 * Phase S2 D2: task_exit_current runs ON the dying task's own kernel stack, so
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
    /*
     * Stage 9-evt step 3: leave through the dispatcher rather than looping on
     * a yield.  The loop existed because a yield could decline and come back,
     * and a dead thread had to keep asking; the dispatcher cannot come back —
     * it resets the core stack under itself — so asking once is asking.
     *
     * The thread's own stack is finished with at this instruction, which is
     * what lets the reaper free it: `sched_pick_for_dispatch` sees TASK_DEAD
     * and enqueues it, and the reaper runs on the NEXT dispatch, by which time
     * nothing is standing on the stack it is about to return.
     */
    core_dispatch_enter(t);
}

/*
 * task_kill_process — DELETED (Stage 7 Step 13).
 *
 * It was the body of SYS_PROCESS_KILL: a sweep of the whole task registry for
 * threads whose process pointer matched.  Nothing else called it, and nothing
 * could — the only way to name "every thread of that process" without holding
 * any of them is to scan the kernel's registry, which is the shape Stage 7
 * spent its whole length removing.  A supervisor stops the threads it holds.
 */
