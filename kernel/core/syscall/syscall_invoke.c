/* SPDX-License-Identifier: Apache-2.0 */
/*
 * syscall_invoke.c — the invocation door (ledger A-32).
 *
 * ONE syscall reaches every method in the kernel: `SYS_INVOKE(cptr, label,
 * a1, a2, a3)`.  The capability says WHAT is being acted on, the label says
 * WHICH method, and neither can be given without the other.  That is the whole
 * of the form change; the numbered table this replaces named a method and left
 * the object to be an argument like any other.
 *
 * WHY THIS IS A FLAT SWITCH AND NOT A TYPE SWITCH
 *
 * The first cut resolved the capability here to learn its type, then switched
 * on (type, label), because labels were scoped per type.  Two things sent that
 * back.
 *
 * The first is that it is not seL4's arrangement: `enum invocation_label` is
 * one flat list, and seL4's type switch exists to reach a decoder that knows
 * how to READ the arguments — not to disambiguate a label that would otherwise
 * be ambiguous.
 *
 * The second is specific to this kernel and is the stronger reason.  Resolving
 * here means a CSpace walk that the method then repeats when it fetches the
 * capability with the type and rights it actually needs — two walks per
 * invocation, forever.  And the repeat cannot be removed by handing the method
 * the object this function resolved, because under the event kernel (D-1)
 * WHEN a method resolves is part of its contract:
 *
 *   - `sys_notify_wait` checks for a notification that CLOSED under it before
 *     resolving, because the close is usually the last capability going away
 *     and a resolve would report NOT_FOUND for something that actually closed;
 *   - `sys_tcb_suspend` returns on re-entry before resolving, because a
 *     restarted suspend that resolved and suspended again would put the thread
 *     straight back to sleep the instant it was resumed;
 *   - `sys_ep_send`, `sys_ep_recv`, `sys_ep_call` and `sys_reply_recv` do the
 *     same, and between them they are the entire hot path.
 *
 * Six of the seven methods that act before resolving are the blocking ones.
 * Hoisting the resolve would break precisely those, for an architectural
 * reason rather than an incidental one, so it is not scheduled: it is refused,
 * and A-32 records why.
 *
 * So WHERE is the type checked?  Where it always was — inside the method, by
 * the resolver that fetches the capability with the type it requires.  A label
 * sent to the wrong kind of capability answers `IRIS_ERR_WRONG_TYPE`, which
 * names what is wrong, and which the kernel only became able to say
 * consistently at A-30.  seL4 answers `IllegalOperation` for the same mistake.
 */
#include "syscall_priv.h"
#include <iris/invoke.h>

uint64_t syscall_invoke(uint64_t cptr, uint64_t label,
                        uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6,
                        uint64_t a7) {
    /* a4, a5 and a7 are message words (A-33).  The IPC methods read the
     * message through the THREAD rather than through this switch, which would
     * otherwise carry nine arguments for the benefit of seven of its sixty
     * cases — and which a restart could not reproduce anyway.  a6 is the one
     * exception, because ReplyRecv needs its reply object before it has
     * touched the message at all. */
    (void)a4; (void)a5; (void)a7;
    switch (label) {
    /* ── KOBJ_TCB ─────────────────────────────────────────────────────── */
    case INV_TCB_SUSPEND:              return sys_tcb_suspend(cptr, a1, a2);
    case INV_TCB_RESUME:               return sys_tcb_resume(cptr, a1, a2);
    case INV_TCB_SET_PRIORITY:         return sys_tcb_set_priority(cptr, a1, a2);
    case INV_TCB_SET_MCPRIORITY:       return sys_tcb_set_mcpriority(cptr, a1, a2);
    case INV_TCB_EXIT:                 return sys_tcb_exit(cptr, a1, a2);
    case INV_TCB_GET_INFO:             return sys_tcb_get_info(cptr, a1, a2);
    case INV_TCB_READ_REGS:            return sys_tcb_read_regs(cptr, a1, a2);
    case INV_TCB_WRITE_REGS:           return sys_tcb_write_regs(cptr, a1, a2, a3);
    case INV_TCB_CONFIGURE:            return sys_tcb_configure(cptr, a1, a2, a3);
    case INV_TCB_WATCH:                return sys_tcb_watch(cptr, a1, a2);
    case INV_TCB_SET_FAULT_HANDLER:    return sys_tcb_set_fault_handler(cptr, a1, a2, a3);
    case INV_TCB_SET_TIMEOUT_HANDLER:  return sys_tcb_set_timeout_handler(cptr, a1, a2, a3);
    case INV_TCB_EXIT_CODE:            return sys_tcb_exit_code(cptr, a1, a2);
    case INV_TCB_SET_IPC_BUFFER:       return sys_tcb_set_ipc_buffer(cptr, a1, a2);
    case INV_TCB_BIND_NOTIFICATION:    return sys_tcb_bind_notification(cptr, a1, a2);

    /* ── KOBJ_ENDPOINT ────────────────────────────────────────────────── */
    case INV_EP_SEND:                  return sys_ep_send(cptr, a1, a2);
    case INV_EP_NB_SEND:               return sys_ep_nb_send(cptr, a1, a2);
    case INV_EP_RECV:                  return sys_ep_recv(cptr, a1, a2);
    case INV_EP_NB_RECV:               return sys_ep_nb_recv(cptr, a1, a2);
    case INV_EP_CALL:                  return sys_ep_call(cptr, a1, a2);
    case INV_EP_CANCEL_BADGED_SENDS:   return sys_ep_cancel_badged_sends(cptr, a1, a2);
    /* ReplyRecv is invoked on the ENDPOINT, which is seL4's arrangement and
     * not the numbered door's: `sys_reply_recv` takes the reply capability
     * first and the endpoint last, because it grew out of SYS_REPLY.  The
     * order is rearranged here rather than in the function, so that adopting
     * seL4's way of NAMING the operation changes nothing it does. */
    /* A-33: the reply object rides in the capability word (a6), because a
     * reply carries no capability and the syscall enforces that itself. */
    case INV_EP_REPLY_RECV:            return sys_reply_recv(a6, 0, cptr);

    /* ── KOBJ_NOTIFICATION ────────────────────────────────────────────── */
    case INV_NOTIFY_SIGNAL:            return sys_notify_signal(cptr, a1, a2);
    case INV_NOTIFY_WAIT:              return sys_notify_wait(cptr, a1, a2);
    case INV_NOTIFY_POLL:              return sys_notify_poll(cptr, a1, a2);

    /* ── KOBJ_REPLY ───────────────────────────────────────────────────── */
    case INV_REPLY_SEND:               return sys_reply(cptr, a1, a2);

    /* ── KOBJ_UNTYPED ─────────────────────────────────────────────────── */
    case INV_UNTYPED_INFO:             return sys_untyped_info(cptr, a1, a2);
    case INV_UNTYPED_QUERY:            return sys_untyped_query(cptr, a1, a2);
    case INV_UNTYPED_RESET:            return sys_untyped_reset(cptr, a1, a2);
    case INV_UNTYPED_RETYPE:           return sys_untyped_retype2(cptr, a1, a2, a3);
    case INV_UNTYPED_SET_DEVICE_BUDGET: return sys_untyped_set_device_budget(cptr, a1, a2);

    /* ── KOBJ_CNODE ───────────────────────────────────────────────────── */
    case INV_CNODE_DELETE:             return sys_cnode_delete(cptr, a1, a2);
    case INV_CNODE_SWAP:               return sys_cnode_swap(cptr, a1, a2);

    /* ── KOBJ_SCHED_CONTEXT ───────────────────────────────────────────── */
    case INV_SC_BIND:                  return sys_sc_bind(cptr, a1, a2);
    case INV_SC_CONSUMED:              return sys_sc_consumed(cptr, a1, a2);
    case INV_SC_YIELD_TO:              return sys_sc_yield_to(cptr, a1, a2);
    case INV_SC_CONFIGURE:             return sys_sc_configure(cptr, a1, a2, a3);
    case INV_DOMAIN_SET:               return sys_domain_set(cptr, a1, a2);
    case INV_SC_SET_ON_CALLER:         return sys_thread_set_sc(cptr, a1, a2);

    /* ── KOBJ_FRAME ───────────────────────────────────────────────────── */
    case INV_FRAME_MAP:                return sys_frame_map(cptr, a1, a2, a3);
    case INV_FRAME_UNMAP:              return sys_frame_unmap(cptr, a1, a2);
    case INV_FRAME_SIZE:               return sys_frame_size(cptr, a1, a2);
    case INV_FRAME_GET_ADDRESS:        return sys_frame_get_address(cptr, a1, a2);

    /* ── KOBJ_IOSPACE — what a DEVICE may reach (Stage 10-dma) ────────── */
    case INV_IOSPACE_BIND:             return sys_iospace_bind(cptr, a1, a2);
    case INV_IOSPACE_MAP_TABLE:        return sys_iospace_map_table(cptr, a1, a2);
    case INV_IOSPACE_MAP_FRAME:        return sys_iospace_map_frame(cptr, a1, a2, a3);
    case INV_IOSPACE_UNMAP:            return sys_iospace_unmap(cptr, a1, a2);
    case INV_IOSPACE_FAULT:            return sys_iospace_fault(cptr, a1, a2, a3);
    case INV_IOPORT_IN16:              return sys_ioport_in16(cptr, a1, a2);
    case INV_IOPORT_OUT16:             return sys_ioport_out16(cptr, a1, a2);
    case INV_IOPORT_IN32:              return sys_ioport_in32(cptr, a1, a2);
    case INV_IOPORT_OUT32:             return sys_ioport_out32(cptr, a1, a2);

    /* ── KOBJ_PAGE_TABLE / KOBJ_ASID_POOL ─────────────────────────────── */
    case INV_PAGE_TABLE_MAP:           return sys_vspace_map_table(cptr, a1, a2);
    case INV_PAGE_TABLE_UNMAP:         return sys_vspace_unmap_table(cptr, a1, a2);
    case INV_ASID_POOL_ASSIGN:         return sys_asid_pool_assign(cptr, a1, a2);

    /* ── KOBJ_IRQ_CAP ─────────────────────────────────────────────────── */
    case INV_IRQ_SET_NOTIFICATION:     return sys_irq_route_register(cptr, a1, a2);
    case INV_IRQ_ACK:                  return sys_irq_ack(cptr, a1, a2);
    case INV_IRQ_CLEAR:                return sys_irq_clear(cptr, a1, a2);

    /* ── KOBJ_IOPORT ──────────────────────────────────────────────────── */
    case INV_IOPORT_IN:                return sys_ioport_in(cptr, a1, a2);
    case INV_IOPORT_OUT:               return sys_ioport_out(cptr, a1, a2);

    /* ── KOBJ_BOOTSTRAP_CAP ───────────────────────────────────────────── */
    case INV_BOOT_FRAMEBUFFER_INFO:    return sys_framebuffer_info(cptr, a1, a2);
    case INV_BOOT_INITRD_COUNT:        return sys_initrd_count(cptr, a1, a2, a3);
    case INV_BOOT_INITRD_FRAME:        return sys_initrd_frame(cptr, a1, a2, a3);
    case INV_BOOT_IOPORT_NARROW:       return sys_ioport_control_narrow(cptr, a1, a2, a3);
    case INV_BOOT_CREATE_IOPORT:       return sys_cap_create_ioport(cptr, a1, a2, a3);
    case INV_BOOT_CREATE_IRQCAP:       return sys_cap_create_irqcap(cptr, a1, a2, a3);
    /* The debug family names its authority LAST in the numbered ABI, because
     * the capability was bolted onto calls that used to be ambient.  Invoked,
     * the authority comes first, which is where an invocation always puts it. */
    case INV_BOOT_KLOG_DRAIN:          return sys_klog_drain(a1, a2, cptr);
    case INV_BOOT_SCHED_INFO:          return sys_sched_info(a1, a2, cptr);
    case INV_BOOT_POWEROFF:            return sys_poweroff(cptr, a1, a2);

    /* ── the slot, whatever it holds ──────────────────────────────────── */
    case INV_CAP_IDENTIFY:             return sys_cap_identify(cptr, a1, a2);
    case INV_CAP_SAME_OBJECT:          return sys_cap_same_object(cptr, a1, a2);
    case INV_CSPACE_MINT:              return sys_cspace_mint(cptr, a1, a2);
    case INV_CSPACE_MOVE:              return sys_cspace_move(cptr, a1, a2);
    case INV_CSPACE_ROTATE:            return sys_cspace_rotate(cptr, a1, a2);
    case INV_CSPACE_REVOKE:            return sys_cspace_revoke(cptr, a1, a2);
    case INV_CSPACE_SET_GUARD:         return sys_cspace_set_guard(cptr, a1, a2);

    default:
        /* A label that names no method.  seL4 answers IllegalOperation; IRIS
         * says the same thing with the code it already uses for an operation
         * that does not exist.  A label sent to the wrong KIND of capability
         * does not land here — it reaches the method, which refuses it by type
         * (A-30) and says so. */
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}
