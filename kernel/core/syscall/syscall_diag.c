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


uint64_t sys_clock_nanosleep(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    if (arg0 == 0) return syscall_ok_u64(0);
    uint64_t ticks = arg0 / 10000000ULL;
    if (ticks == 0) ticks = 1;
    scheduler_sleep_current(ticks);
    return syscall_ok_u64(0);
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
 * Base total: 40 bytes.
 *
 * A1.7 additive extension — written ONLY when the caller passes
 * buf_size >= 88 (a legacy caller passing 40..87 gets the exact historical
 * 40 bytes; no signature or number change — same additive style as the
 * A1.5 message-field reinterpretation):
 *   offset 40: uint32_t self_handles_live     — caller's handle table
 *   offset 44: uint32_t self_handles_hwm
 *   offset 48: uint32_t self_handle_inserts   — cumulative
 *   offset 52: uint32_t self_handle_removes
 *   offset 56: uint32_t handle_global_hwm     — max hwm across ALL tables
 *   offset 60: uint32_t handle_table_max      — HANDLE_TABLE_MAX ceiling
 *   offset 64: uint32_t ipc_cap_slot_deliveries    — receive-slot installs
 *   offset 68: uint32_t ipc_cap_handle_deliveries  — handle materializations
 *   offset 72: uint32_t ipc_cap_toctou_fallbacks   — declared-slot races
 *   offset 76: uint32_t reply_caps_created
 *   offset 80: uint32_t cspace_resolves
 *   offset 84: uint32_t _pad0
 * Extended total: 88 bytes.
 */
#define SCHED_INFO_BASE_BYTES 40u
#define SCHED_INFO_EXT_BYTES  88u

uint64_t sys_sched_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);
    if (arg1 < SCHED_INFO_BASE_BYTES) return syscall_err(IRIS_ERR_INVALID_ARG);
    uint32_t want = (arg1 >= SCHED_INFO_EXT_BYTES) ? SCHED_INFO_EXT_BYTES
                                                   : SCHED_INFO_BASE_BYTES;
    if (!user_range_writable(arg0, want)) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t buf[11];
    buf[0] = sched_current_ticks();
    buf[1] = sched_wall_ticks();
    buf[2] = sched_context_switches();
    buf[3] = sched_idle_ticks();
    uint32_t live = sched_live_task_count();
    uint32_t pad  = 0;
    buf[4] = (uint64_t)live | ((uint64_t)pad << 32);

    if (want >= SCHED_INFO_EXT_BYTES) {
        HandleTable *ht = &t->process->handle_table;
        uint32_t w[12];
        spinlock_lock(&ht->lock);
        w[0] = ht->live;
        w[1] = ht->hwm;
        w[2] = ht->inserts;
        w[3] = ht->removes;
        spinlock_unlock(&ht->lock);
        w[4]  = __atomic_load_n(&handle_table_global_hwm, __ATOMIC_RELAXED);
        w[5]  = HANDLE_TABLE_MAX;
        w[6]  = __atomic_load_n(&iris_ipc_stat_slot_deliveries, __ATOMIC_RELAXED);
        w[7]  = __atomic_load_n(&iris_ipc_stat_handle_deliveries, __ATOMIC_RELAXED);
        w[8]  = __atomic_load_n(&iris_ipc_stat_toctou_fallbacks, __ATOMIC_RELAXED);
        w[9]  = __atomic_load_n(&iris_ipc_stat_reply_caps, __ATOMIC_RELAXED);
        w[10] = __atomic_load_n(&iris_cspace_stat_resolves, __ATOMIC_RELAXED);
        w[11] = 0u;
        for (uint32_t i = 0; i < 6u; i++)
            buf[5u + i] = (uint64_t)w[2u * i] | ((uint64_t)w[2u * i + 1u] << 32);
    }

    if (!copy_to_user_checked(arg0, buf, want))
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
