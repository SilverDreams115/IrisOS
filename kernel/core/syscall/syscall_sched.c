#include "syscall_priv.h"

/*
 * sys_thread_priority — RETIRED (charter A5, ledger A-20).
 *
 * It set the CALLER's own priority, up to TASK_PRIORITY_MAX, taking NO
 * CAPABILITY at all — ambient authority over the scheduler, and the strongest
 * kind, because priority 255 starves everything below it.  It is the same
 * shape as the three SELF syscalls A-18 retired, sitting on the scheduler
 * instead of on the CSpace, and it had no callers.
 *
 * `SYS_TCB_SET_PRIORITY` is the capability-based call that already did this
 * properly, and since A-20 it also takes an AUTHORITY and refuses to grant a
 * priority above that authority's ceiling — which is what seL4 means by
 * setting a priority.  The number stays permanently reserved.
 */

/*
 * Phase S2: SYS_SC_CREATE (83) RETIRED — it fabricated a KSchedContext from
 * kslab and returned a handle: two non-seL4 mechanisms.  SchedulingContexts
 * are created ONLY via SYS_UNTYPED_RETYPE2 (Untyped storage, cap in CSpace)
 * and configured with SYS_SC_CONFIGURE.  Number reserved; no effect.
 */
uint64_t sys_sc_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

uint64_t sys_sc_configure(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3) {
    handle_id_t sc_h    = (handle_id_t)arg0;
    uint64_t    budget  = arg1;
    uint64_t    period  = arg2;

    if (!sc_h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    /*
     * arg3 is the SCHEDCONTROL capability — the authority over CPU TIME
     * (ledger A-20).  seL4 makes this the whole point of
     * `seL4_SchedControl_Configure`: a budget and a period do not come from
     * holding the scheduling context, they come from being granted time.
     * Holding the SC says WHICH context to configure; holding this says you
     * may configure one at all.
     */
    if (!syscall_has_bootcap(t, arg3, IRIS_BOOTCAP_SCHED_CONTROL))
        return syscall_err(IRIS_ERR_ACCESS_DENIED);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2b: dual resolver — the SchedContext may be a CPtr slot or
     * a handle.  WRONG_TYPE maps to INVALID_ARG (this family's error code). */
    iris_error_t err = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)sc_h,
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
 * SYS_SC_BIND (arg0 = sc_cptr, arg1 = tcb_cptr) → 0 | error   (Phase S2, B4)
 *
 * Binds a SchedulingContext one-to-one to a TCB, both by CPtr, both live.
 * Fails BUSY if the SC is already bound to another task or the TCB already
 * has another SC.  arg1 == 0 unbinds the SC from its current task (explicit
 * unbind).
 */
uint64_t sys_sc_bind(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t sc_cptr  = (iris_cptr_t)arg0;
    iris_cptr_t tcb_cptr = (iris_cptr_t)arg1;
    (void)arg2;

    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *sc_obj; iris_rights_t sc_r;
    iris_error_t err = cspace_resolve_only_obj(caller->cspace_root, sc_cptr,
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
    err = cspace_resolve_only_obj(caller->cspace_root, tcb_cptr,
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

    /* Phase S2 D2: the KTCB IS struct task (KObject at offset 0) — resolve
     * directly, no wrapper indirection.  A terminated thread cannot be bound. */
    struct task *target = (struct task *)tcb_obj;
    if (target->terminal) {
        kobject_release(tcb_obj); kobject_release(sc_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    /* Step 0: an unconfigured (retyped, inactive) TCB cannot bind an SC —
     * its destructor unwinds no sched_ctx reference, so allowing the bind
     * would leak the SC when the last cap drops.  TCB_CONFIGURE (Step 5/6)
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
        /* Stage 8-mcs: a fresh binding starts with the whole budget available
         * and no replenishments outstanding — anything queued was earned by a
         * different thread's execution and must not follow the SC. */
        sc->remaining_budget = sc->budget_ticks;
        kschedctx_refill_reset(sc);
    }

    kobject_release(tcb_obj);
    kobject_release(sc_obj);
    return 0;
}

uint64_t sys_thread_set_sc(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t sc_h = (handle_id_t)arg0;
    (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KSchedContext *new_sc = 0;

    if (sc_h != 0) {
        struct KObject *obj;
        iris_rights_t   rights;
        /* A1 Increment 2b: dual resolver (CPtr slot or handle); sc_h == 0
         * stays the unbind path above.  WRONG_TYPE maps to INVALID_ARG. */
        iris_error_t err = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)sc_h,
                                     RIGHT_NONE, KOBJ_SCHED_CONTEXT, &obj, &rights);
        if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
        if (err != IRIS_OK) return syscall_err(err);

        /* Ownership of the retained ref (lifecycle-only, same contract as
         * the resolver) transfers to t->sched_ctx */
        new_sc = (struct KSchedContext *)obj;

        /* Phase S2: enforce one-to-one binding — a SC bound to another task
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
    if (new_sc) {
        new_sc->remaining_budget = new_sc->budget_ticks;
        kschedctx_refill_reset(new_sc);
    }

    return 0;
}
