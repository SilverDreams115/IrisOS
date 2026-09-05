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

static inline void _sc_putc(char c) {
    uint8_t s;
    do { __asm__ volatile ("inb %1,%0":"=a"(s):"Nd"((uint16_t)0x3FD)); } while (!(s&0x20));
    __asm__ volatile ("outb %0,%1"::"a"((uint8_t)c),"Nd"((uint16_t)0x3F8));
}
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
    /* Callee-saved, pushed FIRST so the offsets above did not move. */
    uint64_t user_r15;     /*  72 */
    uint64_t user_r14;     /*  80 */
    uint64_t user_r13;     /*  88 */
    uint64_t user_r12;     /*  96 */
    uint64_t user_rbx;     /* 104 */
    uint64_t user_rbp;     /* 112 */
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
    t->sc_user_regs[0] = f->user_r15;
    t->sc_user_regs[1] = f->user_r14;
    t->sc_user_regs[2] = f->user_r13;
    t->sc_user_regs[3] = f->user_r12;
    t->sc_user_regs[4] = f->user_rbx;
    t->sc_user_regs[5] = f->user_rbp;
}

/*
 * Stage 9-evt Step 1 — the restart loop (ledger D-1).
 *
 * A handler that cannot complete calls syscall_request_restart() and returns.
 * Its continuation is in THREAD state, never in its own locals, so re-entering
 * it from the top is equivalent to resuming it — which is what an event kernel
 * does and what a per-thread kernel stack currently makes unnecessary.
 *
 * Today the reschedule happens through task_yield() from inside this frame, so
 * the frame is still parked on the thread's kernel stack: this step does not
 * yet remove the stack, it removes the REASON the stack has to be kept.  Once
 * every blocking path is restart-safe, step 2 abandons the frame here instead
 * of yielding through it, and step 3 makes the stack per-core.
 *
 * The arguments are re-read from the thread rather than reused from the
 * registers because the register frame is exactly what step 2 discards.
 * Writing it that way now means step 2 changes this function and nothing else.
 */
static uint64_t syscall_dispatch_one(uint64_t num, uint64_t arg0,
                                     uint64_t arg1, uint64_t arg2,
                                     uint64_t arg3);

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
 * Stage 9-evt Step 2 — run one syscall to completion, parking as needed.
 *
 * The preferred park ABANDONS this frame: task_park_restart schedules the
 * thread to resume at syscall_restart_trampoline on a fresh stack and does not
 * return, so nothing of this call survives the block.  That is the property
 * D-1 is about — a blocked thread's kernel stack holds nothing.
 *
 * It can decline, and then the loop below is the fallback: yield through this
 * frame and re-dispatch, which is exactly what step 1 did.  Declining happens
 * when there is nobody else to run, and in that case there is no stack to save
 * by leaving — the alternative to keeping this frame is idling on it.
 */
static uint64_t syscall_run(struct task *t, uint64_t num, uint64_t arg0,
                            uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    for (;;) {
        uint64_t r = syscall_dispatch_one(num, arg0, arg1, arg2, arg3);
        if (!t || !t->sc_restart) { if (t) t->sc_reentry = 0u; return r; }

        t->sc_restart = 0u;
        task_park_restart();          /* usually does not return */

        /* Fallback: nobody else to run, so this frame stays and re-dispatches. */
        t->sc_reentry = 1u;
        num  = t->sc_num;  arg0 = t->sc_arg0; arg1 = t->sc_arg1;
        arg2 = t->sc_arg2; arg3 = t->sc_arg3;
    }
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (t) {
        t->sc_num  = num;  t->sc_arg0 = arg0; t->sc_arg1 = arg1;
        t->sc_arg2 = arg2; t->sc_arg3 = arg3; t->sc_restart = 0u;
        t->sc_reentry = 0u;
    }
    return syscall_run(t, num, arg0, arg1, arg2, arg3);
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

    uint64_t r = syscall_run(t, t->sc_num, t->sc_arg0, t->sc_arg1,
                             t->sc_arg2, t->sc_arg3);

    /*
     * And return to ring 3 without a syscall frame — the whole point of step 2.
     * Everything ring 3 needs came out of the TCB, where syscall_save_user_ctx
     * put it at entry.
     */
    syscall_return_to_user(r, t->sc_user_rip, t->sc_user_rflags,
                           t->sc_user_rsp, t->sc_user_regs);
    __builtin_unreachable();
}

static uint64_t syscall_dispatch_one(uint64_t num, uint64_t arg0,
                                     uint64_t arg1, uint64_t arg2,
                                     uint64_t arg3) {
    switch (num) {
        /* SYS_WRITE(0), SYS_BRK(7) — retired, fall to default */
        case SYS_GETPID: return sys_getpid(arg0, arg1, arg2);
        case SYS_EXIT:  return sys_exit(arg0, arg1, arg2);
        case SYS_YIELD: return sys_yield(arg0, arg1, arg2);
        case SYS_SLEEP: return sys_sleep(arg0, arg1, arg2);
        /* SYS_CHAN_CREATE(12)/SEND(13)/RECV(14) — retired Phase 13/Track G
         * (KChannel fully retired), fall to default → NOT_SUPPORTED */
        case SYS_HANDLE_CLOSE: return sys_handle_close(arg0, arg1, arg2);
        case SYS_VMO_CREATE:  return sys_vmo_create(arg0, arg1, arg2);
        case SYS_VMO_MAP:     return sys_vmo_map(arg0, arg1, arg2);
        case SYS_VMO_UNMAP:   return sys_vmo_unmap(arg0, arg1, arg2);
        /* SYS_SPAWN(18), SYS_SPAWN_SERVICE(31) — retired, fall to default */
        case SYS_NOTIFY_CREATE: return sys_notify_create(arg0, arg1, arg2);
        case SYS_NOTIFY_SIGNAL: return sys_notify_signal(arg0, arg1, arg2);
        case SYS_NOTIFY_WAIT:   return sys_notify_wait(arg0, arg1, arg2);
        case SYS_HANDLE_DUP:    return sys_handle_dup(arg0, arg1, arg2);
        /* SYS_HANDLE_TRANSFER(23) — retired A1.8 (zero in-tree callers; the
         * cross-process placement path is SYS_PROC_CSPACE_MINT), fall to
         * default (NOT_SUPPORTED).  Number permanently reserved. */
        case SYS_PROCESS_SELF:    return sys_process_self(arg0, arg1, arg2);
        case SYS_PROCESS_STATUS:  return sys_process_status(arg0, arg1, arg2);
        case SYS_PROCESS_WATCH:   return sys_process_watch(arg0, arg1, arg2);
        case SYS_IRQ_ROUTE_REGISTER: return sys_irq_route_register(arg0, arg1, arg2);
        case SYS_IOPORT_IN:          return sys_ioport_in(arg0, arg1, arg2);
        case SYS_IOPORT_OUT:         return sys_ioport_out(arg0, arg1, arg2);
        /* SYS_CHAN_RECV_NB retired — Phase 13/Track G, fall to default (NOT_SUPPORTED) */
        case SYS_PROCESS_KILL:        return sys_process_kill(arg0, arg1, arg2);
        /* SYS_DIAG_SNAPSHOT(30) — retired, fall to default */
        /* SYS_CHAN_SEAL retired — Phase 13/Track G, fall to default (NOT_SUPPORTED) */
        /* SYS_CHAN_CALL(38) — retired Phase 13/Track G (zero callers), fall to default */
        case SYS_CAP_CREATE_IRQCAP:   return sys_cap_create_irqcap(arg0, arg1, arg2, arg3);
        case SYS_CAP_CREATE_IOPORT:   return sys_cap_create_ioport(arg0, arg1, arg2, arg3);
        /* SYS_INITRD_LOOKUP(41), SYS_SPAWN_ELF(42) — retired, fall to default */
        case SYS_IOPORT_RESTRICT:      return sys_ioport_restrict(arg0, arg1, arg2);
        /* SYS_WAIT_ANY(44) — retired Phase 13/Track G (zero callers), fall to default */
        case SYS_BOOTCAP_RESTRICT:     return sys_bootcap_restrict(arg0, arg1, arg2);
        case SYS_VMO_SHARE:            return sys_vmo_share(arg0, arg1, arg2);
        case SYS_EXCEPTION_HANDLER:    return sys_exception_handler(arg0, arg1, arg2, arg3);
        case SYS_THREAD_CREATE:        return sys_thread_create(arg0, arg1, arg2);
        case SYS_THREAD_EXIT:          return sys_thread_exit(arg0, arg1, arg2);
        case SYS_FUTEX_WAIT:           return sys_futex_wait(arg0, arg1, arg2);
        case SYS_FUTEX_WAKE:           return sys_futex_wake(arg0, arg1, arg2);
        case SYS_HANDLE_TYPE:          return sys_handle_type(arg0, arg1, arg2);
        case SYS_HANDLE_SAME_OBJECT:   return sys_handle_same_object(arg0, arg1, arg2);
        case SYS_POWEROFF:             return sys_poweroff(arg0, arg1, arg2);
        case SYS_INITRD_VMO:    return sys_initrd_vmo(arg0, arg1, arg2, arg3);
        case SYS_INITRD_COUNT:  return sys_initrd_count(arg0, arg1, arg2, arg3);
        case SYS_PROCESS_CREATE: return sys_process_create(arg0, arg1, arg2, arg3);
        case SYS_VMO_MAP_INTO:  return sys_vmo_map_into(arg0, arg1, arg2, arg3);
        case SYS_THREAD_START:  return sys_thread_start(arg0, arg1, arg2, arg3);
        case SYS_HANDLE_INSERT: return sys_handle_insert(arg0, arg1, arg2, arg3);
        case SYS_CLOCK_GET:           return sys_clock_get(arg0, arg1, arg2);
        /* SYS_CHAN_RECV_TIMEOUT retired — Phase 13/Track G, fall to default (NOT_SUPPORTED) */
        case SYS_NOTIFY_WAIT_TIMEOUT: return sys_notify_wait_timeout(arg0, arg1, arg2);
        case SYS_KLOG_DRAIN:          return sys_klog_drain(arg0, arg1, arg2);
        case SYS_EXCEPTION_RESUME:    return sys_exception_resume(arg0, arg1, arg2);
        case SYS_VMO_SIZE:            return sys_vmo_size(arg0, arg1, arg2);
        case SYS_IRQ_ACK:             return sys_irq_ack(arg0, arg1, arg2);
        case SYS_SCHED_INFO:          return sys_sched_info(arg0, arg1, arg2);
        case SYS_CLOCK_NANOSLEEP:     return sys_clock_nanosleep(arg0, arg1, arg2);
        case SYS_PROCESS_EXIT_CODE:   return sys_process_exit_code(arg0, arg1, arg2);
        case SYS_PROCESS_FAULT_INFO:  return sys_process_fault_info(arg0, arg1, arg2);
        /* SYS_WAIT_ANY_TIMEOUT(72) — retired Phase 13/Track G (zero callers), fall to default */
        case SYS_ENDPOINT_CREATE:     return sys_endpoint_create(arg0, arg1, arg2);
        case SYS_EP_SEND:             return sys_ep_send(arg0, arg1, arg2);
        case SYS_EP_RECV:             return sys_ep_recv(arg0, arg1, arg2);
        case SYS_EP_NB_SEND:          return sys_ep_nb_send(arg0, arg1, arg2);
        case SYS_EP_NB_RECV:          return sys_ep_nb_recv(arg0, arg1, arg2);
        case SYS_CAP_DERIVE:          return sys_cap_derive(arg0, arg1, arg2);
        case SYS_CAP_REVOKE:          return sys_cap_revoke(arg0, arg1, arg2);
        case SYS_CNODE_CREATE:        return sys_cnode_create(arg0, arg1, arg2);
        case SYS_CNODE_MINT:          return sys_cnode_mint(arg0, arg1, arg2, arg3);
        case SYS_THREAD_PRIORITY:     return sys_thread_priority(arg0, arg1, arg2);
        case SYS_SC_CREATE:           return sys_sc_create(arg0, arg1, arg2);
        case SYS_SC_CONFIGURE:        return sys_sc_configure(arg0, arg1, arg2);
        case SYS_THREAD_SET_SC:       return sys_thread_set_sc(arg0, arg1, arg2);
        case SYS_UNTYPED_INFO:        return sys_untyped_info(arg0, arg1, arg2);
        case SYS_UNTYPED_RETYPE:      return sys_untyped_retype(arg0, arg1, arg2);
        case SYS_UNTYPED_RESET:       return sys_untyped_reset(arg0, arg1, arg2);
        case SYS_CNODE_MOVE:          return sys_cnode_move(arg0, arg1, arg2);
        case SYS_CNODE_FETCH:         return sys_cnode_fetch(arg0, arg1, arg2);
        case SYS_CNODE_DELETE:        return sys_cnode_delete(arg0, arg1, arg2);
        case SYS_CNODE_SWAP:          return sys_cnode_swap(arg0, arg1, arg2);
        case SYS_EP_CALL:             return sys_ep_call(arg0, arg1, arg2);
        case SYS_REPLY:               return sys_reply(arg0, arg1, arg2);
        case SYS_CSPACE_RESOLVE:      return sys_cspace_resolve(arg0, arg1, arg2);
        case SYS_PROC_CSPACE_MINT:    return sys_proc_cspace_mint(arg0, arg1, arg2, arg3);
        case SYS_TCB_SELF:            return sys_tcb_self(arg0, arg1, arg2);
        case SYS_TCB_SUSPEND:         return sys_tcb_suspend(arg0, arg1, arg2);
        case SYS_TCB_RESUME:          return sys_tcb_resume(arg0, arg1, arg2);
        case SYS_TCB_SET_PRIORITY:    return sys_tcb_set_priority(arg0, arg1, arg2);
        case SYS_TCB_EXIT:            return sys_tcb_exit(arg0, arg1, arg2);
        case SYS_TCB_GET_INFO:        return sys_tcb_get_info(arg0, arg1, arg2);
        case SYS_FRAME_MAP:           return sys_frame_map(arg0, arg1, arg2, arg3);
        case SYS_FRAME_UNMAP:         return sys_frame_unmap(arg0, arg1, arg2);
        case SYS_VSPACE_SELF:         return sys_vspace_self(arg0, arg1, arg2);
        case SYS_PROCESS_VSPACE:      return sys_process_vspace(arg0, arg1, arg2);
        case SYS_VMO_MAP_PAGE:        return sys_vmo_map_page(arg0, arg1, arg2, arg3);
        case SYS_VMO_CREATE_FOR:      return sys_vmo_create_for(arg0, arg1, arg2, arg3);
        case SYS_RESOURCE_INFO:       return sys_resource_info(arg0, arg1, arg2);
        case SYS_UNTYPED_RETYPE2:     return sys_untyped_retype2(arg0, arg1, arg2, arg3);
        case SYS_UNTYPED_QUERY:       return sys_untyped_query(arg0, arg1, arg2);
        case SYS_SC_BIND:             return sys_sc_bind(arg0, arg1, arg2);
        /* Phase S3 — CSpace-only MDB/CDT derivation surface. */
        case SYS_CAP_IDENTIFY:        return sys_cap_identify(arg0, arg1, arg2);
        case SYS_CAP_SAME_OBJECT:     return sys_cap_same_object(arg0, arg1, arg2);
        /* Stage 5 Step 4: execution for a TCB retyped from an Untyped. */
        case SYS_CSPACE_SELF:         return sys_cspace_self(arg0, arg1, arg2);
        /* Stage 6-pure: a page table the holder retyped, installed by name. */
        case SYS_VSPACE_MAP_TABLE:    return sys_vspace_map_table(arg0, arg1, arg2);
        case SYS_TCB_FAULT_INFO:      return sys_tcb_fault_info(arg0, arg1, arg2);
        case SYS_TCB_WATCH:           return sys_tcb_watch(arg0, arg1, arg2);
        case SYS_TCB_SET_FAULT_HANDLER:
                                      return sys_tcb_set_fault_handler(arg0, arg1, arg2, arg3);
        case SYS_TCB_EXIT_CODE:       return sys_tcb_exit_code(arg0, arg1, arg2);
        case SYS_TCB_CONFIGURE:       return sys_tcb_configure(arg0, arg1, arg2, arg3);
        case SYS_TCB_WRITE_REGS:      return sys_tcb_write_regs(arg0, arg1, arg2, arg3);
        case SYS_CSPACE_MINT:         return sys_cspace_mint(arg0, arg1, arg2);
        case SYS_CSPACE_REVOKE:       return sys_cspace_revoke(arg0, arg1, arg2);
        case SYS_CSPACE_MINT_INTO:    return sys_cspace_mint_into(arg0, arg1, arg2, arg3);
        case SYS_CSPACE_SET_GUARD:    return sys_cspace_set_guard(arg0, arg1, arg2);
        case SYS_TCB_SET_TIMEOUT_HANDLER:
                                      return sys_tcb_set_timeout_handler(arg0, arg1, arg2, arg3);
        case SYS_TCB_SET_IPC_BUFFER:  return sys_tcb_set_ipc_buffer(arg0, arg1, arg2);
        case SYS_IOPORT_CONTROL_NARROW:
                                      return sys_ioport_control_narrow(arg0, arg1, arg2, arg3);
        case SYS_UNTYPED_SET_DEVICE_BUDGET:
                                      return sys_untyped_set_device_budget(arg0, arg1, arg2);
        case SYS_FRAMEBUFFER_INFO:    return sys_framebuffer_info(arg0, arg1, arg2);
        case SYS_REPLY_RECV:          return sys_reply_recv(arg0, arg1, arg2);
        default:
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

/* syscall_kstack_ptr / syscall_user_cr3: RIP-relative shadow globals in
 * syscall_entry.S .data, kept in sync for debug.  The live read path in
 * syscall_entry.S uses GS-relative access (cpu_local.syscall_kstack at %gs:48,
 * cpu_local.syscall_user_cr3 at %gs:56) after SWAPGS at syscall entry.
 *
 * SMP note: syscall_set_kstack / syscall_set_user_cr3 are always called from
 * task_yield() which runs post-SWAPGS.  cpu_self() here resolves to the current
 * CPU's block — correct for any CPU once per-CPU GS is initialized.
 */
extern uint64_t syscall_kstack_ptr;
extern uint64_t syscall_user_cr3;

void syscall_set_kstack(uint64_t kstack_top) {
    syscall_kstack_ptr = kstack_top;
    cpu_self()->syscall_kstack = kstack_top;
}

void syscall_set_user_cr3(uint64_t val) {
    syscall_user_cr3 = val;
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
