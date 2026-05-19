#include "syscall_priv.h"
#include <iris/cpu_local.h>



/*
 * sys_clock_get() → uint64_t nanoseconds since boot or iris_error_t
 *
 * Returns a monotonically increasing timestamp derived from the scheduler tick
 * counter (100 Hz; 10 ms resolution).  No capability required — any task may
 * query.  Does not block.
 */
uint64_t sys_clock_get(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    if (tsc_hz == 0)
        return syscall_ok_u64(sched_wall_ticks() * 10000000ULL);
    uint64_t delta = iris_rdtsc() - tsc_boot;
    uint64_t sec   = delta / tsc_hz;
    uint64_t rem   = delta % tsc_hz;
    return syscall_ok_u64(sec * 1000000000ULL + rem * 1000000000ULL / tsc_hz);
}


uint64_t sys_klog_drain(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);
    uint64_t max = arg1;
    if (max == 0u || max > (uint64_t)KLOG_BUF_SIZE)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg0, (uint32_t)max))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    uint32_t n;
    const char *buf = klog_get_buf(&n);
    if ((uint64_t)n > max) n = (uint32_t)max;
    if (n == 0u) return syscall_ok_u64(0);
    if (!copy_to_user_checked(arg0, buf, n))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    klog_clear();
    return syscall_ok_u64((uint64_t)n);
}


/*
 * sys_sched_info — scheduler diagnostic snapshot.  Requires KDEBUG cap.
 *
 * Copies a fixed-layout struct into the caller's buffer:
 *   offset  0: uint64_t ticks           — scheduler tick counter
 *   offset  8: uint64_t wall_ticks      — real-time tick counter
 *   offset 16: uint64_t context_switches
 *   offset 24: uint64_t idle_ticks
 *   offset 32: uint32_t live_task_count
 *   offset 36: uint32_t _pad
 * Total: 40 bytes.
 */
uint64_t sys_sched_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);
    if (arg1 < 40u) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg0, 40u)) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t buf[5];
    buf[0] = sched_current_ticks();
    buf[1] = sched_wall_ticks();
    buf[2] = sched_context_switches();
    buf[3] = sched_idle_ticks();
    uint32_t live = sched_live_task_count();
    uint32_t pad  = 0;
    buf[4] = (uint64_t)live | ((uint64_t)pad << 32);

    if (!copy_to_user_checked(arg0, buf, 40u))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}


/*
 * sys_poweroff — privileged machine halt.  Requires KDEBUG cap.
 *
 * Hardware power-off (ACPI S5, ISA debug exit) is intentionally NOT performed
 * here.  Ring-3 processes that need ACPI shutdown create a KIoPort cap for
 * 0x0604 via SYS_CAP_CREATE_IOPORT and issue the write themselves via
 * SYS_IOPORT_OUT.  The kernel owns no ACPI state after early boot.
 *
 * arg0 is reserved for future use (ignored).
 */
uint64_t sys_poweroff(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);
    for (;;) __asm__ volatile ("hlt");
    return syscall_ok_u64(0); /* unreachable */
}
