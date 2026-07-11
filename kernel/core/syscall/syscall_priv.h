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
#include <iris/nc/kbootcap.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kirqcap.h>
#include <iris/nc/kioport.h>
#include <iris/nc/kinitrdentry.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kschedctx.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kreply.h>
#include <iris/nc/ktcb.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
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
/* WAIT_ANY_MAX_CHANNELS retired with SYS_WAIT_ANY — Fase 13/Track G */

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

/* Fase 13/Track G: user_kchanmsg_* / copy_kchanmsg_* helpers retired with the
 * KChannel object. */

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
uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_self(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_status(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_kill(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_exit_code(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_fault_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_create(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_thread_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_thread_start(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — IPC ──────────────────────────────────── */
/* sys_chan_call retired — Fase 13/Track G */
uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_wait_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2);
/* sys_wait_any / sys_wait_any_timeout retired — Fase 13/Track G */
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
/* sys_handle_transfer retired — A1.8 (dispatcher falls to NOT_SUPPORTED). */
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

/* ── Forward declarations — endpoint IPC ─────────────────────────── */
uint64_t sys_endpoint_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ep_send(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ep_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ep_nb_send(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ep_nb_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — Block 7 reply caps (Ph85-87) ────────── */
uint64_t sys_ep_call(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_reply(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Shared IPC cap-transfer helpers (defined in syscall_endpoint.c) ──
 * Used by EP_SEND / EP_NB_SEND / EP_CALL and by SYS_REPLY (reply-cap
 * transfer).
 */
uint32_t syscall_ipc_deliver_cap(struct task *receiver,
                                 struct KObject *xo, uint32_t cap_rights);
/* A1.9/A1.10: two-phase staging — EVERY transfer path stages with peek
 * (validate + retain WITHOUT consuming the source handle) and consumes it
 * via commit only once delivery is committed (receiver determined).  Any
 * non-delivery exit — CLOSED, WOULD_BLOCK, endpoint close, waiter cancel,
 * lost one-shot reply race — releases the peeked ref only, so the source
 * cap always stays with its owner.  The single-shot consume-at-stage
 * wrappers were retired in A1.10 (zero callers; do not reintroduce). */
iris_error_t syscall_ipc_stage_cap_peek_badged(struct task *t, uint32_t src_h,
                                               uint32_t requested_rights,
                                               struct KObject **out_obj,
                                               uint32_t *out_rights,
                                               uint64_t *out_badge);
void syscall_ipc_stage_cap_commit(struct task *t, uint32_t src_h);
uint32_t syscall_ipc_deliver_cap_badged(struct task *receiver,
                                        struct KObject *xo,
                                        uint32_t cap_rights, uint64_t badge);
/* A1.5: receive-slot support (defined in syscall_endpoint.c).
 * _recv_slot_declare validates + records a receiver-declared CSpace slot
 * (fail-fast; endpoint untouched on error).  _deliver_cap_routed delivers a
 * staged cap into the receiver's declared slot when one is set — falling
 * back to handle materialization on a delivery-time race — and returns the
 * msg discriminator: 0 = no cap, <1024 = slot CPtr, >=1024 = handle. */
iris_error_t syscall_ipc_recv_slot_declare(struct task *t, uint32_t declared);
uint32_t syscall_ipc_deliver_cap_routed(struct task *receiver,
                                        struct KObject *xo,
                                        uint32_t cap_rights, uint64_t badge);

/* A1.7 diagnostic counters (relaxed atomics; diagnostic only — read by the
 * sys_sched_info extended layout).  slot/handle/toctou partition transferred
 * -cap deliveries; reply_caps counts KReply insertions; resolves counts
 * successful SYS_CSPACE_RESOLVE materializations. */
extern uint32_t iris_ipc_stat_slot_deliveries;    /* syscall_endpoint.c */
extern uint32_t iris_ipc_stat_handle_deliveries;
extern uint32_t iris_ipc_stat_toctou_fallbacks;
extern uint32_t iris_ipc_stat_reply_caps;
extern uint32_t iris_cspace_stat_resolves;        /* syscall_cspace.c */

/* ── Forward declarations — CSpace (Ph70-72, Ph82-84, Ph95) ─────── */
uint64_t sys_cap_derive(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cap_revoke(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cnode_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cnode_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_proc_cspace_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_cnode_move(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cnode_fetch(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cnode_delete(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cnode_swap(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cspace_resolve(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — Block 3 scheduler (Ph73-75) ─────────── */
uint64_t sys_thread_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sc_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sc_configure(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_thread_set_sc(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — Block 4+5 untyped memory (Ph76-81) ───── */
uint64_t sys_untyped_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_untyped_retype(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_untyped_reset(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — Block 9 frame capabilities (Fase 5 / 5.1) ── */
uint64_t sys_frame_map  (uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_frame_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vspace_self(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — TCB caps (Ph96-101) ──────────────────── */
uint64_t sys_tcb_self(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_suspend(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_set_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_get_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — diag ─────────────────────────────────── */
uint64_t sys_clock_get(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_clock_nanosleep(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_klog_drain(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_poweroff(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sched_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);

#endif /* IRIS_SYSCALL_PRIV_H */
