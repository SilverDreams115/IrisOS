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
 *   offset 84: uint32_t live_process_count    — Fase 16 (KProcess objects live)
 *   offset 88: uint32_t reap_queue_hwm        — Fase 16 (deferred-reap depth hwm)
 *   offset 92: uint32_t _pad0
 * Extended total: 96 bytes.
 *
 * A caller passing 88..95 still gets the historical 88-byte snapshot; only a
 * buffer >= 96 receives the Fase 16 lifecycle words (same additive rule).
 *
 * Fase 17 additive scheduler-hardening tier — written ONLY when the caller
 * passes buf_size >= 112 (a caller passing 96..111 gets the exact historical
 * 96-byte snapshot; same additive rule as every tier above):
 *   offset  96: uint32_t run_queue_hwm            — Fase 17 (run-queue depth hwm)
 *   offset 100: uint32_t duplicate_enqueue_count  — Fase 17 (S4 guard trips)
 *   offset 104: uint32_t sched_ctx_live           — Fase 17 (KSchedContext live)
 *   offset 108: uint32_t yield_count              — Fase 17 (task_yield entries)
 * Extended-2 total: 112 bytes.
 *
 * Fase 18 additive authority tier — written ONLY when the caller passes
 * buf_size >= 136 (a caller passing 112..135 gets the exact historical 112-byte
 * snapshot; same additive rule as every tier above).  Live per-type object
 * counts let authority tests prove objects are born and destroyed exactly once:
 *   offset 112: uint32_t untyped_live      — Fase 18 (KUntyped objects live)
 *   offset 116: uint32_t frame_live        — Fase 18 (KFrame objects live)
 *   offset 120: uint32_t endpoint_live     — Fase 18 (KEndpoint objects live)
 *   offset 124: uint32_t notification_live — Fase 18 (KNotification objects live)
 *   offset 128: uint32_t cnode_live        — Fase 18 (KCNode objects live)
 *   offset 132: uint32_t _pad1
 * Extended-3 total: 136 bytes.
 *
 * Fase 19 additive VM/VSpace tier — written ONLY when the caller passes
 * buf_size >= 160 (a caller passing 136..159 gets the exact 136-byte snapshot;
 * same additive rule as every tier above):
 *   offset 136: uint32_t vspace_live        — Fase 19 (KVSpace objects live)
 *   offset 140: uint32_t live_mapping_count  — Fase 19 (KFrameMapping nodes live)
 *   offset 144: uint32_t map_success_count   — Fase 19 (successful maps, cumulative)
 *   offset 148: uint32_t unmap_success_count — Fase 19 (explicit unmaps, cumulative)
 *   offset 152: uint32_t tlb_invalidate_count— Fase 19 (local invlpg, cumulative)
 *   offset 156: uint32_t _pad2
 * Extended-4 total: 160 bytes.
 *
 * Fase 20 additive fault-model tier — written ONLY when the caller passes
 * buf_size >= 184 (a caller passing 160..183 gets the exact 160-byte snapshot;
 * same additive rule as every tier above):
 *   offset 160: uint32_t fault_delivery_count  — Fase 20 (faults handed to a handler)
 *   offset 164: uint32_t fault_nohandler_count — Fase 20 (faults with no handler → kill)
 *   offset 168: uint32_t fault_resume_count    — Fase 20 (EXCEPTION_RESUME action 0)
 *   offset 172: uint32_t fault_kill_count      — Fase 20 (EXCEPTION_RESUME action 1)
 *   offset 176: uint32_t fault_cleanup_count   — Fase 20 (pending faults cleared)
 *   offset 180: uint32_t _pad3
 * Extended-5 total: 184 bytes.
 */
#define SCHED_INFO_BASE_BYTES 40u
#define SCHED_INFO_EXT_BYTES  96u
#define SCHED_INFO_EXT2_BYTES 112u
#define SCHED_INFO_EXT3_BYTES 136u
#define SCHED_INFO_EXT4_BYTES 160u
#define SCHED_INFO_EXT5_BYTES 184u

uint64_t sys_sched_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);
    if (arg1 < SCHED_INFO_BASE_BYTES) return syscall_err(IRIS_ERR_INVALID_ARG);
    uint32_t want;
    if      (arg1 >= SCHED_INFO_EXT5_BYTES) want = SCHED_INFO_EXT5_BYTES;
    else if (arg1 >= SCHED_INFO_EXT4_BYTES) want = SCHED_INFO_EXT4_BYTES;
    else if (arg1 >= SCHED_INFO_EXT3_BYTES) want = SCHED_INFO_EXT3_BYTES;
    else if (arg1 >= SCHED_INFO_EXT2_BYTES) want = SCHED_INFO_EXT2_BYTES;
    else if (arg1 >= SCHED_INFO_EXT_BYTES)  want = SCHED_INFO_EXT_BYTES;
    else                                    want = SCHED_INFO_BASE_BYTES;
    if (!user_range_writable(arg0, want)) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t buf[23];
    buf[0] = sched_current_ticks();
    buf[1] = sched_wall_ticks();
    buf[2] = sched_context_switches();
    buf[3] = sched_idle_ticks();
    uint32_t live = sched_live_task_count();
    uint32_t pad  = 0;
    buf[4] = (uint64_t)live | ((uint64_t)pad << 32);

    if (want >= SCHED_INFO_EXT_BYTES) {
        HandleTable *ht = &t->process->handle_table;
        uint32_t w[14];
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
        w[11] = kprocess_live_count();          /* Fase 16 */
        w[12] = sched_reap_queue_hwm();          /* Fase 16 */
        w[13] = 0u;
        for (uint32_t i = 0; i < 7u; i++)
            buf[5u + i] = (uint64_t)w[2u * i] | ((uint64_t)w[2u * i + 1u] << 32);
    }

    if (want >= SCHED_INFO_EXT2_BYTES) {
        /* Fase 17 scheduler-hardening words (offsets 96..108). */
        uint32_t s0 = sched_run_queue_hwm();
        uint32_t s1 = sched_duplicate_enqueue_count();
        uint32_t s2 = kschedctx_live_count();
        uint32_t s3 = sched_yield_count();
        buf[12] = (uint64_t)s0 | ((uint64_t)s1 << 32);
        buf[13] = (uint64_t)s2 | ((uint64_t)s3 << 32);
    }

    if (want >= SCHED_INFO_EXT3_BYTES) {
        /* Fase 18 authority words (offsets 112..132): live per-type counts. */
        uint32_t a0 = kuntyped_live_count();
        uint32_t a1 = kframe_live_count();
        uint32_t a2 = kendpoint_live_count();
        uint32_t a3 = knotification_live_count();
        uint32_t a4 = kcnode_live_count();
        uint32_t a5 = kvmo_live_count();   /* Fase 26: live memory objects
                                            * (offset 132, was _pad1 = 0 —
                                            * additive, no new tier) */
        buf[14] = (uint64_t)a0 | ((uint64_t)a1 << 32);
        buf[15] = (uint64_t)a2 | ((uint64_t)a3 << 32);
        buf[16] = (uint64_t)a4 | ((uint64_t)a5 << 32);
    }

    if (want >= SCHED_INFO_EXT4_BYTES) {
        /* Fase 19 VM/VSpace words (offsets 136..156). */
        uint32_t v0 = kvspace_live_count();
        uint32_t v1 = kframe_live_mapping_count();
        uint32_t v2 = kframe_map_success_count();
        uint32_t v3 = kframe_unmap_success_count();
        uint32_t v4 = paging_tlb_invalidate_count();
        buf[17] = (uint64_t)v0 | ((uint64_t)v1 << 32);
        buf[18] = (uint64_t)v2 | ((uint64_t)v3 << 32);
        buf[19] = (uint64_t)v4;   /* high half = _pad2 (0) */
    }

    if (want >= SCHED_INFO_EXT5_BYTES) {
        /* Fase 20 fault-model words (offsets 160..176). */
        uint32_t g0 = kprocess_fault_delivery_count();
        uint32_t g1 = kprocess_fault_nohandler_count();
        uint32_t g2 = kprocess_fault_resume_count();
        uint32_t g3 = kprocess_fault_kill_count();
        uint32_t g4 = kprocess_fault_cleanup_count();
        buf[20] = (uint64_t)g0 | ((uint64_t)g1 << 32);
        buf[21] = (uint64_t)g2 | ((uint64_t)g3 << 32);
        buf[22] = (uint64_t)g4;   /* high half = _pad3 (0) */
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
