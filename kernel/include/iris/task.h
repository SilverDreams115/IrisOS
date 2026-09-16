#ifndef IRIS_TASK_H
#define IRIS_TASK_H

#include <stdint.h>
#include <stdatomic.h>
#include <iris/ipc_msg.h>
#include <iris/ipc_stage.h>
#include <iris/user_ctx.h>
#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/spinlock.h>

struct KEndpoint;
struct KSchedContext;
struct KReply;
struct KCNode;
struct KVSpace;
struct KNotification;
struct KFrame;

/*
 * There is no ceiling on live threads (ledger A-19).
 *
 * TASK_MAX was 256 and it bounded two things: a static backing pool, and the
 * scheduler's identity registry — which returned NO_MEMORY when full, so the
 * kernel decided how many threads a system may have.  seL4 decides no such
 * thing: a TCB exists because somebody retyped one out of memory they hold.
 *
 * What is left is the BOOTSTRAP pair, and it is static for the reason seL4's
 * root task is: the idle thread and the root task are built by boot code, out
 * of memory no Untyped exists for yet.  Every other thread is retyped.
 *
 * `TASK_MAX` and `TASK_STACK_SIZE` are gone with it.  They were kept for two
 * consumers that have since gone themselves: the kernel-stack window in
 * paging.h, which D-1 step 3 deleted along with per-thread kernel stacks, and
 * a `tasks_max` diagnostic field that reported the ceiling as 0.  A number
 * kept for readers that no longer read it is a ceiling waiting to be
 * reintroduced by someone who finds it and assumes it means something.
 */
#define TASK_BOOTSTRAP_MAX      2

#define TASK_DEFAULT_SLICE    2     /* ticks per quantum at 100 Hz = 20ms */
#define TASK_PRIORITY_DEFAULT 128u  /* default scheduling priority */
#define TASK_PRIORITY_MAX     255u  /* highest scheduling priority */
#define TASK_PRIORITY_MIN     0u    /* lowest (idle task) */

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED_IPC,       /* blocked waiting for an endpoint IPC rendezvous */
    TASK_BLOCKED_IRQ,       /* blocked waiting for a KNotification signal */
    /*
     * Ledger A-24: TASK_SLEEPING is RETIRED and its number is kept so no other
     * state silently inherits it.  A thread blocked on time was the kernel
     * holding a deadline on somebody's behalf; waiting is a service now, and a
     * thread waiting for one is blocked on a NOTIFICATION like any other.
     */
    TASK_SLEEPING_RETIRED,
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
    /*
     * The MAXIMUM CONTROLLED PRIORITY — the ceiling this thread may grant.
     *
     * seL4's rule (ledger A-20): `seL4_TCB_SetPriority(tcb, authority, prio)`
     * refuses a priority above the AUTHORITY thread's MCP, so priority is
     * delegated downward and never invented.  Without it, a holder of any TCB
     * capability can set 255 and starve the system, which is what IRIS did.
     *
     * A thread inherits the MCP of whoever CONFIGURED it, which is what makes
     * the ceiling travel with delegation: a supervisor given 100 can create
     * threads that can grant at most 100, for ever downward.  The root task
     * starts at TASK_PRIORITY_MAX because somebody has to.
     */
    uint8_t           mcp;
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
     * Registration named a PROCESS, so the kernel kept the handler and the
     * generation counter on KProcess and pointed at "whoever faulted last" to
     * answer a read.  Both are properties of an execution — and a supervisor
     * that arms a thread's faults already holds that thread.
     */
    /*
     * Ledger A-22 — a fault is an IPC MESSAGE on an ENDPOINT.
     *
     * It used to be three mechanisms where seL4 reuses one: a notification was
     * signalled, the faulting thread's capability was published into a mailbox
     * CNode the registrant declared, the handler read the record with
     * SYS_TCB_FAULT_INFO and answered with SYS_EXCEPTION_RESUME carrying a
     * generation number.  Equivalent in what it could express, and a different
     * structure — which meant a fault handler was not a server, "may resume
     * this thread" was not a capability, and the one-shot-ness of an answer had
     * to be rebuilt out of a sequence counter.
     *
     * Now the faulting thread performs a CALL on this endpoint.  The handler
     * receives it like any other request and gets a REPLY capability; replying
     * resumes the thread.  The authority to resume is that capability and
     * nothing else, so it cannot be replayed, forged or guessed, and a handler
     * that drops it can never accidentally answer a later fault.
     *
     * `fault_ep_badge` is the badge on the capability the REGISTRANT used, and
     * it travels in every fault message as `sender_badge` — which is how the
     * handler knows which of its clients faulted.  seL4 identifies the faulter
     * exactly this way, and it is why the delivery no longer has to mint a
     * thread capability into somebody's CSpace on every fault.
     */
    struct KEndpoint *fault_ep;
    uint64_t          fault_ep_badge;

    /*
     * Stage 8-mcs — the TIMEOUT fault endpoint, a SEPARATE registration.
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
     * Unregistered (ep == NULL) is the default and means what it meant before
     * timeout faults existed: budget exhaustion blocks the thread until its
     * period refills it, and nobody is told.
     */
    struct KEndpoint *timeout_ep;
    uint64_t          timeout_ep_badge;

    /*
     * Ledger A-23 — the BOUND notification (seL4's seL4_TCB_BindNotification).
     *
     * A thread blocked receiving on an endpoint is, without this, deaf to
     * signals: it is in the endpoint's queue and nothing else can reach it.
     * That forced every server that needs both — an interrupt and a request
     * queue, which is what a driver IS — to spend a second thread on the
     * choice.  A signal to this notification wakes the thread out of the
     * endpoint queue and is delivered as a message it can tell apart.
     */
    struct KNotification *bound_notif;
    uint8_t           timeout_pending;

    /*
     * Stage 9-evt Step 1 — RESTARTABLE SYSCALLS (ledger D-1).
     *
     * seL4 is an event kernel: no thread blocks inside the kernel.  A syscall
     * that cannot complete records what it needs in the THREAD, returns, and
     * is RE-EXECUTED when the thread runs again.  IRIS parks the thread
     * mid-syscall on an 8 KiB kernel stack instead, which is the reason it can
     * bound neither in-kernel latency nor kernel memory per thread.
     *
     * Converting that is three steps, and the hard one is first:
     *
     *   1. make blocking handlers RESTART-SAFE — hold no live state across the
     *      block, express the continuation in thread state, and prove it by
     *      actually re-executing them.  This flag and these saved arguments
     *      are that step.
     *   2. abandon the syscall frame instead of parking it (the stack becomes
     *      dead while blocked rather than live).
     *   3. one kernel stack per CORE instead of per thread.
     *
     * Step 2 is mechanical ONCE step 1 holds for every blocking path, and
     * unsafe before it: a handler that keeps a local across a block needs the
     * frame that step 2 throws away.  So the order is not a preference.
     *
     * `sc_restart` is set by a handler that wants to be re-entered.  The
     * dispatcher clears it, reschedules, and re-dispatches the SAME syscall
     * with the SAME arguments — which is why they are saved here rather than
     * re-read from the user's registers, whose frame step 2 will discard.
     */
    /*
     * Stage 9-evt Step 2 — the user context a syscall must return to.
     *
     * It lives on the thread's kernel stack today, pushed by syscall_entry
     * before the dispatch call.  That is precisely the frame step 2 abandons
     * when a handler parks, so the return address, flags and stack pointer
     * have to be somewhere the abandonment does not destroy.
     *
     * Saved on every syscall entry, which costs three stores on a path that
     * already does eight pushes.  Cheap enough that making it conditional
     * would be a worse trade than the branch.
     */
    uint64_t          sc_user_rip;
    uint64_t          sc_user_rflags;
    uint64_t          sc_user_rsp;
    /*
     * The user's CALLEE-SAVED registers, and the reason an event kernel has no
     * choice but to save them.
     *
     * A syscall preserves rbx, rbp and r12-r15 for its caller; the kernel does
     * that for free by obeying the C ABI, because the values stay in registers
     * or get spilled onto the syscall's own frame.  Abandoning that frame
     * throws the spills away, so a thread resumed on a fresh stack would return
     * to ring 3 with whatever the kernel last left in those registers.
     *
     * The symptom was precise and worth recording: a service came back from a
     * blocking syscall with a kernel stack address in rbp and wrote through it
     * on its next frame access — a userland page fault at 0xFFFF8001..., which
     * reads like a wild pointer and is actually a register restore that never
     * happened.
     *
     * Order matches struct cpu_context so the two can be read side by side:
     * r15, r14, r13, r12, rbx, rbp.
     */
    uint64_t          sc_user_regs[6];
    uint8_t           sc_restart;
    /*
     * Set by the dispatcher before a RE-dispatch, clear on first entry.
     *
     * A restartable handler must be able to tell "I am starting" from "I am
     * resuming", because the two do different things with the same arguments —
     * and it cannot infer it from the state it parked on, since the scheduler
     * clears that state when it wakes the thread.  SYS_SLEEP learned this the
     * expensive way: keying off `wake_tick == 0` meant a woken sleeper saw no
     * deadline, computed a fresh one from the original duration, and slept for
     * ever.  This is the bit that distinguishes the two entries, and it is on
     * the thread rather than in a handler-specific field so every path
     * converted after this one gets it for free.
     */
    uint8_t           sc_reentry;
    /*
     * The ONE object reference a restartable syscall carries across its park.
     *
     * A parked handler kept its references in C locals, which the frame kept
     * alive.  A restartable one returns, so a reference it still needs — the
     * endpoint a queued receiver is waiting on, whose object must not be freed
     * while the queue points at the thread — has to live somewhere the return
     * does not destroy.  `blocking_ep` cannot serve: the wakers clear it, by
     * design, and the reference would be lost exactly when it is needed.
     *
     * Released by the completion path on re-entry.  One slot, because no
     * blocking syscall holds two, and a second would be a sign the handler is
     * carrying state rather than a continuation.
     */
    struct KObject   *sc_held;
    /*
     * A running total a restartable syscall carries across its slices.
     *
     * A preemptible operation that reports "how much did you do" cannot report
     * per-slice — the caller asked once and expects one answer.  This is where
     * the partial answer lives between re-executions, for the same reason
     * everything else here does: the frame that used to hold it is gone.
     */
    uint64_t          sc_acc;
    /*
     * Stage 8-cap / D-2 — the GUARD on this thread's root CSpace capability.
     *
     * Guards live in the capability, and every CNode capability in a slot
     * carries its own (KCSlot.guard).  The root is the one capability a thread
     * does not reach through a slot: it is a structural pointer, so its guard
     * has nowhere to live but here.  That asymmetry is not a design choice, it
     * is what `cspace_root` being a pointer rather than a capability costs.
     *
     * Set by SYS_TCB_CONFIGURE's arg3, which is seL4's `cspace_root_data` —
     * the same argument carrying the same meaning.  Zero bits means no guard,
     * which is every thread until one asks otherwise.
     */
    uint64_t          cspace_root_guard;
    uint8_t           cspace_root_guard_bits;
    uint64_t          sc_num;
    /*
     * The syscall's arguments, kept where a restart can find them (D-1).
     *
     * An array since A-33, because the message ABI addresses them by POSITION
     * — `ipc_msg.h` says which word of an invocation carries the MessageInfo
     * and which carry the message registers, and one definition of that map is
     * the ABI.  sc_arg[0] is the invoked capability, sc_arg[1] the method, and
     * sc_arg[1 + n] the method's n'th argument.
     */
    uint64_t          sc_arg[9];
    /*
     * A-33 — the return message.
     *
     * A receive returns a whole message and a message does not fit in a return
     * value, so the seven words go here and the exit path puts them in the
     * registers they arrived in.  Initialised from the ARGUMENTS at entry, so
     * a call that returns only a status hands the caller its own inputs back
     * and no handler has to say anything.
     */
    uint64_t          sc_ret[7];
    uint32_t          sc_restart_count;  /* diagnostic: restarts observed */

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
    /* A-24: the ONE deadline the kernel still keeps, and it is not a thread's
     * — it is when a scheduling context's budget comes back (TASK_BUDGET_
     * EXHAUSTED).  It used to also carry SYS_SLEEP's wake time and every timed
     * IPC wait's. */
    uint64_t          wake_tick;

    /* Synchronous endpoint IPC staging (Ph66+). */
    struct ipc_stage      ipc_msg;         /* 64-byte staging/delivery buffer */
    uint32_t            ipc_msg_ready;   /* set by sender on successful rendezvous */
    uint32_t            ipc_ep_closed;   /* set by kendpoint_close while task was blocked */
    struct task        *ep_next;         /* intrusive link for endpoint queue */
    /*
     * ...and for the notification queue.  A separate link rather than a shared
     * one, because a thread's membership of the two is a different fact and
     * sharing the field would make "is it queued anywhere" unanswerable from
     * either side.  Costs one pointer per thread and removes a ceiling that
     * cost four per notification.
     */
    struct task        *notif_next;      /* intrusive link for notification queue */
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
    uint64_t            ep_recv_buf_uptr;/* receiver's output buffer user addr (set at EP_RECV) */
    /* A1.5: receiver-declared receive-slot (direct root-CNode CPtr, 1..1023;
     * 0 = none/legacy).  Written by EVERY recv-family syscall entry
     * (EP_RECV / EP_NB_RECV / EP_CALL) and consumed by at most one routed
     * cap delivery, so it can never leak across operations. */
    uint32_t            ep_recv_slot;
    /*
     * `ipc_kbuf` is DELETED (ledger D-4).
     *
     * 256 bytes inside every TCB, where the kernel staged a message's bulk
     * payload: a size the user did not choose, memory it did not pay for, and
     * a buffer it could not name with a capability.  Every thread carried it
     * whether it sent a payload or not.
     *
     * A payload now lives in a FRAME the thread registered, and a thread with
     * none cannot send one — which is seL4's answer, where the message
     * registers travel in registers and anything longer needs somewhere that
     * somebody owns.
     */
    /*
     * D-4 — the thread's IPC buffer, as a capability.
     *
     * A registered frame REPLACES the staging above for this thread: the user
     * writes its payload into a page it owns, mapped where it chose, and the
     * kernel transfers buffer-to-buffer through its own window on the two
     * frames.  No user pointer is named per call, nothing is validated per
     * call, and the size is the frame's rather than a kernel constant.
     *
     * NULL means the thread has not registered one and still goes through the
     * staging path; that fallback is what the ledger's D-4 row calls
     * MIGRATING, and it is what the boot services still use.
     *
     * `ipc_buffer_uvaddr` is where the OWNER mapped it.  The kernel does not
     * need it to do the copy — it uses the frame's physical address — and
     * keeps it because a thread's IPC buffer address is part of what a
     * debugger, a fault handler or the thread itself has to be able to ask
     * for, and because registering a frame the caller cannot name a mapping
     * for is a mistake worth refusing at the door.
     *
     * Carries active + lifecycle refs while set.
     */
    struct KFrame      *ipc_buffer;
    uint64_t            ipc_buffer_uvaddr;
    /*
     * D-1 step 3 — the thread's ring-3 register state, saved on every kernel
     * entry from user mode and restored on every return to it.
     *
     * It lives here rather than on a kernel stack because an event kernel has
     * one stack per core: a handler that hands the CPU to another thread
     * leaves nothing behind that the incoming thread will not overwrite.  See
     * isr_save_user_ctx / isr_restore_user_ctx.
     */
    struct iris_user_ctx user_ctx;
    /*
     * How this thread resumes (Stage 9-evt step 3).
     *
     * 1 — it was interrupted in RING 3, so `user_ctx` above is its whole state
     *     and resuming it is an iretq off the core's stack.
     * 0 — it resumes INSIDE the kernel: a syscall that parked and must re-run
     *     from its restart trampoline, a thread that has never run, or a
     *     kernel thread.
     *
     * Written where the thread ENTERS the kernel rather than where it leaves,
     * because that is the event that decides the answer: `isr_save_user_ctx`
     * sets it, `syscall_save_user_ctx` clears it.  A thread that made a
     * syscall must not be iretq'd back to the ring-3 context some earlier
     * interrupt saved — it has a syscall to finish.
     */
    uint32_t             resume_user;
    /*
     * Where a KERNEL resume enters (resume_user == 0).
     *
     * A parked syscall's restart trampoline, or a kernel thread's entry.  It
     * runs ON THE CORE'S STACK, called by the dispatcher, which is what
     * removed the second reason a thread needed a stack of its own: the first
     * was its ring-3 context, which moved to `user_ctx`, and this was the
     * other.
     */
    void               (*kentry)(void);
    /* Ph74: optional scheduling context — retained KSchedContext ref (NULL = best-effort) */
    struct KSchedContext *sched_ctx;
    /* Ph85: reply capability fields */
    uint32_t       ep_call_mode;    /* 1 if task entered EP via SYS_EP_CALL (wants reply) */
    /*
     * Ledger A-22: 1 if the call queued on this endpoint is a FAULT, not a
     * syscall.  The endpoint machinery treats it like any other call — that is
     * the whole point — but the two ends differ, and both differences are
     * about there being no syscall frame underneath it:
     *
     *   - nothing is restarted on wake-up.  A fault caller resumes at the
     *     instruction that faulted, from the trap frame the CPU pushed, so
     *     the reply must NOT write a message back into user memory or return
     *     a syscall value; it just makes the thread runnable again.
     *   - the message was built by the kernel, so there is no staged bulk
     *     payload and no capability to transfer.
     */
    uint32_t       ep_fault_call;
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
    /*
     * Execution has ended, or is ending (TERMINATED).
     *
     * ATOMIC, and it is the gate that lets exactly one processor tear a thread
     * down (SMP roadmap §9.3 step 5).  It used to be a plain byte set near the
     * END of teardown, and every entry to teardown tested it first — an
     * unlocked read that four cores calling Exit on one thread all pass, so
     * all four tore the same thread down: the registry slot released four
     * times, the CSpace, the address space and the scheduling context released
     * four times, and a TCB whose storage had gone back to its Untyped while
     * somebody was still walking it.
     *
     * It is claimed with an exchange at the top of teardown now, so the flag
     * means "this thread is being torn down OR has been".  Every reader uses
     * it as "do not touch this thread", and that is true of both.
     */
    _Atomic uint8_t terminal;
    /*
     * Scheduler membership, as a LIST rather than an index.
     *
     * It was `reg_slot`, an index into ktcb_registry[TASK_MAX] — which made
     * TASK_MAX a ceiling on live threads, and `task_registry_alloc` returned
     * NO_MEMORY when the array filled.  seL4 has no thread limit: a TCB exists
     * because somebody retyped one.  The registry's only remaining job was to
     * let the scheduler WALK every live thread looking for expired deadlines,
     * and a walk wants a list (charter P2, ledger A-19).
     */
    int32_t        reg_slot;     /* 1 while on the scheduler list, else -1 */
    struct task   *sched_prev;   /* intrusive links for that list */
    struct task   *sched_next;
    /* `kstack_slot` is GONE with the per-thread kernel stack (D-1, step 3).
     * It recorded which slot of the KSTACK_VIRT_BASE region a thread owned;
     * after the region went, two sites still set it to -1 and nothing ever
     * read it. */

    /* SMP: CPU this task is homed to (its run queue owner).
     * Set at creation time; stays constant for the task's lifetime. */
    uint8_t           home_cpu;

    /*
     * Is a processor still standing on this thread?  (SMP roadmap §9.3 step 4.)
     *
     * Not "is it RUNNING" — that is a scheduling state, and a thread stops
     * being RUNNING at the instant it decides to block, which is several
     * hundred instructions before the core that was executing it is finished
     * with it.  In that gap the core still has to save the thread's FPU
     * registers, flush its scheduling context and read its state; and in that
     * same gap the thread is already visible to whoever it blocked on, so
     * another core can satisfy the wait, mark it READY, put it in a run queue
     * and RESUME IT.  Two processors then run one thread: one of them restores
     * an FPU image the other is in the middle of writing, and iretqs into a
     * register set that is being rewritten under it.
     *
     * This flag is what a core holds instead.  It is raised by the dispatcher
     * that commits to the thread and lowered by the dispatcher that has
     * finished releasing it, and nothing may resume a thread while it is up.
     * It is the only thing in the kernel that answers "is this thread on a
     * processor" truthfully; `state` answers a different question, and
     * `cpu_local[].current_task` answers it one instruction too early.
     */
    _Atomic uint8_t   on_cpu;

    /* The scheduling DOMAIN this thread runs in (seL4's tcbDomain).  A thread
     * is dispatchable only while its domain holds the CPU, whatever its
     * priority.  Every thread starts in domain 0, so a system that never
     * configures one behaves exactly as it did before domains existed.
     * Changed only through `Domain_Set`, which needs its own boot authority. */
    uint8_t           domain;

    /* FPU/SSE state — 512-byte FXSAVE image, must be 16-byte aligned.
     * Saved and restored on every context switch so FPU state never leaks
     * across task boundaries. Placed last to keep alignment padding minimal. */
    uint8_t           fpu_state[512] __attribute__((aligned(16)));

    /*
     * `next` is DELETED (SMP roadmap §9.3 step 4).
     *
     * It threaded every thread onto a circular list whose head was
     * `task_list_head`.  Two things used that list: a pointer comparison
     * meaning "the idle thread", and a walk to find a dying thread's
     * predecessor so it could be spliced out.  Nothing read it otherwise — so
     * it was a list maintained in order to be maintained, and with four
     * processors it was an unlocked structure being spliced on every thread
     * create and destroy.  The list the kernel actually uses is
     * `sched_thread_list`, under `sched_list_lock`, with the links above.
     */
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
/* D-4 gauges, defined in syscall_tcb.c: how many threads hold a registered IPC
 * buffer, and the teardown hook that keeps the count honest. */
uint32_t ipc_buffers_registered(void);
void     ipc_buffer_gauge_drop(void);

/* Stage 9-evt step 3 — give the CPU to `next`; never returns.  Defined in
 * scheduler.c, named by the dispatcher and by the abandoning park. */
__attribute__((noreturn))
void sched_resume(struct task *next, struct task *outgoing);

/* The two halves of what context_switch used to do inline; a thread resumed
 * from its TCB never goes through a switch (kernel/arch/x86_64). */
void fpu_save_to(void *area);
void fpu_restore_from(void *area);

/* Resume a ring-3 context straight from a TCB (kernel/arch/x86_64). */
__attribute__((noreturn))
void restore_user_ctx_and_iretq(const struct iris_user_ctx *ctx);

/* A thread's FIRST entry into ring 3, from its TCB (kernel/arch/x86_64).
 * Differs from the above in what a thread that has never run still needs: data
 * selectors, CR3, and IA32_KERNEL_GS_BASE for its first syscall's swapgs. */
__attribute__((noreturn))
void start_user_ctx_and_iretq(const struct iris_user_ctx *ctx);

/* resume_user values. */
#define TASK_RESUME_KERNEL     0u   /* kentry(), on the core stack */
#define TASK_RESUME_USER       1u   /* iretq from user_ctx          */
#define TASK_RESUME_USER_FIRST 2u   /* ...and set up ring 3 first   */

/* Enter the per-core dispatcher on the core's stack; never returns. */
__attribute__((noreturn))
void core_dispatch_enter(struct task *outgoing);

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
