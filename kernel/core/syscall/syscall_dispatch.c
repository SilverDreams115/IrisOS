/* SPDX-License-Identifier: Apache-2.0 */
/*
 * syscall_dispatch.c — syscall infrastructure: MSR setup, dispatch table.
 *
 * Contains syscall_init (MSR wiring), syscall_set_kstack, and the
 * syscall_dispatch switch that routes each syscall number to its handler.
 * All sys_* implementations live in the syscall_*.c subsystem files.
 */
#include "syscall_priv.h"
#include <iris/panic.h>
#include <iris/serial.h>
#include <stdatomic.h>
#include <iris/cpu_local.h>

/* MSR addresses */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void syscall_entry(void);

/* `_sc_putc` is gone: a raw COM1 write from inside the syscall dispatcher,
 * uncalled.  The kernel's output path is klog, drained by the console service;
 * a second one that bypasses it is a debugging aid that outlived its session. */
/*
 * Stage 9-evt Step 2 — the syscall frame, as C sees it.
 *
 * syscall_entry pushes this and then calls into C.  Naming the layout HERE,
 * next to the only code that reads it, is what lets the user context be copied
 * into the TCB without hard-coding `struct task` offsets in assembly — which
 * is how a struct field added in C silently corrupts a register save.
 *
 * Offsets are from the stack pointer AFTER the 16-byte alignment adjustment,
 * which is where syscall_entry calls from.
 */
struct syscall_frame {
    uint64_t pad;          /*   0 — alignment                       */
    uint64_t num;          /*   8 — rax                             */
    uint64_t arg0;         /*  16 — rdi                             */
    uint64_t arg1;         /*  24 — rsi                             */
    uint64_t arg2;         /*  32 — rdx                             */
    uint64_t arg3;         /*  40 — r10                             */
    uint64_t user_rip;     /*  48 — rcx, set by the syscall insn    */
    uint64_t user_rflags;  /*  56 — r11, set by the syscall insn    */
    uint64_t user_rsp;     /*  64 — the caller's stack              */
    uint64_t arg4;         /*  72 — r8  (ledger A-32)               */
    uint64_t arg5;         /*  80 — r9  (ledger A-33)               */
    uint64_t arg6;         /*  88 — r15 (ledger A-33)               */
    uint64_t arg7;         /*  96 — r14 (ledger A-33)               */
    uint64_t arg8;         /* 104 — r13 (ledger A-33)               */
    /* Callee-saved, pushed FIRST so the offsets above did not move.
     * r15, r14 and r13 are ABOVE, because they are arguments now: the same
     * three pushes, carrying the user's values on the way in and the message
     * on the way out. */
    uint64_t user_r12;     /* 112 */
    uint64_t user_rbx;     /* 120 */
    uint64_t user_rbp;     /* 128 */
};

/*
 * Copy the user context out of the frame and into the thread.
 *
 * Called once per syscall entry.  The frame is where it lives today and where
 * the return path still reads it from; this is the copy that survives the
 * frame, so that a parked syscall can be resumed by re-entering the dispatcher
 * on a fresh stack rather than by returning through a preserved one.
 */
void syscall_save_user_ctx(struct syscall_frame *f) {
    struct task *t = task_current();
    if (!t || !f) return;
    t->sc_user_rip    = f->user_rip;
    t->sc_user_rflags = f->user_rflags;
    t->sc_user_rsp    = f->user_rsp;
    /* Callee-saved too: abandoning the frame throws away the spills that
     * would otherwise have preserved them for the caller. */
    /* r15 and r14 are message registers now (A-33), so what a parked thread
     * gets back in them is what its receive delivered — not what it happened
     * to be holding.  They are saved here anyway, because a thread that parked
     * in a call which returns no message must find them unchanged. */
    t->sc_user_regs[0] = f->arg6;   /* r15 */
    t->sc_user_regs[1] = f->arg7;   /* r14 */
    t->sc_user_regs[2] = f->arg8;   /* r13 */
    t->sc_user_regs[3] = f->user_r12;
    t->sc_user_regs[4] = f->user_rbx;
    t->sc_user_regs[5] = f->user_rbp;

    /*
     * A-33: the return message starts out as the arguments.
     *
     * A call that returns nothing but a status writes none of these, and the
     * caller gets its own inputs back — strictly kinder than the clobber ring
     * 3 already assumes, and it means only the calls that RETURN a message
     * have to say anything.
     */
    t->sc_ret[0] = f->arg0;
    t->sc_ret[1] = f->arg1;
    t->sc_ret[2] = f->arg2;
    t->sc_ret[3] = f->arg3;
    t->sc_ret[4] = f->arg4;
    t->sc_ret[5] = f->arg5;
    t->sc_ret[6] = f->arg6;
    /*
     * Step 3: a thread that made a SYSCALL resumes in the KERNEL, not at the
     * ring-3 context some earlier interrupt saved.  It has a syscall to
     * finish, and iretq-ing it back to where the timer caught it last would
     * silently drop that.
     */
    t->resume_user = 0u;
}

/*
 * Stage 9-evt Step 1 — the restart loop (ledger D-1).
 *
 * A handler that cannot complete calls syscall_request_restart() and returns.
 * Its continuation is in THREAD state, never in its own locals, so re-entering
 * it from the top is equivalent to resuming it — which is what an event kernel
 * does and what a per-thread kernel stack currently makes unnecessary.
 *
 * When this was written the reschedule happened through `task_yield()` from
 * inside this frame, so the frame was still parked on the thread's kernel
 * stack: the step did not yet remove the stack, it removed the REASON the
 * stack had to be kept.  Both have since happened — step 2 abandons the frame
 * and step 3 deleted `task_yield` along with the per-thread stack.  Once
 * every blocking path is restart-safe, step 2 abandons the frame here instead
 * of yielding through it, and step 3 makes the stack per-core.
 *
 * The arguments are re-read from the thread rather than reused from the
 * registers because the register frame is exactly what step 2 discards.
 * Writing it that way now means step 2 changes this function and nothing else.
 */
static uint64_t syscall_dispatch_one(uint64_t num, uint64_t arg0,
                                     uint64_t arg1, uint64_t arg2,
                                     uint64_t arg3, uint64_t arg4,
                                     uint64_t arg5, uint64_t arg6,
                                     uint64_t arg7, uint64_t arg8);

/*
 * A-33 — copy the return message out of the thread and into the frame.
 *
 * Called by syscall_entry between the dispatch and the restore, from a pointer
 * to the frame, so that `struct task`'s field offsets stay out of assembly.
 */
void syscall_store_user_ret(struct syscall_frame *f) {
    struct task *t = task_current();
    if (!t || !f) return;
    f->arg0 = t->sc_ret[0];
    f->arg1 = t->sc_ret[1];
    f->arg2 = t->sc_ret[2];
    f->arg3 = t->sc_ret[3];
    f->arg4 = t->sc_ret[4];
    f->arg5 = t->sc_ret[5];
    f->arg6 = t->sc_ret[6];
}

/* Global restart gauge — the only way, from outside, to tell a restartable
 * blocking path from a stack-parked one. */
static _Atomic uint32_t syscall_restart_total;

uint32_t syscall_restart_count(void) {
    return atomic_load_explicit(&syscall_restart_total, memory_order_relaxed);
}

void syscall_request_restart(struct task *t) {
    if (!t) return;
    t->sc_restart = 1u;
    t->sc_restart_count++;
    atomic_fetch_add_explicit(&syscall_restart_total, 1u, memory_order_relaxed);
}

/*
 * Stage 9-evt — run one syscall to completion, parking as needed.
 *
 * Parking ABANDONS this frame and does not come back: `task_park_restart`
 * records the thread's resume point in its TCB, moves the CPU to the CORE's
 * kernel stack, and enters the dispatcher.  Nothing of this call survives the
 * block, which is the property D-1 is about.
 *
 * Step 2 had a fallback here — "nobody else to run, so keep this frame and
 * re-dispatch" — and step 3 removed the condition it existed for.  The
 * dispatcher's answer to "nobody else can run" is to wait for an interrupt on
 * the core's stack, which is what the idle task used to be; there is no longer
 * a case in which keeping a frame is cheaper than leaving.  The loop that
 * remains is not a loop: it runs once and either returns or leaves.
 */
static uint64_t syscall_run(struct task *t, uint64_t num, uint64_t arg0,
                            uint64_t arg1, uint64_t arg2, uint64_t arg3,
                            uint64_t arg4, uint64_t arg5, uint64_t arg6,
                            uint64_t arg7, uint64_t arg8) {
    uint64_t r = syscall_dispatch_one(num, arg0, arg1, arg2, arg3, arg4,
                                      arg5, arg6, arg7, arg8);
    if (!t || !t->sc_restart) { if (t) t->sc_reentry = 0u; return r; }

    t->sc_restart = 0u;
    t->sc_reentry = 1u;
    task_park_restart();              /* never returns */
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6,
                          uint64_t arg7, uint64_t arg8) {
    struct task *t = task_current();
    if (t) {
        t->sc_num = num;
        t->sc_arg[0] = arg0; t->sc_arg[1] = arg1; t->sc_arg[2] = arg2;
        t->sc_arg[3] = arg3; t->sc_arg[4] = arg4; t->sc_arg[5] = arg5;
        t->sc_arg[6] = arg6; t->sc_arg[7] = arg7; t->sc_arg[8] = arg8;
        t->sc_restart = 0u;
        t->sc_reentry = 0u;
    }
    return syscall_run(t, num, arg0, arg1, arg2, arg3, arg4, arg5, arg6,
                       arg7, arg8);
}

/*
 * Where an abandoned syscall comes back to life.
 *
 * Entered by the context switch on a FRESH kernel stack — the frame the
 * original call ran on is gone, and everything this needs is in the thread:
 * the syscall number and its arguments, and the user context to return to.
 * That is the whole of what step 1 was for.
 *
 * Never returns: it goes straight to ring 3 through an iretq built from the
 * saved user context, because there is no frame to return through.
 */
/*
 * How many syscalls have resumed on a fresh stack.
 *
 * The only evidence that step 2 is doing anything: a restart that fell back to
 * yielding through its own frame and one that abandoned it are identical from
 * outside, and both advance the restart gauge.  This advances only for the
 * abandonment, because the trampoline is the only way an abandoned syscall
 * can complete.
 */
static _Atomic uint32_t syscall_abandon_total;

uint32_t syscall_abandon_count(void) {
    return atomic_load_explicit(&syscall_abandon_total, memory_order_relaxed);
}

__attribute__((noreturn)) void syscall_restart_trampoline(void) {
    struct task *t = task_current();
    /* Reached only from the abandon path in task_yield_impl, which sets this
     * as the resume RIP for a task it is about to hand the CPU away from; a
     * task cannot be scheduled without being current. */
    IRIS_ASSERT(t && 1, "syscall restart trampoline with no current task");

    atomic_fetch_add_explicit(&syscall_abandon_total, 1u, memory_order_relaxed);
    t->sc_reentry = 1u;

    uint64_t r = syscall_run(t, t->sc_num, t->sc_arg[0], t->sc_arg[1],
                             t->sc_arg[2], t->sc_arg[3], t->sc_arg[4],
                             t->sc_arg[5], t->sc_arg[6], t->sc_arg[7],
                             t->sc_arg[8]);

    /*
     * And return to ring 3 without a syscall frame — the whole point of step 2.
     * Everything ring 3 needs came out of the TCB, where syscall_save_user_ctx
     * put it at entry.
     */
    syscall_return_to_user(r, t->sc_user_rip, t->sc_user_rflags,
                           t->sc_user_rsp, t->sc_user_regs, t->sc_ret);
    __builtin_unreachable();
}

/*
 * Ledger A-32 — how many calls still came through the numbered door.
 *
 * The invocation ABI is adopted one caller at a time, and a caller that was
 * never migrated keeps working: that is what makes the migration safe and also
 * what makes it fail SILENTLY.  D-4 already recorded this exact shape — a
 * service whose IPC-buffer registration was refused kept using the staging
 * path and the whole suite passed either way.  So this counts, from the first
 * commit of the conversion, and `SYS_UNTYPED_QUERY` reports it: the number
 * must be falling while stage C runs and zero when stage E closes.
 *
 * Relaxed: nothing branches on it.
 */
static _Atomic uint64_t syscall_numbered_calls;

uint64_t syscall_numbered_call_count(void) {
    return atomic_load_explicit(&syscall_numbered_calls, memory_order_relaxed);
}

static uint64_t syscall_dispatch_one(uint64_t num, uint64_t arg0,
                                     uint64_t arg1, uint64_t arg2,
                                     uint64_t arg3, uint64_t arg4,
                                     uint64_t arg5, uint64_t arg6,
                                     uint64_t arg7, uint64_t arg8) {
    /* The invocation door. */
    if (num == SYS_INVOKE)
        return syscall_invoke(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
                              arg8);

    /*
     * Everything below is the numbered door, and what takes it is counted —
     * EXCEPT the calls that are meant to stay numbers.
     *
     * seL4 keeps `seL4_Yield` as a real syscall because it invokes nothing;
     * IRIS keeps `SYS_EXIT` for the same reason a thread ending itself names
     * no object, and `SYS_CLOCK_GET` because A-27 answered it rather than
     * retiring it.  Counting those would make the gauge measure traffic
     * instead of migration: the suite spins on YIELD in its settle loops, and
     * the first reading was 438,901 — almost all of it one call that is not
     * going anywhere.  A number that cannot reach zero is not a progress bar.
     */
    if (num != SYS_EXIT && num != SYS_YIELD && num != SYS_CLOCK_GET)
        atomic_fetch_add_explicit(&syscall_numbered_calls, 1u,
                                  memory_order_relaxed);

    switch (num) {
    /*
     * What is left of the numbered table (ledger A-32).
     *
     * Three calls, and each is here because it invokes NOTHING.  seL4 keeps
     * `seL4_Yield` as a real syscall for exactly this reason: there is no
     * capability it could be a method of.  A thread ending itself names no
     * object either, and `SYS_CLOCK_GET` reads a counter that A-27 established
     * is unprivileged on this architecture anyway — retiring it would have
     * bought nothing, so it was answered rather than removed.
     *
     * Everything else is a method, reached by naming the capability it acts
     * on.  The ninety-odd cases that used to be here are gone, and with them
     * the thirty-two stubs whose entire body was a refusal: a number that
     * names nothing is refused by this switch having no case for it, which is
     * the same answer with nothing to maintain.
     */
    case SYS_EXIT:      return sys_exit(arg0, arg1, arg2);
    case SYS_YIELD:     return sys_yield(arg0, arg1, arg2);
    case SYS_CLOCK_GET: return sys_clock_get(arg0, arg1, arg2);
    default:
        /* A number that names nothing.  It reached no method, so it does not
         * count as a caller still using the numbered door — take the increment
         * above back.  The distinction matters because T148 fuzzes every hole
         * in the table on purpose, and a gauge that counted those would read
         * as if the migration had stalled. */
        atomic_fetch_sub_explicit(&syscall_numbered_calls, 1u,
                                  memory_order_relaxed);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

/*
 * Where a syscall lands, and which address space it came from — both per-CPU.
 *
 * They also wrote a RIP-relative global copy of each, "for debug".  The copies
 * are deleted (SMP roadmap §9.3 step 4): a per-CPU value with a global twin is
 * a value that will eventually be read from the wrong one, and it was — see
 * the note where the globals used to live, at the top of syscall_entry.S.
 *
 * `cpu_self()` is this core's block from any ring-0 context, so both of these
 * are correct on any processor and always have been.
 */
void syscall_set_kstack(uint64_t kstack_top) {
    cpu_self()->syscall_kstack = kstack_top;
}

void syscall_set_user_cr3(uint64_t val) {
    cpu_self()->syscall_user_cr3 = val;
}

void syscall_init(void) {
    /* enable SCE bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= (1ULL << 0); /* SCE = syscall enable */
    wrmsr(MSR_EFER, efer);

    /* STAR: bits 47:32 = kernel CS (syscall: CS=this, SS=this+8)
     *       bits 63:48 = X       (sysretq: CS=X+16|3, SS=X+8|3)
     * GDT layout: slot3=user_data(0x1B), slot4=user_code(0x23)
     * STAR[47:32] = 0x0008  (syscall:  CS=0x08, SS=0x10)
     * STAR[63:48] = 0x0013  (sysretq: CS=(0x13+16)|3=0x23, SS=(0x13+8)|3=0x1B)
     */
    uint64_t star = 0;
    star |= ((uint64_t)0x0008 << 32); /* kernel CS selector */
    star |= ((uint64_t)0x0013 << 48); /* sysretq: CS=0x23 (user code), SS=0x1B (user data) */
    wrmsr(MSR_STAR, star);

    /* LSTAR: syscall handler entry point */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* SFMASK: clear IF on syscall entry (disable interrupts) */
    wrmsr(MSR_SFMASK, (1ULL << 9)); /* IF = bit 9 */
}
