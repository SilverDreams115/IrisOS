#include "syscall_priv.h"



/* SYS_WRITE retired in Phase 30: serial output moved to ring-3 console service. */
uint64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_exit_current();
    return 0; /* unreachable */
}


uint64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_yield();
    return 0;
}


uint64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    return t ? t->id : 0;
}


uint64_t sys_process_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    handle_id_t h;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    h = handle_table_insert(&t->process->handle_table,
                            &t->process->base,
                            RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)h);
}


/* ── Process lifecycle query ──────────────────────────────────────── */

/*
 * sys_process_status(proc_handle) → 1 (alive), 0 (dead), or iris_error_t
 *
 * Non-blocking.  Returns immediately regardless of the target state.
 * Requires RIGHT_READ on proc_handle.
 *
 * Lifecycle contract:
 *   - Returns 1 while the process is running or blocked (main_thread alive).
 *   - Returns 0 once the process has called SYS_EXIT or been reaped;
 *     kprocess_teardown has run and TASK_DEAD has been set.
 *   - The handle remains valid after death until the caller closes it;
 *     this allows the caller to detect and then clean up in one pass.
 *   - Closing the handle (SYS_HANDLE_CLOSE) is the caller's responsibility
 *     after observing death; the KProcess is released when refcount hits zero.
 */
uint64_t sys_process_status(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (obj->type != KOBJ_PROCESS) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    int alive = kprocess_is_alive((struct KProcess *)obj);
    kobject_release(obj);
    return syscall_ok_u64(alive ? 1 : 0);
}


/*
 * sys_process_watch(proc_handle, chan_handle, cookie) → 0 or iris_error_t
 *
 * Registers a single process-exit watch for proc_handle. When the target
 * process tears down, the kernel sends PROC_EVENT_MSG_EXIT into chan_handle.
 *
 * This is the current event-driven lifecycle path for service supervision.
 * SYS_PROCESS_STATUS remains available as a fallback/non-blocking query.
 */
uint64_t sys_process_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    struct KObject *proc_obj;
    struct KObject *chan_obj;
    iris_rights_t proc_rights;
    iris_rights_t chan_rights;
    iris_error_t r;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg0, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_READ)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &chan_obj, &chan_rights);
    if (r != IRIS_OK) {
        kobject_release(proc_obj);
        return syscall_err(r);
    }
    if (chan_obj->type != KOBJ_CHANNEL) {
        kobject_release(proc_obj);
        kobject_release(chan_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(chan_rights, RIGHT_WRITE)) {
        kobject_release(proc_obj);
        kobject_release(chan_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    r = kprocess_watch_exit((struct KProcess *)proc_obj,
                            (struct KChannel *)chan_obj,
                            (handle_id_t)arg0,
                            (uint32_t)arg2);
    kobject_release(proc_obj);
    kobject_release(chan_obj);
    return syscall_err(r);
}


uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = ticks to sleep (at 100 Hz, 1 tick = 10ms) */
    if (arg0 == 0) return 0;
    scheduler_sleep_current(arg0);
    return 0;
}


/* ── Process termination ──────────────────────────────────────────── */

/*
 * sys_process_kill(proc_handle) → 0 or iris_error_t
 *
 * Requires RIGHT_MANAGE on proc_handle.
 * Cannot be used for self-termination — use SYS_EXIT for that (IRIS_ERR_INVALID_ARG).
 * Idempotent: if the target is already dead, returns 0 immediately.
 *
 * Internally calls task_kill_external which: runs kprocess_teardown (fires exit
 * watches, closes the process's own handle table, unregisters IRQ routes),
 * frees user stack pages, reaps the address space (safe since the caller's CR3
 * is different from the target's), and releases the kernel's creation reference.
 *
 * The caller's handle to the proc remains valid until the caller closes it;
 * the KProcess object is freed when all handles to it are closed.
 */
uint64_t sys_process_kill(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (obj->type != KOBJ_PROCESS) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_MANAGE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *target = (struct KProcess *)obj;

    /* Prevent suicide — caller must use SYS_EXIT for self-termination. */
    if (target == t->process) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Idempotent: already dead. */
    if (!kprocess_is_alive(target)) {
        kobject_release(obj);
        return syscall_ok_u64(0);
    }

    task_kill_process(target);
    kobject_release(obj);
    return syscall_ok_u64(0);
}


/*
 * sys_process_create() → proc_handle or iris_error_t
 *
 * Allocates a new empty KProcess with a fresh user address space (new CR3).
 * No threads are created.  The caller uses sys_vmo_map_into to populate the
 * address space and sys_thread_start to launch the first thread.
 */
uint64_t sys_process_create(uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject   *auth_obj;
    iris_rights_t     auth_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (auth_obj->type != KOBJ_BOOTSTRAP_CAP ||
        !kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_SPAWN_SERVICE)) {
        kobject_release(auth_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth_obj);

    struct KProcess *proc = kprocess_alloc();
    if (!proc) return syscall_err(IRIS_ERR_NO_MEMORY);

    proc->cr3 = paging_create_user_space();
    if (!proc->cr3) {
        kprocess_free(proc);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    handle_id_t h = handle_table_insert(&t->process->handle_table, &proc->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE |
                                        RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_ROUTE);
    if (h == HANDLE_INVALID) {
        kprocess_free(proc);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    /* On success: do NOT kobject_release.  The initial ref is the
     * thread-lifecycle reference; reap_dead_task_off_cpu drops it via
     * kprocess_free once the last thread exits. */
    return syscall_ok_u64((uint64_t)h);
}


/*
 * sys_thread_start(proc_h, entry_vaddr, stack_top, boot_arg) → 0 or iris_error_t
 *
 * Creates a new ring-3 thread in the process identified by proc_h.
 * entry_vaddr must be within the child private user image window; stack_top
 * must be within private user space and 8-byte aligned.
 * boot_arg is delivered in RBX on first execution.
 * Requires RIGHT_MANAGE on proc_h.
 */
uint64_t sys_thread_start(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t entry_vaddr = arg1;
    uint64_t user_rsp    = arg2;

    if (entry_vaddr < USER_PRIVATE_BASE || entry_vaddr >= USER_VMO_BASE)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp < USER_PRIVATE_BASE || user_rsp > USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp & 0x7ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_MANAGE)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *proc = (struct KProcess *)proc_obj;
    /* Accept fresh (thread_count=0) processes that haven't been torn down. */
    if (kprocess_teardown_complete(proc)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    struct task *nt = task_thread_create(proc, entry_vaddr, user_rsp, arg3);
    kobject_release(proc_obj);
    if (!nt) return syscall_err(IRIS_ERR_NO_MEMORY);
    return syscall_ok_u64(0);
}


/* ── Threading (D2) ──────────────────────────────────────────────── */

uint64_t sys_thread_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    uint64_t entry_vaddr = arg0;
    uint64_t user_rsp    = arg1;
    uint64_t arg         = arg2;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (entry_vaddr < USER_SPACE_BASE || entry_vaddr >= USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp < USER_SPACE_BASE || user_rsp > USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp & 0x7ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);  /* must be 8-byte aligned */

    struct task *nt = task_thread_create(t->process, entry_vaddr, user_rsp, arg);
    if (!nt) return syscall_err(IRIS_ERR_NO_MEMORY);
    return syscall_ok_u64((uint64_t)nt->id);
}


uint64_t sys_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_exit_current();
    return 0;  /* unreachable */
}
