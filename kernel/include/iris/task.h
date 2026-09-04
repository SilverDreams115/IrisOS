#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>
#include <iris/ipc_msg.h>
#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/spinlock.h>

struct KEndpoint;
struct KSchedContext;
struct KReply;
struct KCNode;
struct KVSpace;
struct KNotification;

#define TASK_MAX              256
#define TASK_STACK_SIZE       8192  /* kernel stack per task */
#define TASK_DEFAULT_SLICE    2     /* ticks per quantum at 100 Hz = 20ms */
#define TASK_PRIORITY_DEFAULT 128u  /* default scheduling priority */
#define TASK_PRIORITY_MAX     255u  /* highest scheduling priority */
#define TASK_PRIORITY_MIN     0u    /* lowest (idle task) */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED_IPC,       /* blocked waiting for an endpoint IPC rendezvous */
    TASK_BLOCKED_IRQ,       /* blocked waiting for a KNotification signal */
    TASK_SLEEPING,          /* blocked until a timer tick count is reached */
    TASK_BLOCKED_FAULT,     /* suspended pending exception handler decision */
    TASK_BLOCKED_SEND,      /* blocked waiting for a receiver on a KEndpoint */
    TASK_BLOCKED_RECV,      /* blocked waiting for a sender on a KEndpoint */
    TASK_BUDGET_EXHAUSTED,  /* Ph75: SC budget spent; sleeping until refill tick */
    TASK_BLOCKED_REPLY,     /* Ph85: EP_CALL caller blocked waiting for KReply invocation */
    TASK_SUSPENDED,         /* Ph96: explicitly suspended via SYS_TCB_SUSPEND */
    /*
     * Phase S2 D2 — execution ended but the KTCB OBJECT may still be alive
     * (referenced by surviving capabilities).  TERMINATED != destroyed:
     *   - not in any run/wait/reap queue;
     *   - not holding a registry slot (scheduler capacity released);
     *   - execution resources (kstack/addrspace) freed;
     *   - a surviving cap can still SYS_TCB_GET_INFO it (reports TERMINATED);
     *   - the backing storage is freed only by the final destructor.
     * TASK_DEAD remains the "free backing slot" marker (pre-creation state).
     */
    TASK_TERMINATED,
    TASK_DEAD,
} task_state_t;

/* Helper: true when task is runnable (may be scheduled) */
static inline int task_is_runnable(task_state_t s) {
    return s == TASK_READY || s == TASK_RUNNING;
}

typedef enum {
    TASK_RING0 = 0,   /* kernel task */
    TASK_RING3 = 3,   /* user task */
} task_ring_t;

/* saved kernel-mode registers (callee-saved + rip + rflags) */
struct cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rip;
    uint64_t rflags;   /* IF state per-task — prevents IRQ preemption contamination */
} __attribute__((packed));

/*
 * Phase S2 D2 — the canonical TCB.
 *
 * `struct task` IS the KTCB: it carries the KObject header at offset 0 and all
 * execution state directly.  The old `struct KTcb { KObject; struct task* }`
 * wrapper is REMOVED — there is one structure, one object identity.
 *
 * Four separate lifetimes (see docs/architecture/sel4-task-model.md):
 *   1. Object    — kobject refcount (creation ref + capability refs); ends at
 *                  the final destructor.
 *   2. Execution — configure/resume..suspend/terminate; the scheduler holds
 *                  ONE "execution ref" dropped at termination.
 *   3. Registry  — a scheduler-identity slot held only while runnable/alive;
 *                  released at termination (NOT at last cap).
 *   4. Storage   — the backing slot; freed by the destructor, reusable only
 *                  after the last reference (D2 transitional static backing;
 *                  D/E moves it to Untyped).
 */
struct task {
    struct KObject    base;        /* MUST be offset 0 — KOBJ_TCB object header */
    irq_spinlock_t    obj_lock;    /* guards object-identity fields */
    uint32_t          object_generation; /* +1 on final destroy (stale-cap defense) */
    uint32_t          id;
    task_state_t      state;
    task_ring_t       ring;
    uint8_t           priority;  /* Ph73: 0=lowest, 255=highest; idle=0, user=128 */
    uint8_t           awaiting_reap; /* A1.11: TASK_DEAD but still queued for the
                                      * deferred reaper — the slot is NOT free yet.
                                      * Set by task_exit_current, cleared by the
                                      * reaper's task_reset_slot (which zeroes the
                                      * struct).  task_create* must skip it. */

    struct cpu_context ctx;

    /* kernel stack — allocated from the kstack virtual region in paging.h.
     * kstack points to the lowest byte of the usable stack (the guard page
     * sits one page below this address and is intentionally not mapped).
     * kstack_phys is the physical base used to free the PMM pages on teardown.
     * Both fields are 0 for an uninitialized/dead task slot. */
    uint8_t          *kstack;
    uint64_t          kstack_phys;
    /* Phase S2 (scheduler indirection): saved kernel RSP across a context
     * switch.  Replaces the parallel index-keyed task_rsp[TASK_MAX] array — the
     * scheduler no longer derives a slot index by pointer arithmetic
     * (old - tasks) to find where to save/restore the kernel stack pointer;
     * it lives inside the TCB backing itself.  This is the first structural
     * decoupling of scheduler identity from the static-array index. */
    uint64_t          saved_krsp;
    /* Phase S2 Inc.2B (Bloque A): intrusive run-queue links.  The per-CPU run
     * queue no longer uses index-keyed parallel arrays (next[TASK_MAX] /
     * queued[TASK_MAX]) nor (t - tasks) pointer arithmetic; each TCB carries
     * its own FIFO link + queued flag, so scheduling identity is by pointer,
     * not by position in a static array. */
    struct task      *rq_next;      /* next in same-priority FIFO, NULL = tail */
    uint8_t           rq_queued;    /* 1 while enqueued in a run queue */

    /* user entry point and virtual stack info (ring 3 only) */
    uint64_t          user_entry;
    uint64_t          user_rsp;        /* current user stack pointer (for iretq) */
    uint64_t          user_stack_base; /* virtual base of user stack region */
    uint64_t          user_stack_top;  /* virtual top of user stack region */
    uint32_t          user_stack_pages; /* number of pages allocated */
    uint64_t          ustack_phys;     /* physical base of user stack */
    uint64_t          utext_phys;      /* physical base of userboot text copy (ring-3 only) */
    uint32_t          utext_pages;     /* page count at utext_phys; 0 if not applicable */
    /*
     * `process` DELETED (Stage 7-proc).
     *
     * A thread's process was the object it belonged to, and everything that
     * object was for has moved to the thread: its CSpace root and its address
     * space (Steps 4 and 5), its fault record and handler (Steps 6 and 12),
     * its death and exit code (Step 10), and its budget, which the caller of
     * an allocating syscall now names (Step 14).  What a "process" is, is
     * threads configured with the same CSpace and the same VSpace — which is
     * a fact about those two capabilities, not a third object to point at.
     */
    /*
     * Stage 7 Step 4 — the CSpace this thread resolves CPtrs in, held by the
     * THREAD.
     *
     * SYS_TCB_CONFIGURE has named the CSpace as a capability since Stage 5
     * Step 4, and then the kernel resolved every CPtr through
     * `t->process->cspace_root` anyway — so the argument described the truth
     * without being it, and a thread's most basic authority was a property of
     * a shared object it did not name.  It is the thread's now: one lifecycle
     * ref plus one active ref, exactly the pair KProcess holds, released at
     * execution teardown.
     *
     * Threads of one process still share one CNode object, so nothing about
     * what a CPtr resolves to changes.  What changes is that resolving no
     * longer reads KProcess, which is most of what KProcess was on the hot
     * path.
     */
    struct KCNode    *cspace_root;
    /*
     * Stage 7 Step 5 — the address space this thread runs in, held by the
     * THREAD, for the same reason as cspace_root above: SYS_TCB_CONFIGURE
     * names it as a capability and the scheduler then loaded CR3 out of
     * `t->process`, so what a thread ran in was a property of a shared object
     * rather than of the thread.  One lifecycle reference (a VSpace has no
     * slots, so there is no active half), released with the thread's
     * execution.
     */
    struct KVSpace   *vspace;
    /*
     * Stage 7 Step 10 — this thread's death, observed by whoever holds it.
     *
     * A supervisor used to watch a PROCESS: the kernel signalled when its last
     * thread went, and the exit code lived on the process.  That made "my
     * service died" a question you asked an object you had to be given
     * authority over, rather than one you asked the execution you started and
     * still hold.  A supervisor HAS the thread — it retyped the TCB and
     * configured it — and every service in the tree is single-threaded, so
     * watching the thread is not an approximation of watching the process, it
     * is the same event named by the thing that produces it.
     *
     * One watcher, not four.  KProcess carried an array because several
     * unrelated holders could watch the same process; a thread is watched by
     * whoever holds its TCB, and a second watcher is a second capability
     * rather than a second slot.
     */
    /*
     * Stage 7 Step 12 — the fault HANDLER is the thread's too.
     *
     * Registration named a PROCESS, so the kernel kept the handler, the
     * mailbox and the generation counter on KProcess and pointed at "whoever
     * faulted last" to answer a read.  Every one of those is a property of an
     * execution: which handler to tell, where to put the thread, and which
     * generation this fault is.  On the thread they need no last-faulter
     * pointer, because the thread IS the record — and a supervisor that arms a
     * thread's faults already holds that thread.
     */
    struct KNotification *fault_notif;
    uint64_t          fault_bits;
    struct KCNode    *fault_cspace;   /* mailbox CNode, retained */
    uint32_t          fault_slot;
    /*
     * Stage 8-cap / D-6 — where the delivered capability's AUTHORITY came
     * from, so the copy the kernel publishes has an ancestor.
     *
     * A fault used to publish the faulting thread into a mailbox as an MDB
     * LEGACY_ROOT: a capability with no parent, which SYS_CSPACE_REVOKE can
     * never reach because revoke walks descendants.  Revoking the supervisor's
     * thread capability therefore did not reach the copies the kernel had
     * handed to handlers.
     *
     * The ancestor is the slot the REGISTRANT named when it armed the handler
     * — the same rule Stage 2 set for IPC, where a delivered capability is a
     * child of the sender's SOURCE slot.  Recorded here with the CNode
     * retained, and verified by IDENTITY at delivery (kcnode_slot_holds): a
     * slot is a reusable location, and between arming and faulting it can be
     * refilled with something that never authorised anything.  A registration
     * whose source slot no longer holds this thread is a delegation made with
     * a capability that no longer exists, and is treated as no handler at all.
     */
    struct KCNode    *fault_src_cn;   /* registrant's slot for THIS tcb */
    uint32_t          fault_src_idx;

    /*
     * Stage 8-mcs — the TIMEOUT fault handler, a SEPARATE registration.
     *
     * seL4 keeps seL4_TCB_SetTimeoutEndpoint apart from the fault endpoint,
     * and the reason is authority rather than tidiness: the principal that
     * answers "this thread ran out of time" is a temporal supervisor, and the
     * principal that answers "this thread touched an unmapped page" is a
     * pager.  Folding the two would hand the pager temporal authority over
     * every thread it serves, and hand the scheduler the ability to resume a
     * thread out of a page fault.  They are different questions asked by
     * different servers, so they are different registrations.
     *
     * Same shape as the exception registration above — notification, bits,
     * mailbox, and the source slot the delivered capability is parented to —
     * because it IS the same mechanism: a fault delivered by naming the
     * thread's capability.  Both are written by one shared registration path
     * (tcb_register_handler), so the two cannot drift.
     *
     * Unregistered (notif == NULL) is the default and means what it meant
     * before timeout faults existed: budget exhaustion blocks the thread until
     * its period refills it, and nobody is told.
     */
    struct KNotification *timeout_notif;
    uint64_t          timeout_bits;
    struct KCNode    *timeout_cspace;
    uint32_t          timeout_slot;
    struct KCNode    *timeout_src_cn;
    uint32_t          timeout_src_idx;
    /*
     * Set by the timer tick when the budget runs out and a timeout handler is
     * armed; consumed in task context by task_yield.  The tick MUST NOT
     * deliver: delivery signals a notification, which wakes a task and touches
     * the run queue, and the tick runs in the PIT ISR.  Same discipline the
     * timed-block path already follows two functions above — the ISR records
     * the fact, task context acts on it.
     */
    uint8_t           timeout_pending;

    uint32_t          fault_seq_counter;
    struct KNotification *exit_notif;
    uint64_t          exit_bits;
    uint32_t          exit_code;
    uint8_t           exit_reported;   /* 1 once the watch has fired */
    /*
     * Stage 7 Step 6 — the fault record belongs to the thread that took it.
     *
     * It lived on KProcess, one copy per process, and the code said what that
     * cost: "the per-process record is last-writer-wins".  Two threads of one
     * process faulting before the handler runs left one of them describing the
     * other, and a handler reading SYS_PROCESS_FAULT_INFO got a vector, a rip
     * and a CR2 that might belong to a thread it was not looking at.  The
     * generation counter was added to make the RESUME safe against that; it
     * could not make the READ safe, because there was only one record.
     *
     * A fault is a property of an execution, so it is stored on the execution.
     * The process keeps the HANDLER (whom to tell) and the generation counter
     * (a per-process sequence is what the handler sees), and a reference to
     * the thread that faulted last, so the process-scoped read still answers.
     */
    uint32_t          fault_vector;
    uint64_t          fault_rip;
    uint32_t          fault_error;
    uint64_t          fault_cr2;
    uint8_t           fault_valid;    /* 1 = this thread has a pending fault */
    uint32_t          fault_seq;       /* Phase 25: generation of the fault this
                                        * task is blocked on (TASK_BLOCKED_FAULT);
                                        * 0 = no fault ever delivered to it */

    /* cooperative scheduler quantum */
    uint32_t          time_slice;   /* ticks per quantum (default TASK_DEFAULT_SLICE) */
    uint32_t          ticks_left;   /* ticks remaining before need_resched */
    uint32_t          need_resched; /* set by scheduler_tick when ticks_left hits 0 */
    uint32_t          timed_out;   /* set by scheduler_tick when a timed block expires */
    uint64_t          wake_tick;   /* deadline tick: valid for TASK_SLEEPING and timed BLOCKED_IPC/IRQ */

    /* Synchronous endpoint IPC staging (Ph66+). */
    struct IrisMsg      ipc_msg;         /* 64-byte staging/delivery buffer */
    uint32_t            ipc_msg_ready;   /* set by sender on successful rendezvous */
    uint32_t            ipc_ep_closed;   /* set by kendpoint_close while task was blocked */
    struct task        *ep_next;         /* intrusive link for endpoint queue */
    struct KEndpoint   *blocking_ep;     /* endpoint where task is blocked, or NULL */
    /* Ph68: capability staged for transfer during a blocking send */
    struct KObject     *ep_cap_obj;      /* kobject being transferred; NULL = none */
    uint32_t            ep_cap_rights;   /* rights to grant on ep_cap_obj */
    uint64_t            ep_cap_badge;    /* Phase 9: badge carried by the staged cap */
    /* Phase S4 (Step 2): source SLOT backing ep_cap_obj (two-phase staging).
     * The transfer source is a CSpace slot, not a handle — it is the MDB
     * identity the delivered cap is parented to.  The sender's slot stays
     * occupied while queued; the receiver commits (deletes it) only when it
     * takes the staged cap for delivery.  Cancel / endpoint-close paths
     * release the CNode ref without deleting the slot, so the sender keeps
     * its cap when nothing was delivered.  ep_cap_src_cn == NULL = none;
     * it carries active+lifecycle refs while set. */
    struct KCNode      *ep_cap_src_cn;
    uint32_t            ep_cap_src_idx;
    /* Ph69: IPC buffer staging */
    uint32_t            ipc_kbuf_len;    /* valid bytes in ipc_kbuf */
    uint64_t            ep_recv_buf_uptr;/* receiver's output buffer user addr (set at EP_RECV) */
    /* A1.5: receiver-declared receive-slot (direct root-CNode CPtr, 1..1023;
     * 0 = none/legacy).  Written by EVERY recv-family syscall entry
     * (EP_RECV / EP_NB_RECV / EP_CALL) and consumed by at most one routed
     * cap delivery, so it can never leak across operations. */
    uint32_t            ep_recv_slot;
    uint8_t             ipc_kbuf[IRIS_IPC_BUF_SIZE]; /* kernel-side bulk payload staging */
    /* Ph74: optional scheduling context — retained KSchedContext ref (NULL = best-effort) */
    struct KSchedContext *sched_ctx;
    /* Ph85: reply capability fields */
    uint32_t       ep_call_mode;    /* 1 if task entered EP via SYS_EP_CALL (wants reply) */
    struct KReply *pending_kreply;  /* non-NULL while state == TASK_BLOCKED_REPLY (task holds a ref) */
    /* Phase S1: explicit MCS-style reply object staged by the receiver.
     * Set at EP_RECV / EP_NB_RECV entry from the reply CPtr in arg2 (the
     * task holds a lifecycle ref + the object's staged claim); consumed at
     * an EP_CALL rendezvous (bound to the caller) or released when the recv
     * concludes without a call / the receiver dies.  NULL = none. */
    struct KReply *ep_reply_obj;
    uint32_t       ep_reply_val;    /* raw CPtr/handle value the receiver passed —
                                     * echoed to the server in msg.attached_handle */
    /* Phase S2 D2: `struct task` IS the KTCB — no separate wrapper.  A cap to
     * this thread is a KOBJ_TCB cap on &base.  `configured`/`terminal` flag
     * the object/execution state; `reg_slot` is the registry witness. */
    uint8_t        configured;   /* Phase S2: TCB_CONFIGURE committed */
    uint8_t        started;      /* Stage 5: has been made runnable at least
                                  * once — after that its kernel stack holds
                                  * live state and the entry frame must not be
                                  * rewritten (TCB_WRITE_REGS refuses) */
    uint8_t        terminal;     /* execution ended (TERMINATED) */
    int32_t        reg_slot;     /* registry index while registered, else -1 */
    /* Stage 5 Step 4: which kernel-stack slot of the KSTACK_VIRT_BASE region
     * this thread owns, or -1.  It used to be implied — the backing-array
     * index of the pool slot the task was carved from — which only exists for
     * a task that lives IN the pool.  A TCB retyped from an Untyped has no
     * such index, so ownership is recorded instead of derived. */
    int32_t        kstack_slot;

    /* SMP: CPU this task is homed to (its run queue owner).
     * Set at creation time; stays constant for the task's lifetime. */
    uint8_t           home_cpu;

    /* FPU/SSE state — 512-byte FXSAVE image, must be 16-byte aligned.
     * Saved and restored on every context switch so FPU state never leaks
     * across task boundaries. Placed last to keep alignment padding minimal. */
    uint8_t           fpu_state[512] __attribute__((aligned(16)));

    struct task      *next;
};

/* Phase S2 D2: canonical KTCB name for the unified structure. */
typedef struct task KTCB;

/* KObject header must be at offset 0 so a KOBJ_TCB cap (KObject*) aliases the
 * KTCB, and cpu_context keeps its assembly-visible packed layout. */
_Static_assert(__builtin_offsetof(struct task, base) == 0u,
               "KTCB object header must be at offset 0");
_Static_assert(sizeof(struct cpu_context) == 64u,
               "cpu_context layout is ABI (context_switch.S offsets 0..56)");

void         task_init(void);
struct task *task_create(void (*entry)(void));
struct task *task_spawn_user(uint64_t arg0);
/* Stage 5 Step 4 — execution for a TCB retyped from an Untyped.
 *
 * ktcb_configure gives an inactive (RETYPE2-born) TCB the execution state a
 * pool-born thread gets at creation: a registry slot, a kernel stack, FPU
 * state and an address space.  It leaves the thread SUSPENDED — configuring
 * is not starting — and refuses a TCB that is already configured or dead.
 *
 * ktcb_write_regs sets where it starts, and refuses once it has been runnable:
 * rewriting the entry frame of a thread that has run would corrupt the kernel
 * stack it is standing on.
 */
iris_error_t ktcb_configure(struct task *t,
                            struct KCNode *cspace, struct KVSpace *vspace);
iris_error_t ktcb_write_regs(struct task *t, uint64_t entry, uint64_t sp,
                             uint64_t arg);

void         task_set_bootstrap_arg0(struct task *t, uint64_t arg0);
void         task_abort_spawned_user(struct task *t);
void         task_exit_current(void);
void         task_yield(void);
struct task *task_current(void);

/*
 * task_kill_external — forcibly terminate a task that is NOT the current task.
 *
 * Safe to call only when the target is blocked or ready (not running).
 * Because the caller is executing on a different CR3, address-space reap
 * is performed immediately rather than via the pending_reap deferred path.
 *
 * Idempotent: if t is already TASK_DEAD this is a no-op.
 * Must NOT be called with t == task_current(); use task_exit_current() instead.
 */
void task_kill_external(struct task *t);

/*
 * task_wakeup — transition a blocked/sleeping task to READY and enqueue it
 * in the O(1) priority run queue.  No-op for TASK_DEAD or TASK_RUNNING.
 * Must be called with IRQs disabled or from a single-CPU context.
 */
void task_wakeup(struct task *t);

/*
 * task_suspend — move a task to TASK_SUSPENDED and remove it from the run
 * queue.  No-op for TASK_DEAD.  Caller must yield afterwards if t is the
 * current task, so that a different task is scheduled.
 */
void task_suspend(struct task *t);

#endif
