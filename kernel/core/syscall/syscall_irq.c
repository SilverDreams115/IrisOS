#include "syscall_priv.h"



/* ── IRQ route management ─────────────────────────────────────────── */

/*
 * sys_irq_route_register(irqcap_handle, chan_handle, proc_handle) → 0 or iris_error_t
 *
 * Routes the hardware IRQ line embedded in irqcap_handle into the KChannel
 * behind chan_handle.  The IRQ number is extracted from the KIrqCap object;
 * callers cannot supply an arbitrary vector — they are bound to whatever the
 * kernel encoded into the capability at boot.
 *
 * The route is owned by the KProcess behind proc_handle: when that process
 * exits, kprocess_teardown → irq_routing_unregister_owner clears the route
 * automatically.
 *
 * The irqcap_handle is not consumed — svcmgr may reuse it across service
 * restarts without requesting a new capability.
 *
 * Authority:
 *   irqcap_handle — KOBJ_IRQ_CAP with RIGHT_ROUTE.
 *   chan_handle   — KOBJ_CHANNEL with RIGHT_READ|RIGHT_WRITE.
 *   proc_handle   — KOBJ_PROCESS with RIGHT_READ|RIGHT_ROUTE.
 */
uint64_t sys_irq_route_register(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Resolve the IRQ capability — it carries the authorized IRQ number */
    struct KObject  *irqcap_obj;
    iris_rights_t    irqcap_rights;
    /* Phase 13: dual resolver — irqcap may be a CPtr slot or a handle. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_IRQ_CAP, &irqcap_obj, &irqcap_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(irqcap_rights, RIGHT_ROUTE)) {
        kobject_release(irqcap_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    uint8_t irq_num = ((struct KIrqCap *)irqcap_obj)->irq_num;
    kobject_release(irqcap_obj);

    if (irq_num >= IRQ_ROUTE_MAX) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Phase 13/Track G: the destination is a KNotification (signal route) — the
     * legacy KChannel message route is retired. */
    struct KObject  *ch_obj;
    iris_rights_t    ch_rights;
    /* Step 4: third occurrence of the same half-migration — the irqcap (arg0)
     * and the owning process (arg2) resolved either way while the destination
     * notification did not, so a service holding its IRQ notification in
     * CSpace could not register a route.  The dual object resolver returns a
     * lifecycle-only reference and reports WRONG_TYPE itself. */
    r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg1, RIGHT_NONE,
                                     KOBJ_NOTIFICATION, &ch_obj, &ch_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(ch_rights, RIGHT_WRITE)) {
        kobject_release(ch_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /*
     * Stage 7-mem: arg2 is RESERVED and ignored.
     *
     * It named the PROCESS that would own the route, and required
     * RIGHT_READ|RIGHT_ROUTE on it — a third party to an operation between an
     * interrupt line and a notification.  It existed so process teardown could
     * sweep the routes that process had installed; the binding belongs to the
     * notification now and is cleared when its last capability goes.
     *
     * What authorises a route is what it acts on, and both are already
     * checked: RIGHT_ROUTE on the IRQ capability (you hold the line) and
     * RIGHT_WRITE on the notification (you may signal it).  There is nothing a
     * process capability added except a fourth thing to hold.
     */
    (void)arg2;

    /* The routing table takes its own lifecycle reference on the destination. */
    irq_routing_register_notification(irq_num, (struct KNotification *)ch_obj);
    kobject_release(ch_obj);
    return syscall_err(IRIS_OK);
}


/* ── IRQ deferred ACK ─────────────────────────────────────────────── */

/*
 * sys_irq_ack(irqcap_handle) → 0 or iris_error_t
 *
 * Unmasks the hardware IRQ line embedded in irqcap_handle.  Ring-3 calls
 * this after reading the hardware register (e.g. PS/2 port 0x60) to re-enable
 * delivery of subsequent interrupts on the same line.
 *
 * Authority: irqcap_handle — KOBJ_IRQ_CAP with RIGHT_ROUTE.
 */
uint64_t sys_irq_ack(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_IRQ_CAP, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_ROUTE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    uint8_t irq_num = ((struct KIrqCap *)obj)->irq_num;
    kobject_release(obj);

    irq_routing_ack(irq_num);
    return syscall_ok_u64(0);
}


/* ── I/O port access via KIoPort capability ──────────────────────── */

/*
 * sys_ioport_in(ioport_handle, port_offset) → uint8_t value or iris_error_t
 *
 * Executes the x86 IN instruction for one byte from:
 *   cap->base_port + port_offset
 * Returns the byte in bits [7:0] of the result on success.
 * Requires RIGHT_READ on ioport_handle.
 */
uint64_t sys_ioport_in(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_IOPORT, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KIoPort *port = (struct KIoPort *)obj;
    uint32_t offset = (uint32_t)(arg1 & 0xFFFFu);
    if (offset >= (uint32_t)port->count) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint16_t io_port = (uint16_t)(port->base_port + (uint16_t)offset);
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(io_port));
    kobject_release(obj);
    return syscall_ok_u64((uint64_t)value);
}


/*
 * sys_ioport_out(ioport_handle, port_offset, value) → 0 or iris_error_t
 *
 * Executes the x86 OUT instruction, writing the low byte of value to:
 *   cap->base_port + port_offset
 * Requires RIGHT_WRITE on ioport_handle.
 */
uint64_t sys_ioport_out(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_IOPORT, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KIoPort *port = (struct KIoPort *)obj;
    uint32_t offset = (uint32_t)(arg1 & 0xFFFFu);
    if (offset >= (uint32_t)port->count) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint16_t io_port = (uint16_t)(port->base_port + (uint16_t)offset);
    uint8_t  value   = (uint8_t)(arg2 & 0xFFu);
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(io_port));
    kobject_release(obj);
    return syscall_ok_u64(0);
}


/* ── B5: exception handler registration ───────────────────────────── */

/*
 * SYS_EXCEPTION_HANDLER — RETIRED (Stage 7 Step 12).
 *
 * It armed a PROCESS: every thread in it faulted into one mailbox, one
 * notification, one set of signal bits, and a handler holding the registration
 * could not tell two executions apart except by reading an id out of the
 * record.  That is the shape the charter calls a global identifier standing in
 * for a capability, one level up — the process was standing in for the thread.
 *
 * SYS_TCB_SET_FAULT_HANDLER (126) arms the EXECUTION that takes the fault.
 * Two threads of one process can have two handlers, or one and none, because
 * whose faults go where is now said with a capability to the thread rather
 * than inferred from what it belongs to.  The destination packing, the
 * registrant-names-the-mailbox rule Step 7 established, and the delivered TCB
 * capability all carry over unchanged — only the object being armed moved.
 */
uint64_t sys_exception_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/* ── B6: exception resume ──────────────────────────────────────────── */

/*
 * SYS_EXCEPTION_RESUME(tcb_cptr, action) — answer a fault.
 *
 * Stage 7 Step 7: the faulting thread is named by CAPABILITY.  It used to be
 * (process capability, task id, action): authority came from the process cap
 * and the id was checked against it, so the number conferred nothing — but it
 * SELECTED, which charter §3.4/§3.5 forbid, and a supervisor could not hold,
 * delegate or revoke "that thread".  The capability arrives in the mailbox the
 * handler was registered with, so answering a fault now uses only things the
 * handler was handed.
 */
uint64_t sys_exception_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    (void)arg2;
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint32_t action = (uint32_t)arg1;
    /* Phase 25: actions 2 (resume) / 3 (kill) are the seq-checked variants —
     * bits [63:32] of arg2 carry the fault generation the caller observed via
     * SYS_PROCESS_FAULT_INFO.  Actions 0/1 keep the exact Phase 20 semantics
     * (any value > 1 was INVALID_ARG before, so this is additive surface). */
    if (action > 3) return syscall_err(IRIS_ERR_INVALID_ARG);
    int      seq_checked  = (action >= 2);
    uint32_t expected_seq = (uint32_t)(arg1 >> 32);
    action &= 1u;
    if (seq_checked && expected_seq == 0)
        return syscall_err(IRIS_ERR_INVALID_ARG);   /* 0 is never a valid generation */

    /* The TCB capability IS the authority: RIGHT_WRITE on a thread is what lets
     * you decide whether it runs again.  The process capability and its rights
     * check went with the id they qualified. */
    struct KObject *tcb_obj; iris_rights_t tcb_rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                             RIGHT_NONE, KOBJ_TCB,
                                             &tcb_obj, &tcb_rights);
    if (r != IRIS_OK)
        return syscall_err(r == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : r);
    if (!rights_check(tcb_rights, RIGHT_WRITE)) {
        kobject_release(tcb_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct task *ft = (struct task *)tcb_obj;
    /* Stage 7 Step 15: the fault, the record and its resolution are all the
     * thread's — there is no longer a process to hold across this. */
    if (ft->state != TASK_BLOCKED_FAULT) {
        kobject_release(tcb_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    /* Phase 25 (P13): a seq-checked resolution must name the exact fault the
     * caller observed.  If the task refaulted since (a NEW generation), or the
     * caller is replaying a generation it never matched, refuse cleanly —
     * same NOT_FOUND class as every other stale (process, task, state)
     * mismatch, with no side effect on the pending fault. */
    if (seq_checked && ft->fault_seq != expected_seq) {
        kobject_release(tcb_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    if (action == 0) {
        task_wakeup(ft);
    } else {
        task_kill_external(ft);
    }

    /* Phase 20: the fault is resolved — drop the pending-fault record so a
     * later SYS_TCB_FAULT_INFO honestly returns WOULD_BLOCK, and bump the
     * resume/kill counter. */
    kfault_resolve(ft, action == 1);

    kobject_release(tcb_obj);
    return syscall_ok_u64(0);
}
