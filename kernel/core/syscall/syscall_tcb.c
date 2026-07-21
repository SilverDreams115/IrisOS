/*
 * syscall_tcb.c — Block 8 (Ph96-101): TCB capability syscalls.
 *
 * Fase S2 D2: the KTCB IS `struct task` (KObject at offset 0).  A KOBJ_TCB
 * capability resolves directly to the task; there is no wrapper indirection.
 * A cap to a TERMINATED thread still identifies the same object and answers
 * SYS_TCB_GET_INFO (state = TERMINATED); it cannot be resumed.
 *
 * SYS_TCB_SELF:          return a handle to the calling thread's KTCB.
 * SYS_TCB_SUSPEND:       transition target thread to TASK_SUSPENDED.
 * SYS_TCB_RESUME:        wake a TASK_SUSPENDED thread.
 * SYS_TCB_SET_PRIORITY:  change a thread's scheduling priority.
 * SYS_TCB_EXIT:          forcibly terminate a thread.
 * SYS_TCB_GET_INFO:      copy struct iris_tcb_info to userland.
 */
#include "syscall_priv.h"

/* Resolve a KOBJ_TCB cap → struct task (lifecycle ref held on success).
 * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
static iris_error_t tcb_resolve(struct KProcess *proc, iris_cptr_t cptr,
                                iris_rights_t required,
                                struct task **out, iris_rights_t *rights_out) {
    struct KObject *obj;
    iris_error_t err = cspace_or_handle_resolve_obj(proc, cptr, RIGHT_NONE,
                                                    KOBJ_TCB, &obj, rights_out);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return err;
    if (required != RIGHT_NONE && !rights_check(*rights_out, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out = (struct task *)obj;   /* KObject at offset 0 */
    return IRIS_OK;
}

uint64_t sys_tcb_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_NOT_FOUND);

    /* The calling thread IS its own KTCB — hand out a cap to &t->base. */
    handle_id_t h = handle_table_insert(
        &t->process->handle_table, &t->base,
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

uint64_t sys_tcb_suspend(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->process, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_FOUND); }
    /* Etapa 0: an unconfigured (retyped, inactive) TCB has no execution to
     * suspend — refuse without side effects (TCB_CONFIGURE: Etapa 5/6). */
    if (!target->configured) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_SUPPORTED); }

    int is_self = (target == caller);
    task_suspend(target);
    kobject_release(&target->base);

    if (is_self) task_yield();
    return 0;
}

uint64_t sys_tcb_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->process, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_FOUND); }
    /* Etapa 0: an unconfigured TCB must NEVER be made runnable — it has no
     * kstack, no registry slot, no process.  Hard refuse (charter O5/S-gate). */
    if (!target->configured) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_SUPPORTED); }
    if (target->state == TASK_SUSPENDED)
        task_wakeup(target);

    kobject_release(&target->base);
    return 0;
}

uint64_t sys_tcb_set_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint8_t prio = (uint8_t)(arg1 & 0xFFu);
    (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->process, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_FOUND); }
    target->priority = prio;
    kobject_release(&target->base);
    return 0;
}

uint64_t sys_tcb_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->process, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return 0; /* already gone */ }
    /* Etapa 0: nothing is executing in an unconfigured TCB — refuse. */
    if (!target->configured) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_SUPPORTED); }

    int is_self = (target == caller);
    kobject_release(&target->base);

    if (is_self) {
        task_exit_current(); /* does not return */
    } else {
        task_kill_external(target);
    }
    return 0;
}

uint64_t sys_tcb_get_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t info_uptr = arg1;
    (void)arg2;
    if (!info_uptr) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->process, (iris_cptr_t)arg0,
                                   RIGHT_READ, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    /* A cap to a TERMINATED thread still answers here (object lifetime
     * outlives execution).  We read a snapshot under the object lock. */
    uint64_t flags = irq_spinlock_lock(&target->obj_lock);
    struct iris_tcb_info info;
    info.task_id  = target->id;
    info.priority = target->priority;
    info.state    = (uint8_t)target->state;
    info._pad[0]  = 0;
    info._pad[1]  = 0;
    irq_spinlock_unlock(&target->obj_lock, flags);
    kobject_release(&target->base);

    if (!copy_to_user_checked(info_uptr, &info, (uint32_t)sizeof(info)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return 0;
}
