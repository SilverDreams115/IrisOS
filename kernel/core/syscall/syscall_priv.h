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
#include <iris/kslab.h>
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
#include <iris/nc/kpagetable.h>
#include <iris/nc/ktcb.h>
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
/* WAIT_ANY_MAX_CHANNELS retired with SYS_WAIT_ANY — Phase 13/Track G */

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

/* Phase 13/Track G: user_kchanmsg_* / copy_kchanmsg_* helpers retired with the
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
 * Debug authority, named.  The caller must NAME the capability that
 * authorises the call — what a capability system requires, and what charter A5
 * (no ambient authority) and §3.5 (nothing but a capability confers authority)
 * demand.
 *
 * The ambient predecessor scanned the caller's whole handle table for any
 * KBootstrapCap carrying KDEBUG: the caller proved nothing and named nothing,
 * and a process whose bootstrap capability lived in CSpace was denied while
 * holding it.  Every in-tree caller of SYS_KLOG_DRAIN / SYS_SCHED_INFO /
 * SYS_POWEROFF names its CPtr now, so the scan is DELETED rather than kept as
 * a fallback — a fallback is how ambient authority survives a cleanup.
 */
static inline int task_kdebug_cap_named(struct task *t, uint64_t auth_cptr) {
    if (!t || !t->cspace_root) return 0;
    if (auth_cptr == 0u) return 0;   /* authority must be named */
    if (!cspace_value_is_cptr((iris_cptr_t)auth_cptr)) return 0;

    struct KObject *obj; iris_rights_t r;
    if (cspace_resolve_cap(t->cspace_root, (iris_cptr_t)auth_cptr, RIGHT_READ,
                           &obj, &r) != IRIS_OK) return 0;
    /* Stage 5 Step 2: exact match.  Debug authority is its own capability, so
     * a capability that merely includes the bit — which, before the split, was
     * every boot capability in the system — does not authorise draining the
     * kernel log or powering the machine off. */
    int ok = (obj->type == KOBJ_BOOTSTRAP_CAP) &&
             kbootcap_is((struct KBootstrapCap *)obj, IRIS_BOOTCAP_DEBUG_CONTROL);
    kobject_active_release(obj);
    kobject_release(obj);
    return ok;
}

/*
 * Whitelist of port ranges ring-3 services may claim via SYS_CAP_CREATE_IOPORT.
 * A request must fall entirely within one entry ([base, base+count)).
 * The kernel itself owns PIC/PIT; those are not listed here.
 */
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
uint64_t sys_tcb_fault_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_set_fault_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3);
uint64_t sys_tcb_exit_code(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_create(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_thread_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_thread_start(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — IPC ──────────────────────────────────── */
/* sys_chan_call retired — Phase 13/Track G */
uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_notify_wait_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2);
/* sys_wait_any / sys_wait_any_timeout retired — Phase 13/Track G */
uint64_t sys_futex_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_futex_wake(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — VM ───────────────────────────────────── */
uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_create_for(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3); /* Phase 29; Stage 7 Step 14: arg3 = budget */
uint64_t sys_resource_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);  /* Phase 29 */
uint64_t sys_vmo_map(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_frame_size(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vmo_map_into(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_vmo_share(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_initrd_count(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/* ── Forward declarations — cap / handle ─────────────────────────── */
uint64_t sys_handle_close(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_handle_dup(uint64_t arg0, uint64_t arg1, uint64_t arg2);
/* sys_handle_transfer retired — A1.8 (dispatcher falls to NOT_SUPPORTED). */
uint64_t sys_handle_insert(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_handle_type(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_handle_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2);
/* Caller's root CNode with active+lifecycle refs (defined in syscall_cspace.c). */
iris_error_t cspace_own_root(struct KCNode *root, struct KCNode **out);

/*
 * Publish a freshly created object into a CSpace slot of the CALLER's root
 * CNode, optionally as an MDB child of the slot that authorised it.
 *
 * Step 4: this is what replaces "return a handle".  A syscall that hands back
 * a handle is a handle PRODUCER, and the charter forbids new ones outright
 * (§3.1); retiring the existing ones means every creation lands in CSpace
 * instead.  Where the creation was authorised by a capability the caller
 * named, the result is parented to that slot, so the grant is revocable by
 * whoever granted the authority.
 *
 * Consumes the caller's reference to obj on every path.
 */
/* Resolve a destination CNode for publication.  CSpace only, deliberately: a
 * destination is a place to PUT authority, and naming it in the retiring
 * namespace would make this a new dual-namespace path — exactly what charter
 * §3.6 forbids as a pattern.  Yields active + lifecycle, like the traversal. */
static inline iris_error_t cspace_resolve_cnode_for_publish(struct KCNode *root,
                                                            iris_cptr_t cptr,
                                                            struct KCNode **out) {
    struct KObject *obj; iris_rights_t r;
    iris_error_t err = cspace_resolve_cap(root, cptr, RIGHT_WRITE, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_CNODE) {
        kobject_active_release(obj);
        kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    *out = (struct KCNode *)obj;
    return IRIS_OK;
}

static inline iris_error_t syscall_publish_slot(struct task *t,
                                                struct KObject *obj,
                                                iris_rights_t rights,
                                                uint64_t dest,
                                                struct KCNode *parent_cn,
                                                uint32_t parent_idx) {
    /* `dest` follows the RETYPE2 convention: destination CNode in the low 32
     * bits (0 = the caller's own root), destination slot index in the high 32.
     * A spawning service holds its working capabilities in a second-level
     * CNode, so "a slot" is not always a root slot. */
    uint64_t dest_cnode = dest & 0xFFFFFFFFu;
    uint32_t dest_slot  = (uint32_t)(dest >> 32);
    if (dest_slot == 0u) { kobject_release(obj); return IRIS_ERR_INVALID_ARG; }

    struct KCNode *cn = 0;
    iris_error_t err;
    if (dest_cnode == 0u) {
        err = cspace_own_root(t->cspace_root, &cn);
    } else {
        err = cspace_resolve_cnode_for_publish(t->cspace_root,
                                               (iris_cptr_t)dest_cnode, &cn);
    }
    if (err != IRIS_OK) { kobject_release(obj); return err; }

    err = kcnode_slot_install_linked(cn, dest_slot, obj, rights, 0,
                                     parent_cn, parent_idx,
                                     /*exclusive*/1,
                                     /*legacy*/parent_cn ? 0 : 1);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    kobject_release(obj);          /* the slot holds its own refs */
    return err;
}


uint64_t sys_cap_create_irqcap(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_cap_create_ioport(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_ioport_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_bootcap_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — IRQ / exception ──────────────────────── */
uint64_t sys_irq_route_register(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_irq_ack(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ioport_in(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ioport_out(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_exception_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3);
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
/* CSpace-only source guard: nonzero direct CPtr territory (<1024).  Shared by
 * the CSpace syscalls and the IPC transfer path — a handle value is never a
 * valid SOURCE for either (charter §3.6/§3.7). */
static inline int cspace_only_cptr(uint64_t v) {
    return cspace_value_is_cptr((iris_cptr_t)v);
}

/* A1.9/A1.10: two-phase staging — EVERY transfer path stages with peek
 * (validate + retain WITHOUT consuming the source handle) and consumes it
 * via commit only once delivery is committed (receiver determined).  Any
 * non-delivery exit — CLOSED, WOULD_BLOCK, endpoint close, waiter cancel,
 * lost one-shot reply race — releases the peeked ref only, so the source
 * cap always stays with its owner.  The single-shot consume-at-stage
 * wrappers were retired in A1.10 (zero callers; do not reintroduce).
 *
 * Phase S4 (Step 2): the SOURCE is a CSpace CPtr (<1024), resolved to its
 * terminal slot — never a handle.  The slot identity (out_src_cn/out_src_idx)
 * rides with the staged object so delivery can parent the delivered cap to it
 * in the MDB.  out_src_cn carries active+lifecycle refs; release them with
 * syscall_ipc_stage_cap_commit (delivery) or syscall_ipc_stage_cap_abort
 * (any non-delivery exit). */
iris_error_t syscall_ipc_stage_cap_peek_badged(struct task *t, uint32_t src_cptr,
                                               uint32_t requested_rights,
                                               struct KObject **out_obj,
                                               uint32_t *out_rights,
                                               uint64_t *out_badge,
                                               struct KCNode **out_src_cn,
                                               uint32_t *out_src_idx);
/* Delivery committed: consume the source slot (move semantics).  Children of
 * the deleted source — including the cap just delivered — are reparented to
 * its grandparent, so every surviving ancestor keeps revocation authority.
 * Releases the CNode refs taken by peek. */
void syscall_ipc_stage_cap_commit(struct task *t, struct KCNode *src_cn,
                                  uint32_t src_idx);
/* Non-delivery exit: release the CNode refs WITHOUT touching the slot. */
void syscall_ipc_stage_cap_abort(struct KCNode *src_cn);
/* A1.5: receive-slot support (defined in syscall_endpoint.c).
 * _recv_slot_declare validates + records a receiver-declared CSpace slot
 * (fail-fast; endpoint untouched on error).  _deliver_cap_routed installs a
 * staged cap into that slot and returns the msg discriminator: a CPtr on
 * success, 0 when no capability was delivered.
 *
 * Stage 4: there is no handle leg.  A receive that declared no slot, or whose
 * slot cannot be installed into, gets the MESSAGE without the capability —
 * charter I1's destination half, matching the fail-closed shape the raced
 * slot has had since Step 2.
 *
 * Phase S4 (Step 2): src_cn/src_idx are the sender's source slot.  A slot
 * delivery installs the cap as an MDB CHILD of that slot (real CSpace
 * ancestry) instead of a LEGACY_ROOT; the source must still be occupied at
 * delivery time, so a cap revoked while staged is never delivered. */
iris_error_t syscall_ipc_recv_slot_declare(struct task *t, uint32_t declared);
uint32_t syscall_ipc_deliver_cap_routed(struct task *receiver,
                                        struct KObject *xo,
                                        uint32_t cap_rights, uint64_t badge,
                                        struct KCNode *src_cn, uint32_t src_idx);

/* A1.7 diagnostic counters (relaxed atomics; diagnostic only — read by the
 * sys_sched_info extended layout).  slot/handle/toctou partition transferred
 * -cap deliveries; reply_caps counts KReply insertions; resolves counts
 * successful SYS_CSPACE_RESOLVE materializations. */
extern uint32_t iris_ipc_stat_slot_deliveries;    /* syscall_endpoint.c */
extern uint32_t iris_ipc_stat_handle_deliveries;
/* Phase S4 (Step 2): RETIRED — the TOCTOU slot→handle degradation is gone,
 * so this counter is a STRUCTURAL ZERO.  It stays in the ABI (sys_sched_info
 * extended layout, offset w[8]) as the retirement witness: T094 forces the
 * race and T095 asserts the counter never moves.  If it ever becomes
 * non-zero, a CPtr→handle fallback has been reintroduced (charter §3.7). */
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
/* Phase S3 — CSpace-only MDB/CDT derivation surface. */
uint64_t sys_cspace_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cap_identify(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cap_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2);
/* Stage 8-cap / D-2 — install a guard on a CNode capability. */
uint64_t sys_cspace_set_guard(uint64_t arg0, uint64_t arg1, uint64_t arg2);
/* Stage 8-mcs — arm a thread's timeout fault handler. */
uint64_t sys_tcb_set_ipc_buffer(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_ioport_control_narrow(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3);
uint64_t sys_untyped_set_device_budget(uint64_t arg0, uint64_t arg1,
                                       uint64_t arg2);
uint64_t sys_framebuffer_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_initrd_frame(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3);

/* D-4 — bulk payload routing, defined in syscall_endpoint.c and shared with
 * syscall_reply.c.  See the comment block there. */
uint8_t     *ipc_buf_kva(struct task *t);
uint32_t     ipc_buf_capacity(struct task *t);
iris_error_t ipc_stage_out(struct task *t);
void         ipc_transfer_bulk(struct task *sender, struct task *receiver,
                               int receiver_current);
void         ipc_transfer_reply(struct task *server, struct task *caller,
                                const struct IrisMsg *reply_msg);
uint64_t sys_tcb_set_timeout_handler(uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3);
/* Stage 8-mcs — atomic reply-then-receive (seL4_ReplyRecv). */
uint64_t sys_reply_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/*
 * Stage 9-evt Step 1 — ask to be re-executed (ledger D-1).
 *
 * A blocking handler parks the thread, calls this, and returns.  The
 * dispatcher reschedules and re-enters the SAME syscall with the SAME
 * arguments.  The handler must therefore keep NO live state across the block:
 * everything it needs on the second entry has to be in thread state, which is
 * the continuation an event kernel would have recorded explicitly.
 */
void syscall_request_restart(struct task *t);
/* Global count of syscall re-executions (diagnostic). */
uint32_t syscall_restart_count(void);
/* Syscalls that resumed on a FRESH stack — the abandonment gauge. */
uint32_t syscall_abandon_count(void);

/* Stage 9-evt Step 2 — return to ring 3 with no syscall frame (assembly). */
__attribute__((noreturn)) void syscall_return_to_user(uint64_t rax_value,
                                                      uint64_t user_rip,
                                                      uint64_t user_rflags,
                                                      uint64_t user_rsp,
                                                      const uint64_t *callee_saved);
/* Where an abandoned syscall resumes, on a fresh stack. */
__attribute__((noreturn)) void syscall_restart_trampoline(void);
/* Park the caller so it resumes at the trampoline, abandoning this frame.
 * Returns if it could not switch away — see syscall_run. */
__attribute__((noreturn)) void task_park_restart(void);

/*
 * Stage 9-evt / D-8 — capabilities revoked per preemptible slice.
 *
 * Chosen for the shape of the bound rather than for throughput: each victim
 * costs one mdb_lock acquisition plus a lifecycle release taken OUTSIDE that
 * lock, so a slice of 16 bounds the syscall's uninterrupted run at a small
 * multiple of a single delete.  Larger would blunt the preemption point;
 * smaller would pay a reschedule for almost no work.
 */
#define IRIS_REVOKE_SLICE 16u
uint64_t sys_cspace_self(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vspace_map_table(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_tcb_configure(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3);
uint64_t sys_tcb_write_regs(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3);
uint64_t sys_cspace_revoke(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_cspace_mint_into(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3);

/* ── Forward declarations — Block 3 scheduler (Ph73-75) ─────────── */
uint64_t sys_thread_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sc_create(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sc_configure(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_thread_set_sc(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_sc_bind(uint64_t arg0, uint64_t arg1, uint64_t arg2);         /* Phase S2 */

/* ── Forward declarations — Block 4+5 untyped memory (Ph76-81) ───── */
uint64_t sys_untyped_info(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_untyped_retype(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_untyped_retype2(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                             uint64_t arg3);                               /* Phase S1 */
uint64_t sys_untyped_query(uint64_t arg0, uint64_t arg1, uint64_t arg2);   /* Phase S1 */
uint64_t sys_untyped_reset(uint64_t arg0, uint64_t arg1, uint64_t arg2);

/* ── Forward declarations — Block 9 frame capabilities (Phase 5 / 5.1) ── */
uint64_t sys_frame_map  (uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t sys_frame_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_vspace_self(uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t sys_process_vspace(uint64_t arg0, uint64_t arg1, uint64_t arg2);  /* Phase 25 */
uint64_t sys_vmo_map_page(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);  /* Phase 26 */

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
