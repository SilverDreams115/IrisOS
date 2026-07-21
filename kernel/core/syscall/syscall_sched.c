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

/*
 * Fase S2: SYS_SC_CREATE (83) RETIRADO — fabricaba un KSchedContext desde
 * kslab y devolvía un handle: dos mecanismos no-seL4.  Los SchedulingContexts
 * se crean SOLO vía SYS_UNTYPED_RETYPE2 (storage Untyped, cap en CSpace) y se
 * configuran con SYS_SC_CONFIGURE.  Número reservado; sin efecto.
 */
uint64_t sys_sc_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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

/*
 * SYS_SC_BIND (arg0 = sc_cptr, arg1 = tcb_cptr) → 0 | error   (Fase S2, B4)
 *
 * Enlaza uno-a-uno un SchedulingContext a un TCB, ambos por CPtr, ambos vivos.
 * Falla BUSY si el SC ya está ligado a otra task o el TCB ya tiene otro SC.
 * arg1 == 0 desliga el SC de su task actual (unbind explícito).
 */
uint64_t sys_sc_bind(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t sc_cptr  = (iris_cptr_t)arg0;
    iris_cptr_t tcb_cptr = (iris_cptr_t)arg1;
    (void)arg2;

    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *sc_obj; iris_rights_t sc_r;
    iris_error_t err = cspace_or_handle_resolve_obj(caller->process, sc_cptr,
                                 RIGHT_NONE, KOBJ_SCHED_CONTEXT, &sc_obj, &sc_r);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(sc_r, RIGHT_WRITE)) {
        kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    struct KSchedContext *sc = (struct KSchedContext *)sc_obj;

    /* Unbind path (tcb_cptr == 0): detach from whatever task holds this SC. */
    if (tcb_cptr == 0u) {
        uint64_t f = irq_spinlock_lock(&sc->lock);
        struct task *bound = sc->bound_task;
        irq_spinlock_unlock(&sc->lock, f);
        if (bound && bound->sched_ctx == sc) {
            bound->sched_ctx = 0;
            kobject_release(&sc->base);   /* task's SC ref */
        }
        kschedctx_unbind(sc, 0);
        kobject_release(sc_obj);
        return 0;
    }

    struct KObject *tcb_obj; iris_rights_t tcb_r;
    err = cspace_or_handle_resolve_obj(caller->process, tcb_cptr,
                                 RIGHT_NONE, KOBJ_TCB, &tcb_obj, &tcb_r);
    if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    if (err != IRIS_OK) { kobject_release(sc_obj); return syscall_err(err); }
    if (!rights_check(tcb_r, RIGHT_WRITE)) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* SC must be configured before it can drive a thread (B2/B3). */
    if (!sc->configured) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Fase S2 D2: the KTCB IS struct task (KObject at offset 0) — resolve
     * directly, no wrapper indirection.  A terminated thread cannot be bound. */
    struct task *target = (struct task *)tcb_obj;
    if (target->terminal) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    /* Etapa 0: an unconfigured (retyped, inactive) TCB cannot bind an SC —
     * its destructor unwinds no sched_ctx reference, so allowing the bind
     * would leak the SC when the last cap drops.  TCB_CONFIGURE (Etapa 5/6)
     * is the point where a retyped TCB becomes bindable. */
    if (!target->configured) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
    /* Target must not already hold a different SC (S2.9). */
    if (target->sched_ctx && target->sched_ctx != sc) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_BUSY);
    }

    err = kschedctx_bind(sc, target);   /* one-to-one (BUSY if bound elsewhere) */
    if (err != IRIS_OK) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(err);
    }
    if (target->sched_ctx != sc) {
        kobject_retain(&sc->base);       /* target's SC ref */
        target->sched_ctx = sc;
        sc->remaining_budget = sc->budget_ticks;
    }

    kobject_release(tcb_obj);
    kobject_release(sc_obj);
    return 0;
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

        /* Fase S2: enforce one-to-one binding — a SC bound to another task
         * cannot be self-bound here (S2.9). */
        iris_error_t berr = kschedctx_bind(new_sc, t);
        if (berr != IRIS_OK) {
            kobject_release(&new_sc->base);
            return syscall_err(berr);
        }
    }

    /* Release old SC ref (and unbind it — this task no longer drives it). */
    if (t->sched_ctx) {
        kschedctx_unbind(t->sched_ctx, t);
        kobject_release(&t->sched_ctx->base);
    }

    t->sched_ctx = new_sc;

    /* Initialize budget from current configuration */
    if (new_sc)
        new_sc->remaining_budget = new_sc->budget_ticks;

    return 0;
}
