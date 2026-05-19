/*
 * syscall_priv.h — private header shared by all syscall subsystem TUs.
 *
 * Include this and nothing else at the top of each syscall_*.c file.
 * Contains: all kernel includes, PAGE_SIZE constant, shared helper
 * functions (static inline to suppress unused-function warnings), and
 * forward declarations for every sys_* handler.
 */
#ifndef IRIS_SYSCALL_PRIV_H
#define IRIS_SYSCALL_PRIV_H

#include <iris/syscall.h>
#include <iris/task.h>
#include <iris/pmm.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kirqcap.h>
#include <iris/nc/kioport.h>
#include <iris/nc/kinitrdentry.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <iris/futex.h>
#include <iris/initrd.h>
#include <iris/tsc.h>
#include <iris/klog.h>
#include <iris/fb_info.h>
#include <iris/paging.h>

#define PAGE_SIZE             0x1000ULL
#define WAIT_ANY_MAX_CHANNELS 64u

/* ── Shared helper functions ─────────────────────────────────────────
 * All static inline to avoid unused-function warnings when a given
 * subsystem TU does not call every helper.
 */

static inline uint64_t syscall_err(iris_error_t err) {
    return (uint64_t)(int64_t)err;
}

static inline uint64_t syscall_ok_u64(uint64_t value) {
    return value;
}

static inline int user_kchanmsg_readable(uint64_t uptr) {
    return user_range_readable(uptr, (uint32_t)sizeof(struct KChanMsg));
}

static inline int user_kchanmsg_writable(uint64_t uptr) {
    return user_range_writable(uptr, (uint32_t)sizeof(struct KChanMsg));
}

static inline int copy_kchanmsg_from_user(struct KChanMsg *dst, uint64_t src_uptr) {
    return copy_from_user_checked(dst, src_uptr, (uint32_t)sizeof(*dst));
}

static inline int copy_kchanmsg_to_user(uint64_t dst_uptr, const struct KChanMsg *src) {
    return copy_to_user_checked(dst_uptr, src, (uint32_t)sizeof(*src));
}

static inline int copy_u32_to_user_checked(uint64_t dst_uptr, uint32_t value) {
    return copy_to_user_checked(dst_uptr, &value, (uint32_t)sizeof(value));
}

static inline int copy_u64_to_user_checked(uint64_t dst_uptr, uint64_t value) {
    return copy_to_user_checked(dst_uptr, &value, (uint32_t)sizeof(value));
}

static inline int timeout_ns_to_deadline_ticks(uint64_t timeout_ns,
                                                uint64_t *out_deadline_ticks) {
    uint64_t now_ticks;
    uint64_t timeout_ticks;

    if (!out_deadline_ticks) return 0;
    now_ticks = sched_current_ticks();
    timeout_ticks = timeout_ns / 10000000ULL;
    if (timeout_ticks > UINT64_MAX - now_ticks - 1ULL) return 0;
    *out_deadline_ticks = now_ticks + timeout_ticks + 1ULL;
    return 1;
}

static inline int user_vmo_range_valid(uint64_t virt, uint64_t size) {
    uint64_t end;

    if (size == 0) return 0;
    if ((virt & (PAGE_SIZE - 1ULL)) != 0) return 0;
    if (virt < USER_VMO_BASE || virt >= USER_VMO_TOP) return 0;
    if (size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;

    size = (size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    end = virt + size;
    if (end < virt) return 0;
    if (end > USER_VMO_TOP) return 0;
    return 1;
}

static inline int user_private_range_valid(uint64_t virt, uint64_t size,
                                            uint64_t upper_bound) {
    uint64_t end;

    if (size == 0) return 0;
    if ((virt & (PAGE_SIZE - 1ULL)) != 0) return 0;
    if (virt < USER_PRIVATE_BASE || virt >= upper_bound) return 0;
    if (size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;

    size = (size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    end = virt + size;
    if (end < virt) return 0;
    if (end > upper_bound) return 0;
    return 1;
}

static inline int page_round_up_u64(uint64_t size, uint64_t *out_rounded) {
    uint64_t rounded;
    if (!out_rounded || size == 0) return 0;
    if (size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;
    rounded = (size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    if (rounded < size) return 0;
    *out_rounded = rounded;
    return 1;
}

static inline void rollback_user_maps(uint64_t cr3, uint64_t start, uint64_t end) {
    for (uint64_t virt = start; virt < end; virt += PAGE_SIZE)
        paging_unmap_in(cr3, virt);
}

/*
 * task_has_kdebug_cap — check whether task t holds a KBootstrapCap with
 * IRIS_BOOTCAP_KDEBUG.  Scans the handle table under its spinlock so that
 * no entry can be closed while the check runs.
 */
static inline int task_has_kdebug_cap(struct task *t) {
    if (!t || !t->process) return 0;
    HandleTable *ht = &t->process->handle_table;
    int found = 0;
    spinlock_lock(&ht->lock);
    for (uint32_t i = 0; i < HANDLE_TABLE_MAX && !found; i++) {
        if (!ht->used[i]) continue;
        struct KObject *obj = ht->slots[i].object;
        if (!obj || obj->type != KOBJ_BOOTSTRAP_CAP) continue;
        if (kbootcap_allows((struct KBootstrapCap *)obj, IRIS_BOOTCAP_KDEBUG))
            found = 1;
    }
    spinlock_unlock(&ht->lock);
    return found;
}

/*
 * Whitelist of port ranges ring-3 services may claim via SYS_CAP_CREATE_IOPORT.
 * A request must fall entirely within one entry ([base, base+count)).
 * The kernel itself owns PIC/PIT; those are not listed here.
 */
static const struct { uint16_t base; uint16_t count; } kioport_whitelist[] = {
    { 0x0060u, 5u },   /* PS/2: data(0x60) + status/cmd(0x64) */
    { 0x02F8u, 8u },   /* COM2 serial */
    { 0x03F8u, 8u },   /* COM1 serial */
    { 0x0600u, 8u },   /* QEMU ACPI power management (0x604 poweroff) */
};

static inline int kioport_in_whitelist(uint16_t base, uint16_t count) {
    uint32_t req_end = (uint32_t)base + (uint32_t)count;
    for (uint32_t i = 0; i < sizeof(kioport_whitelist)/sizeof(kioport_whitelist[0]); i++) {
        uint32_t wl_end = (uint32_t)kioport_whitelist[i].base +
                          (uint32_t)kioport_whitelist[i].count;
        if ((uint32_t)base >= (uint32_t)kioport_whitelist[i].base && req_end <= wl_end)
            return 1;
    }
    return 0;
}

/* ── Forward declarations — proc ─────────────────────────────────── */
uint64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_self(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_status(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_kill(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_exit_code(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_create(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_thread_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_thread_start(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — IPC ──────────────────────────────────── */
uint64_t sys_chan_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_chan_send(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_chan_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_chan_recv_nb(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_chan_seal(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_chan_call(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_chan_recv_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_wait_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_wait_any(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_wait_any_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_futex_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_futex_wake(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — VM ───────────────────────────────────── */
uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_map(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_size(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_map_into(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_vmo_share(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_framebuffer_vmo(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_initrd_vmo(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_initrd_count(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/* ── Forward declarations — cap / handle ─────────────────────────── */
uint64_t sys_handle_close(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_handle_dup(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_handle_transfer(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_handle_insert(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_handle_type(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_handle_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cap_create_irqcap(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cap_create_ioport(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ioport_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_bootcap_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — IRQ / exception ──────────────────────── */
uint64_t sys_irq_route_register(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_irq_ack(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ioport_in(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ioport_out(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_exception_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_exception_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — diag ─────────────────────────────────── */
uint64_t sys_clock_get(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_clock_nanosleep(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_klog_drain(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_poweroff(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sched_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);

#endif /* IRIS_SYSCALL_PRIV_H */
