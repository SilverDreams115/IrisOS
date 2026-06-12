/*
 * syscall_dispatch.c — syscall infrastructure: MSR setup, dispatch table.
 *
 * Contains syscall_init (MSR wiring), syscall_set_kstack, and the
 * syscall_dispatch switch that routes each syscall number to its handler.
 * All sys_* implementations live in the syscall_*.c subsystem files.
 */
#include "syscall_priv.h"
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
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (num) {
        /* SYS_WRITE(0), SYS_BRK(7) — retired, fall to default */
        case SYS_GETPID: return sys_getpid(arg0, arg1, arg2);
        case SYS_EXIT:  return sys_exit(arg0, arg1, arg2);
        case SYS_YIELD: return sys_yield(arg0, arg1, arg2);
        case SYS_SLEEP: return sys_sleep(arg0, arg1, arg2);
        case SYS_CHAN_CREATE:  return sys_chan_create(arg0, arg1, arg2);
        case SYS_CHAN_SEND:    return sys_chan_send(arg0, arg1, arg2);
        case SYS_CHAN_RECV:    return sys_chan_recv(arg0, arg1, arg2);
        case SYS_HANDLE_CLOSE: return sys_handle_close(arg0, arg1, arg2);
        case SYS_VMO_CREATE:  return sys_vmo_create(arg0, arg1, arg2);
        case SYS_VMO_MAP:     return sys_vmo_map(arg0, arg1, arg2);
        case SYS_VMO_UNMAP:   return sys_vmo_unmap(arg0, arg1, arg2);
        /* SYS_SPAWN(18), SYS_SPAWN_SERVICE(31) — retired, fall to default */
        case SYS_NOTIFY_CREATE: return sys_notify_create(arg0, arg1, arg2);
        case SYS_NOTIFY_SIGNAL: return sys_notify_signal(arg0, arg1, arg2);
        case SYS_NOTIFY_WAIT:   return sys_notify_wait(arg0, arg1, arg2);
        case SYS_HANDLE_DUP:    return sys_handle_dup(arg0, arg1, arg2);
        case SYS_HANDLE_TRANSFER: return sys_handle_transfer(arg0, arg1, arg2);
        case SYS_PROCESS_SELF:    return sys_process_self(arg0, arg1, arg2);
        case SYS_PROCESS_STATUS:  return sys_process_status(arg0, arg1, arg2);
        case SYS_PROCESS_WATCH:   return sys_process_watch(arg0, arg1, arg2);
        case SYS_IRQ_ROUTE_REGISTER: return sys_irq_route_register(arg0, arg1, arg2);
        case SYS_IOPORT_IN:          return sys_ioport_in(arg0, arg1, arg2);
        case SYS_IOPORT_OUT:         return sys_ioport_out(arg0, arg1, arg2);
        case SYS_CHAN_RECV_NB:        return sys_chan_recv_nb(arg0, arg1, arg2);
        case SYS_PROCESS_KILL:        return sys_process_kill(arg0, arg1, arg2);
        /* SYS_DIAG_SNAPSHOT(30) — retired, fall to default */
        case SYS_CHAN_SEAL:       return sys_chan_seal(arg0, arg1, arg2);
        case SYS_CHAN_CALL:            return sys_chan_call(arg0, arg1, arg2);
        case SYS_CAP_CREATE_IRQCAP:   return sys_cap_create_irqcap(arg0, arg1, arg2);
        case SYS_CAP_CREATE_IOPORT:   return sys_cap_create_ioport(arg0, arg1, arg2);
        /* SYS_INITRD_LOOKUP(41), SYS_SPAWN_ELF(42) — retired, fall to default */
        case SYS_IOPORT_RESTRICT:      return sys_ioport_restrict(arg0, arg1, arg2);
        case SYS_WAIT_ANY:             return sys_wait_any(arg0, arg1, arg2);
        case SYS_BOOTCAP_RESTRICT:     return sys_bootcap_restrict(arg0, arg1, arg2);
        case SYS_VMO_SHARE:            return sys_vmo_share(arg0, arg1, arg2);
        case SYS_EXCEPTION_HANDLER:    return sys_exception_handler(arg0, arg1, arg2);
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
        case SYS_FRAMEBUFFER_VMO: return sys_framebuffer_vmo(arg0, arg1, arg2, arg3);
        case SYS_CLOCK_GET:           return sys_clock_get(arg0, arg1, arg2);
        case SYS_CHAN_RECV_TIMEOUT:   return sys_chan_recv_timeout(arg0, arg1, arg2);
        case SYS_NOTIFY_WAIT_TIMEOUT: return sys_notify_wait_timeout(arg0, arg1, arg2);
        case SYS_KLOG_DRAIN:          return sys_klog_drain(arg0, arg1, arg2);
        case SYS_EXCEPTION_RESUME:    return sys_exception_resume(arg0, arg1, arg2);
        case SYS_VMO_SIZE:            return sys_vmo_size(arg0, arg1, arg2);
        case SYS_IRQ_ACK:             return sys_irq_ack(arg0, arg1, arg2);
        case SYS_SCHED_INFO:          return sys_sched_info(arg0, arg1, arg2);
        case SYS_CLOCK_NANOSLEEP:     return sys_clock_nanosleep(arg0, arg1, arg2);
        case SYS_PROCESS_EXIT_CODE:   return sys_process_exit_code(arg0, arg1, arg2);
        case SYS_WAIT_ANY_TIMEOUT:    return sys_wait_any_timeout(arg0, arg1, arg2, arg3);
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
        case SYS_TCB_SELF:            return sys_tcb_self(arg0, arg1, arg2);
        case SYS_TCB_SUSPEND:         return sys_tcb_suspend(arg0, arg1, arg2);
        case SYS_TCB_RESUME:          return sys_tcb_resume(arg0, arg1, arg2);
        case SYS_TCB_SET_PRIORITY:    return sys_tcb_set_priority(arg0, arg1, arg2);
        case SYS_TCB_EXIT:            return sys_tcb_exit(arg0, arg1, arg2);
        case SYS_TCB_GET_INFO:        return sys_tcb_get_info(arg0, arg1, arg2);
        case SYS_FRAME_MAP:           return sys_frame_map(arg0, arg1, arg2, arg3);
        case SYS_FRAME_UNMAP:         return sys_frame_unmap(arg0, arg1, arg2);
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
