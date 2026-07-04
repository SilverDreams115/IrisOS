/*
 * syscall_tcb.c — Block 8 (Ph96-101): TCB capability syscalls.
 *
 * SYS_TCB_SELF:          return a handle to the calling thread's KTcb.
 * SYS_TCB_SUSPEND:       transition target thread to TASK_SUSPENDED.
 * SYS_TCB_RESUME:        wake a TASK_SUSPENDED thread.
 * SYS_TCB_SET_PRIORITY:  change a thread's scheduling priority.
 * SYS_TCB_EXIT:          forcibly terminate a thread.
 * SYS_TCB_GET_INFO:      copy struct iris_tcb_info to userland.
 */
#include "syscall_priv.h"

uint64_t sys_tcb_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process || !t->ktcb)
        return syscall_err(IRIS_ERR_NOT_FOUND);

    handle_id_t h = handle_table_insert(
        &t->process->handle_table, &t->ktcb->base,
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

uint64_t sys_tcb_suspend(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t tcb_h = (handle_id_t)arg0;
    (void)arg1; (void)arg2;

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the TCB may be a CPtr slot or a handle.
     * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
    iris_error_t err = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)tcb_h,
                                 RIGHT_NONE, KOBJ_TCB, &obj, &rights);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(rights, RIGHT_WRITE)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KTcb *tcb = (struct KTcb *)obj;
    uint64_t flags = irq_spinlock_lock(&tcb->lock);
    struct task *target = tcb->task;
    irq_spinlock_unlock(&tcb->lock, flags);
    kobject_release(obj);

    if (!target) return syscall_err(IRIS_ERR_NOT_FOUND);

    int is_self = (target == caller);
    task_suspend(target);

    if (is_self) task_yield(); /* yield so another task runs; won't be rescheduled */
    return 0;
}

uint64_t sys_tcb_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t tcb_h = (handle_id_t)arg0;
    (void)arg1; (void)arg2;

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the TCB may be a CPtr slot or a handle.
     * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
    iris_error_t err = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)tcb_h,
                                 RIGHT_NONE, KOBJ_TCB, &obj, &rights);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(rights, RIGHT_WRITE)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KTcb *tcb = (struct KTcb *)obj;
    uint64_t flags = irq_spinlock_lock(&tcb->lock);
    struct task *target = tcb->task;

    if (!target) {
        irq_spinlock_unlock(&tcb->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    if (target->state == TASK_SUSPENDED)
        task_wakeup(target);

    irq_spinlock_unlock(&tcb->lock, flags);
    kobject_release(obj);
    return 0;
}

uint64_t sys_tcb_set_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t tcb_h = (handle_id_t)arg0;
    uint8_t     prio  = (uint8_t)(arg1 & 0xFFu);
    (void)arg2;

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the TCB may be a CPtr slot or a handle.
     * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
    iris_error_t err = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)tcb_h,
                                 RIGHT_NONE, KOBJ_TCB, &obj, &rights);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(rights, RIGHT_WRITE)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KTcb *tcb = (struct KTcb *)obj;
    uint64_t flags = irq_spinlock_lock(&tcb->lock);
    struct task *target = tcb->task;

    if (!target) {
        irq_spinlock_unlock(&tcb->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    target->priority = prio;

    irq_spinlock_unlock(&tcb->lock, flags);
    kobject_release(obj);
    return 0;
}

uint64_t sys_tcb_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t tcb_h = (handle_id_t)arg0;
    (void)arg1; (void)arg2;

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the TCB may be a CPtr slot or a handle.
     * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
    iris_error_t err = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)tcb_h,
                                 RIGHT_NONE, KOBJ_TCB, &obj, &rights);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(rights, RIGHT_WRITE)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KTcb *tcb = (struct KTcb *)obj;
    uint64_t flags = irq_spinlock_lock(&tcb->lock);
    struct task *target = tcb->task;

    if (!target) {
        irq_spinlock_unlock(&tcb->lock, flags);
        kobject_release(obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    int is_self = (target == caller);
    irq_spinlock_unlock(&tcb->lock, flags);
    kobject_release(obj);

    if (is_self) {
        task_exit_current(); /* does not return */
    } else {
        task_kill_external(target);
    }
    return 0;
}

uint64_t sys_tcb_get_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t tcb_h     = (handle_id_t)arg0;
    uint64_t    info_uptr = arg1;
    (void)arg2;

    if (!info_uptr) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the TCB may be a CPtr slot or a handle.
     * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
    iris_error_t err = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)tcb_h,
                                 RIGHT_NONE, KOBJ_TCB, &obj, &rights);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KTcb *tcb = (struct KTcb *)obj;
    uint64_t flags = irq_spinlock_lock(&tcb->lock);
    struct task *target = tcb->task;

    struct iris_tcb_info info;
    info.task_id  = tcb->task_id;
    info.priority = target ? target->priority : 0u;
    info.state    = (uint8_t)(target ? (uint8_t)target->state : (uint8_t)TASK_DEAD);
    info._pad[0]  = 0;
    info._pad[1]  = 0;

    irq_spinlock_unlock(&tcb->lock, flags);
    kobject_release(obj);

    if (!copy_to_user_checked(info_uptr, &info, (uint32_t)sizeof(info)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return 0;
}
