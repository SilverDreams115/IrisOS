/*
 * syscall_invoke.c — the invocation door (ledger A-31, stage A).
 *
 * ONE syscall reaches every method in the kernel: `SYS_INVOKE(cptr, label,
 * a1, a2, a3)`.  It resolves the capability, switches on its TYPE, and then
 * switches on the LABEL — which is `decodeInvocation`, and which is the shape
 * seL4 has and IRIS did not.
 *
 * What this does NOT change, deliberately, is any authority semantics.  Every
 * method below is the same function the numbered door calls, with the same
 * arguments in the same order, and each still resolves its own capability and
 * checks its own rights.  That double resolve is the cost of stage A and the
 * subject of stage B, where the type check moves up here for good and roughly
 * sixty function bodies lose their opening lines.  Doing it in two steps is
 * what lets both doors stay open while ring 3 migrates one caller at a time.
 *
 * The type switch is not decoration.  Under a per-type label space (invoke.h)
 * the pair names the method and the number alone does not, so a label sent to
 * the wrong kind of capability MUST NOT land on the method that number happens
 * to mean there.  It lands on WRONG_TYPE — which is the answer A-30 made the
 * kernel able to give — or on NOT_SUPPORTED when the type has no such method.
 */
#include "syscall_priv.h"
#include <iris/invoke.h>

/*
 * The three arguments after the label, and why there are exactly three.
 *
 * `Untyped_Retype`, `TCB_Configure` and `Frame_Map` are the widest operations
 * in the kernel and each takes three arguments besides the object it acts on.
 * The syscall entry grew a fifth register (A-31) to carry them; nothing here
 * needs to know that, which is the point of measuring the width once.
 */

/* Methods on a KOBJ_TCB. */
static uint64_t inv_tcb(uint64_t c, uint64_t label,
                        uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (label) {
    case INV_TCB_SUSPEND:              return sys_tcb_suspend(c, a1, a2);
    case INV_TCB_RESUME:               return sys_tcb_resume(c, a1, a2);
    case INV_TCB_SET_PRIORITY:         return sys_tcb_set_priority(c, a1, a2);
    case INV_TCB_EXIT:                 return sys_tcb_exit(c, a1, a2);
    case INV_TCB_GET_INFO:             return sys_tcb_get_info(c, a1, a2);
    case INV_TCB_READ_REGS:            return sys_tcb_read_regs(c, a1, a2);
    case INV_TCB_WRITE_REGS:           return sys_tcb_write_regs(c, a1, a2, a3);
    case INV_TCB_CONFIGURE:            return sys_tcb_configure(c, a1, a2, a3);
    case INV_TCB_WATCH:                return sys_tcb_watch(c, a1, a2);
    case INV_TCB_SET_FAULT_HANDLER:    return sys_tcb_set_fault_handler(c, a1, a2, a3);
    case INV_TCB_SET_TIMEOUT_HANDLER:  return sys_tcb_set_timeout_handler(c, a1, a2, a3);
    case INV_TCB_EXIT_CODE:            return sys_tcb_exit_code(c, a1, a2);
    case INV_TCB_SET_IPC_BUFFER:       return sys_tcb_set_ipc_buffer(c, a1, a2);
    case INV_TCB_BIND_NOTIFICATION:    return sys_tcb_bind_notification(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

/* Methods on a KOBJ_ENDPOINT.
 *
 * ReplyRecv is invoked on the ENDPOINT, which is seL4's arrangement and not
 * the numbered door's: `sys_reply_recv` takes the reply capability first and
 * the endpoint last, because it grew out of SYS_REPLY.  The argument order is
 * rearranged HERE rather than in the function, so stage A changes no
 * behaviour — the conversation is identical, only the thing being invoked is
 * named the way seL4 names it. */
static uint64_t inv_endpoint(uint64_t c, uint64_t label,
                             uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    switch (label) {
    case INV_EP_SEND:                  return sys_ep_send(c, a1, a2);
    case INV_EP_NB_SEND:               return sys_ep_nb_send(c, a1, a2);
    case INV_EP_RECV:                  return sys_ep_recv(c, a1, a2);
    case INV_EP_NB_RECV:               return sys_ep_nb_recv(c, a1, a2);
    case INV_EP_CALL:                  return sys_ep_call(c, a1, a2);
    case INV_EP_CANCEL_BADGED_SENDS:   return sys_ep_cancel_badged_sends(c, a1, a2);
    case INV_EP_REPLY_RECV:            return sys_reply_recv(a1, a2, c);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_notification(uint64_t c, uint64_t label,
                                 uint64_t a1, uint64_t a2) {
    switch (label) {
    case INV_NOTIFY_SIGNAL:            return sys_notify_signal(c, a1, a2);
    case INV_NOTIFY_WAIT:              return sys_notify_wait(c, a1, a2);
    case INV_NOTIFY_POLL:              return sys_notify_poll(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_reply(uint64_t c, uint64_t label,
                          uint64_t a1, uint64_t a2) {
    switch (label) {
    case INV_REPLY_SEND:               return sys_reply(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_untyped(uint64_t c, uint64_t label,
                            uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (label) {
    case INV_UNTYPED_INFO:             return sys_untyped_info(c, a1, a2);
    case INV_UNTYPED_QUERY:            return sys_untyped_query(c, a1, a2);
    case INV_UNTYPED_RESET:            return sys_untyped_reset(c, a1, a2);
    case INV_UNTYPED_RETYPE:           return sys_untyped_retype2(c, a1, a2, a3);
    case INV_UNTYPED_SET_DEVICE_BUDGET: return sys_untyped_set_device_budget(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_cnode(uint64_t c, uint64_t label,
                          uint64_t a1, uint64_t a2) {
    switch (label) {
    case INV_CNODE_DELETE:             return sys_cnode_delete(c, a1, a2);
    case INV_CNODE_SWAP:               return sys_cnode_swap(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_sched_context(uint64_t c, uint64_t label,
                                  uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (label) {
    case INV_SC_BIND:                  return sys_sc_bind(c, a1, a2);
    case INV_SC_CONSUMED:              return sys_sc_consumed(c, a1, a2);
    case INV_SC_YIELD_TO:              return sys_sc_yield_to(c, a1, a2);
    case INV_SC_CONFIGURE:             return sys_sc_configure(c, a1, a2, a3);
    case INV_SC_SET_ON_CALLER:         return sys_thread_set_sc(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_frame(uint64_t c, uint64_t label,
                          uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (label) {
    case INV_FRAME_MAP:                return sys_frame_map(c, a1, a2, a3);
    case INV_FRAME_UNMAP:              return sys_frame_unmap(c, a1, a2);
    case INV_FRAME_SIZE:               return sys_frame_size(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

static uint64_t inv_bootstrap(uint64_t c, uint64_t label,
                              uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (label) {
    case INV_BOOT_FRAMEBUFFER_INFO:    return sys_framebuffer_info(c, a1, a2);
    case INV_BOOT_INITRD_COUNT:        return sys_initrd_count(c, a1, a2, a3);
    case INV_BOOT_INITRD_FRAME:        return sys_initrd_frame(c, a1, a2, a3);
    case INV_BOOT_IOPORT_NARROW:       return sys_ioport_control_narrow(c, a1, a2, a3);
    case INV_BOOT_CREATE_IOPORT:       return sys_cap_create_ioport(c, a1, a2, a3);
    case INV_BOOT_CREATE_IRQCAP:       return sys_cap_create_irqcap(c, a1, a2, a3);
    /* The debug family names its authority LAST in the numbered ABI, because
     * the capability was bolted onto calls that used to be ambient.  Invoked,
     * the authority comes first, which is where an invocation always puts it. */
    case INV_BOOT_KLOG_DRAIN:          return sys_klog_drain(a1, a2, c);
    case INV_BOOT_SCHED_INFO:          return sys_sched_info(a1, a2, c);
    case INV_BOOT_POWEROFF:            return sys_poweroff(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

/*
 * The methods that act on the SLOT rather than on what is in it.
 *
 * No type switch reaches these and none should: "copy this capability" means
 * the same thing whatever the capability is, which is exactly why they are a
 * separate range rather than repeated under every type.  They also skip the
 * resolve below — each of these already resolves the slot itself, and a slot
 * that does not resolve is the error the method returns anyway.
 */
static uint64_t inv_generic(uint64_t c, uint64_t label,
                            uint64_t a1, uint64_t a2) {
    switch (label) {
    case INV_CAP_IDENTIFY:             return sys_cap_identify(c, a1, a2);
    case INV_CAP_SAME_OBJECT:          return sys_cap_same_object(c, a1, a2);
    case INV_CSPACE_MINT:              return sys_cspace_mint(c, a1, a2);
    case INV_CSPACE_MOVE:              return sys_cspace_move(c, a1, a2);
    case INV_CSPACE_REVOKE:            return sys_cspace_revoke(c, a1, a2);
    case INV_CSPACE_SET_GUARD:         return sys_cspace_set_guard(c, a1, a2);
    default:                           return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

uint64_t syscall_invoke(uint64_t cptr, uint64_t label,
                        uint64_t a1, uint64_t a2, uint64_t a3) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (label >= INV_GENERIC_BASE) return inv_generic(cptr, label, a1, a2);

    if (!cspace_only_cptr(cptr)) return syscall_err(IRIS_ERR_INVALID_ARG);

    /*
     * Resolve to learn the TYPE, and nothing else.  RIGHT_NONE on purpose: an
     * invocation's rights are the method's business, and checking them twice
     * would mean two places that must agree about what each method needs.
     * What this establishes is only that the capability exists and what kind
     * it is — which is precisely what choosing the method requires.
     */
    struct KObject *obj; iris_rights_t rights;
    iris_error_t err = cspace_resolve_cap(t->cspace_root, (iris_cptr_t)cptr,
                                          RIGHT_NONE, &obj, &rights);
    if (err != IRIS_OK) return syscall_err(err);
    uint32_t type = obj->type;
    kobject_active_release(obj);
    kobject_release(obj);

    switch (type) {
    case KOBJ_TCB:            return inv_tcb(cptr, label, a1, a2, a3);
    case KOBJ_ENDPOINT:       return inv_endpoint(cptr, label, a1, a2, a3);
    case KOBJ_NOTIFICATION:   return inv_notification(cptr, label, a1, a2);
    case KOBJ_REPLY:          return inv_reply(cptr, label, a1, a2);
    case KOBJ_UNTYPED:        return inv_untyped(cptr, label, a1, a2, a3);
    case KOBJ_CNODE:          return inv_cnode(cptr, label, a1, a2);
    case KOBJ_SCHED_CONTEXT:  return inv_sched_context(cptr, label, a1, a2, a3);
    case KOBJ_FRAME:          return inv_frame(cptr, label, a1, a2, a3);
    case KOBJ_BOOTSTRAP_CAP:  return inv_bootstrap(cptr, label, a1, a2, a3);
    case KOBJ_PAGE_TABLE:
        return (label == INV_PAGE_TABLE_MAP)
             ? sys_vspace_map_table(cptr, a1, a2)
             : syscall_err(IRIS_ERR_NOT_SUPPORTED);
    case KOBJ_ASID_POOL:
        return (label == INV_ASID_POOL_ASSIGN)
             ? sys_asid_pool_assign(cptr, a1, a2)
             : syscall_err(IRIS_ERR_NOT_SUPPORTED);
    case KOBJ_IRQ_CAP:
        switch (label) {
        case INV_IRQ_SET_NOTIFICATION: return sys_irq_route_register(cptr, a1, a2);
        case INV_IRQ_ACK:              return sys_irq_ack(cptr, a1, a2);
        case INV_IRQ_CLEAR:            return sys_irq_clear(cptr, a1, a2);
        default:                       return syscall_err(IRIS_ERR_NOT_SUPPORTED);
        }
    case KOBJ_IOPORT:
        switch (label) {
        case INV_IOPORT_IN:            return sys_ioport_in(cptr, a1, a2);
        case INV_IOPORT_OUT:           return sys_ioport_out(cptr, a1, a2);
        default:                       return syscall_err(IRIS_ERR_NOT_SUPPORTED);
        }
    default:
        /* A real capability of a kind that has no invocations at all.  seL4
         * answers IllegalOperation; IRIS says the same thing with the code it
         * already uses for "this operation does not exist". */
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}
