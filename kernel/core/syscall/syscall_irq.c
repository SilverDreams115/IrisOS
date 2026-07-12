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
    /* Fase 13: dual resolver — irqcap may be a CPtr slot or a handle. */
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_IRQ_CAP, &irqcap_obj, &irqcap_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(irqcap_rights, RIGHT_ROUTE)) {
        kobject_release(irqcap_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    uint8_t irq_num = ((struct KIrqCap *)irqcap_obj)->irq_num;
    kobject_release(irqcap_obj);

    if (irq_num >= IRQ_ROUTE_MAX) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Fase 13/Track G: the destination is a KNotification (signal route) — the
     * legacy KChannel message route is retired. */
    struct KObject  *ch_obj;
    iris_rights_t    ch_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &ch_obj, &ch_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (ch_obj->type != KOBJ_NOTIFICATION) {
        kobject_release(ch_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(ch_rights, RIGHT_WRITE)) {
        kobject_release(ch_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Resolve and validate the process cap (will own the route).
     * A1 Increment 2a: dual resolver — CPtr slot or handle. */
    struct KObject  *proc_obj;
    iris_rights_t    proc_rights;
    r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg2,
                                     RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (r != IRIS_OK) {
        kobject_release(ch_obj);
        return syscall_err(r);
    }
    if (!rights_check(proc_rights, RIGHT_READ | RIGHT_ROUTE)) {
        kobject_release(ch_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* irq_routing_register retains the destination on its own; proc is an
     * unretained owner pointer — safe because kprocess_teardown calls
     * unregister_owner before the process object is freed. */
    irq_routing_register_notification(irq_num, (struct KNotification *)ch_obj,
                                      (struct KProcess *)proc_obj);
    kobject_release(ch_obj);
    kobject_release(proc_obj);
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
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
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
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
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
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
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

uint64_t sys_exception_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KProcess *target_proc;
    struct KObject  *proc_obj = 0;

    if ((handle_id_t)arg0 == HANDLE_INVALID) {
        /* HANDLE_INVALID = self: no rights check needed */
        target_proc = t->process;
        kobject_retain(&target_proc->base);
    } else {
        iris_rights_t proc_rights;
        /* A1 Increment 2a: dual resolver on the non-self process.  The self
         * path above owns arg0 == 0 (HANDLE_INVALID == CPTR_NULL). */
        iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                     RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
        if (r != IRIS_OK) return syscall_err(r);
        if (!rights_check(proc_rights, RIGHT_MANAGE)) {
            kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        target_proc = (struct KProcess *)proc_obj;
    }

    /* Fase 13 (Track I): the handler is a KNotification (arg1) signalled with
     * signal_bits (arg2) on fault — not a KChannel.  The handler reads the fault
     * details via SYS_PROCESS_FAULT_INFO. */
    struct KObject *notif_obj;
    iris_rights_t notif_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg1, &notif_obj, &notif_rights);
    if (r != IRIS_OK) { kobject_release(&target_proc->base); return syscall_err(r); }
    if (notif_obj->type != KOBJ_NOTIFICATION) {
        kobject_release(&target_proc->base);
        kobject_release(notif_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(notif_rights, RIGHT_WRITE)) {
        kobject_release(&target_proc->base);
        kobject_release(notif_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    r = kprocess_set_exception_handler(target_proc,
                                       (struct KNotification *)notif_obj, arg2);
    kobject_release(&target_proc->base);
    kobject_release(notif_obj);
    return syscall_err(r);
}


/* ── B6: exception resume ──────────────────────────────────────────── */

uint64_t sys_exception_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint32_t target_id = (uint32_t)arg1;
    uint32_t action    = (uint32_t)arg2;
    if (action > 1) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KProcess *target_proc;
    struct KObject  *proc_obj = 0;

    if ((handle_id_t)arg0 == HANDLE_INVALID) {
        target_proc = t->process;
        kobject_retain(&target_proc->base);
    } else {
        iris_rights_t proc_rights;
        /* A1 Increment 2a: dual resolver on the non-self process.  The self
         * path above owns arg0 == 0 (HANDLE_INVALID == CPTR_NULL). */
        iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                     RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
        if (r != IRIS_OK) return syscall_err(r);
        if (!rights_check(proc_rights, RIGHT_MANAGE)) {
            kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        target_proc = (struct KProcess *)proc_obj;
    }

    struct task *ft = task_find_by_id(target_id);
    if (!ft || ft->process != target_proc || ft->state != TASK_BLOCKED_FAULT) {
        kobject_release(&target_proc->base);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    if (action == 0) {
        task_wakeup(ft);
    } else {
        task_kill_external(ft);
    }

    /* Fase 20: the fault is resolved — drop the pending-fault record so a later
     * SYS_PROCESS_FAULT_INFO honestly returns WOULD_BLOCK, and bump the
     * resume/kill counter. */
    kprocess_fault_clear(target_proc, target_id, action == 1);

    kobject_release(&target_proc->base);
    return syscall_ok_u64(0);
}
