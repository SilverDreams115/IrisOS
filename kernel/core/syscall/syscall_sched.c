#include "syscall_priv.h"

uint64_t sys_thread_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint32_t new_prio = (uint32_t)arg0;
    (void)arg1; (void)arg2;

    if (new_prio > TASK_PRIORITY_MAX)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint8_t old = t->priority;
    t->priority  = (uint8_t)new_prio;
    return (uint64_t)old;
}

uint64_t sys_sc_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KSchedContext *sc = kschedctx_alloc();
    if (!sc) return syscall_err(IRIS_ERR_NO_MEMORY);

    handle_id_t h = handle_table_insert(
        &t->process->handle_table, &sc->base,
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    kobject_release(&sc->base);

    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

uint64_t sys_sc_configure(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t sc_h    = (handle_id_t)arg0;
    uint64_t    budget  = arg1;
    uint64_t    period  = arg2;

    if (!sc_h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the SchedContext may be a CPtr slot or
     * a handle.  WRONG_TYPE maps to INVALID_ARG (this family's error code). */
    iris_error_t err = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)sc_h,
                                 RIGHT_NONE, KOBJ_SCHED_CONTEXT, &obj, &rights);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);

    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    err = kschedctx_configure((struct KSchedContext *)obj, budget, period);
    kobject_release(obj);
    return (err == IRIS_OK) ? 0ULL : syscall_err(err);
}

uint64_t sys_thread_set_sc(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t sc_h = (handle_id_t)arg0;
    (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KSchedContext *new_sc = 0;

    if (sc_h != 0) {
        struct KObject *obj;
        iris_rights_t   rights;
        /* A1 Increment 2b: dual resolver (CPtr slot or handle); sc_h == 0
         * stays the unbind path above.  WRONG_TYPE maps to INVALID_ARG. */
        iris_error_t err = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)sc_h,
                                     RIGHT_NONE, KOBJ_SCHED_CONTEXT, &obj, &rights);
        if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
        if (err != IRIS_OK) return syscall_err(err);

        /* Ownership of the retained ref (lifecycle-only, same contract as
         * handle_table_get_object) transfers to t->sched_ctx */
        new_sc = (struct KSchedContext *)obj;
    }

    /* Release old SC ref */
    if (t->sched_ctx)
        kobject_release(&t->sched_ctx->base);

    t->sched_ctx = new_sc;

    /* Initialize budget from current configuration */
    if (new_sc)
        new_sc->remaining_budget = new_sc->budget_ticks;

    return 0;
}
