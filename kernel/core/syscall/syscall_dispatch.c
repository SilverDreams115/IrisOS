/*
 * syscall_dispatch.c — syscall infrastructure: MSR setup, dispatch table.
 *
 * Contains syscall_init (MSR wiring), syscall_set_kstack, and the
 * syscall_dispatch switch that routes each syscall number to its handler.
 * All sys_* implementations live in the syscall_*.c subsystem files.
 */
#include "syscall_priv.h"

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

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (num) {
        case SYS_WRITE:  return sys_write(arg0, arg1, arg2);
        case SYS_EXIT:   return sys_exit(arg0, arg1, arg2);
        case SYS_GETPID: return sys_getpid(arg0, arg1, arg2);
        case SYS_YIELD:  return sys_yield(arg0, arg1, arg2);
        case SYS_BRK:        return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 20 */
        case SYS_SLEEP:      return sys_sleep(arg0, arg1, arg2);
        case SYS_CHAN_CREATE:  return sys_chan_create(arg0, arg1, arg2);
        case SYS_CHAN_SEND:    return sys_chan_send(arg0, arg1, arg2);
        case SYS_CHAN_RECV:    return sys_chan_recv(arg0, arg1, arg2);
        case SYS_HANDLE_CLOSE: return sys_handle_close(arg0, arg1, arg2);
        case SYS_VMO_CREATE:  return sys_vmo_create(arg0, arg1, arg2);
        case SYS_VMO_MAP:     return sys_vmo_map(arg0, arg1, arg2);
        case SYS_VMO_UNMAP:   return sys_vmo_unmap(arg0, arg1, arg2);
        case SYS_SPAWN:         return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 19 */
        case SYS_SPAWN_SERVICE: return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 22 */
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
        case SYS_DIAG_SNAPSHOT:  return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 51 */
        case SYS_CHAN_SEAL:       return sys_chan_seal(arg0, arg1, arg2);
        case SYS_CHAN_CALL:            return sys_chan_call(arg0, arg1, arg2);
        case SYS_CAP_CREATE_IRQCAP:   return sys_cap_create_irqcap(arg0, arg1, arg2);
        case SYS_CAP_CREATE_IOPORT:   return sys_cap_create_ioport(arg0, arg1, arg2);
        case SYS_INITRD_LOOKUP: return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 29 */
        case SYS_SPAWN_ELF:     return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 29 */
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
        default:
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

/* syscall_kstack_ptr lives in syscall_entry.S .data section */
extern uint64_t syscall_kstack_ptr;

void syscall_set_kstack(uint64_t kstack_top) {
    syscall_kstack_ptr = kstack_top;
}

void syscall_init(void) {
    /* enable SCE bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= (1ULL << 0); /* SCE = syscall enable */
    wrmsr(MSR_EFER, efer);

    /* STAR: bits 63:48 = user CS-8 (sysret CS = this+16, SS = this+8)
     *       bits 47:32 = kernel CS (syscall CS, SS = this+8)
     * kernel CS = 0x08, kernel SS = 0x10
     * user   CS = 0x1B (0x18|3), user SS = 0x23 (0x20|3)
     * STAR[47:32] = 0x0008  (kernel: CS=0x08, SS=0x10)
     * STAR[63:48] = 0x0013  (sysret: CS=0x1B=0x13|3, SS=0x23=0x1B+8)
     */
    uint64_t star = 0;
    star |= ((uint64_t)0x0008 << 32); /* kernel CS selector */
    star |= ((uint64_t)0x0013 << 48); /* user CS-8 for sysret */
    wrmsr(MSR_STAR, star);

    /* LSTAR: syscall handler entry point */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* SFMASK: clear IF on syscall entry (disable interrupts) */
    wrmsr(MSR_SFMASK, (1ULL << 9)); /* IF = bit 9 */
}
