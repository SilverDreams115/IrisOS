/*
 * syscall_tcb.c — Block 8 (Ph96-101): TCB capability syscalls.
 *
 * Phase S2 D2: the KTCB IS `struct task` (KObject at offset 0).  A KOBJ_TCB
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
 * SYS_TCB_CONFIGURE:     give a retyped (inactive) TCB its execution state,
 *                        naming the CSpace and VSpace it runs in as capabilities.
 * SYS_TCB_WRITE_REGS:    set where a configured, not-yet-started thread starts.
 */
#include "syscall_priv.h"

/* Resolve a KOBJ_TCB cap → struct task (lifecycle ref held on success).
 * WRONG_TYPE maps to INVALID_ARG to preserve this family's error code. */
static iris_error_t tcb_resolve(struct KCNode *root, iris_cptr_t cptr,
                                iris_rights_t required,
                                struct task **out, iris_rights_t *rights_out) {
    struct KObject *obj;
    iris_error_t err = cspace_resolve_only_obj(root, cptr, RIGHT_NONE,
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
    (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_NOT_FOUND);

    /* The calling thread IS its own KTCB — hand out a cap to &t->base. */
    const iris_rights_t rights =
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    /* Stage 4: arg0 is a destination slot (RETYPE2 packing).  The TCB is the
     * caller itself, borrowed, so publish_slot's consuming release needs a
     * reference of its own. */
    /* Stage 4: a destination slot is REQUIRED — the handle result is retired. */
    if (arg0 == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    kobject_retain(&t->base);
    iris_error_t pe = syscall_publish_slot(t, &t->base, rights, arg0, 0, 0);
    if (pe != IRIS_OK) return syscall_err(pe);
    return syscall_ok_u64(0);
}

/*
 * SYS_TCB_CONFIGURE(tcb_cptr, cspace_cptr, vspace_cptr)
 *
 * The operation Phase S2 named and could not implement: a TCB retyped from an
 * Untyped is born cap-complete but inactive — no registry slot, no kernel
 * stack, no address space — and every execution syscall refuses it.  What was
 * missing was not the code but the ARGUMENTS: a thread runs in a CSpace and a
 * VSpace, and neither was addressable as a capability until Stages 3-5 made
 * them so.
 *
 * Both must be the caller's own CSpace root and VSpace.  IRIS still composes a
 * thread's authority through KProcess, so a thread in a foreign address space
 * is process-server work (Stage 7) — accepting foreign capabilities here and
 * quietly running the thread somewhere else would be a lie in the signature.
 * The check is by object identity, not by convention: the caller must HOLD
 * capabilities to the CSpace and VSpace it names, which is why SYS_CSPACE_SELF
 * exists.
 */
uint64_t sys_tcb_configure(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                           uint64_t arg3) {
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg1) || !cspace_only_cptr(arg2))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    /*
     * Stage 7: WHICH process this thread joins is an argument.
     *
     * Until now it was always the caller's own, because the spawner could not
     * name its child's CSpace and VSpace — they were carved by the kernel
     * inside SYS_PROCESS_CREATE and never handed out.  Stage 6-pure Step 4/5
     * made the spawner RETYPE both and pass them in, so it holds them from
     * before the child exists, and the restriction had nothing left to
     * protect.  arg3 == 0 still means "my own", which is what a thread
     * creating a sibling wants.
     *
     * RIGHT_MANAGE on the process, because adding a thread to it is a change
     * to what that process can do, not something reading it should permit.
     */
    struct KProcess *proc = caller->process;
    struct KObject  *proc_obj = 0;
    if (arg3 != 0u) {
        iris_rights_t pr;
        err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg3,
                                      RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &pr);
        if (err != IRIS_OK) {
            kobject_release(&target->base);
            return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
        }
        if (!rights_check(pr, RIGHT_MANAGE)) {
            kobject_release(proc_obj); kobject_release(&target->base);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        proc = (struct KProcess *)proc_obj;
    }

    /* The CSpace argument: a real KCNode capability, which must BE that
     * process's root.  Identity, not equivalence — a different CNode with the
     * same contents is a different CSpace.  Naming the pair rather than
     * trusting the process object is the whole point: it is what makes the
     * signature honest about a thread running in a named CSpace and VSpace. */
    struct KObject *cs_obj; iris_rights_t cs_rights;
    err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg1, RIGHT_NONE,
                                  KOBJ_CNODE, &cs_obj, &cs_rights);
    if (err != IRIS_OK) {
        if (proc_obj) kobject_release(proc_obj);
        kobject_release(&target->base);
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
    }
    /* cspace_resolve_only_obj hands back a LIFECYCLE-only reference — it has
     * already dropped the traversal's active ref.  Releasing an active ref
     * here would decrement a count this call never took, and on the root CNode
     * that is not a leak but a demolition: reaching zero active refs runs the
     * close callback, which empties every slot of the CSpace being used. */
    struct KCNode *cspace = (struct KCNode *)cs_obj;
    int cs_ok = (cspace == proc->cspace_root);
    kobject_release(cs_obj);

    /* The VSpace argument: likewise that process's own address space. */
    struct KObject *vs_obj; iris_rights_t vs_rights;
    struct KVSpace *vspace = 0;
    int vs_ok = 0;
    if (cs_ok) {
        err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg2,
                                      RIGHT_NONE, KOBJ_VSPACE, &vs_obj, &vs_rights);
        if (err != IRIS_OK) {
            if (proc_obj) kobject_release(proc_obj);
            kobject_release(&target->base);
            return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
        }
        vspace = (struct KVSpace *)vs_obj;
        vs_ok  = (vspace == proc->vspace);
        kobject_release(vs_obj);
    }
    if (!cs_ok || !vs_ok) {
        if (proc_obj) kobject_release(proc_obj);
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Stage 7 Step 4: the CSpace travels to the thread as the capability the
     * caller named.  Safe to pass after its resolve reference was dropped: the
     * identity check above proved it is the process's root, and the process
     * holds that root for as long as it lives — ktcb_configure takes its own
     * pair before anything can drop the last one. */
    err = ktcb_configure(target, proc, cspace, vspace);
    if (proc_obj) kobject_release(proc_obj);
    kobject_release(&target->base);
    if (err != IRIS_OK) return syscall_err(err);
    return syscall_ok_u64(0);
}

/*
 * SYS_TCB_WRITE_REGS(tcb_cptr, entry, sp, arg)
 *
 * Where a configured thread starts.  Separate from CONFIGURE because they
 * answer different questions — what a thread IS, and what it will DO — and
 * because a supervisor may want to configure a thread long before it decides
 * either.  Refused once the thread has been runnable: its kernel stack then
 * holds live state, and the entry frame lives on that stack.
 */
uint64_t sys_tcb_write_regs(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3) {
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    /* An unconfigured TCB has no address space to point at, so the answer is
     * the same NOT_SUPPORTED that RESUME and EXIT give it — checked BEFORE the
     * ownership test, which would otherwise report ACCESS_DENIED for a thread
     * that simply has no process yet. */
    if (!target->configured || target->terminal) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
    /*
     * Stage 7: the TCB capability IS the authority.
     *
     * This used to refuse a target in another process, because a spawner had
     * no legitimate reason to point one — it could not have configured it
     * either.  Now it configures its child's initial thread, so refusing to
     * say where that thread starts would leave it configured and unstartable.
     * The address is still checked against the address space the thread was
     * CONFIGURED for, which is the guarantee that mattered; who invoked it is
     * answered by holding the capability with RIGHT_WRITE.
     */

    err = ktcb_write_regs(target, arg1, arg2, arg3);
    kobject_release(&target->base);
    if (err != IRIS_OK) return syscall_err(err);
    return syscall_ok_u64(0);
}

uint64_t sys_tcb_suspend(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_FOUND); }
    /* Step 0: an unconfigured (retyped, inactive) TCB has no execution to
     * suspend — refuse without side effects (TCB_CONFIGURE: Step 5/6). */
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
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_FOUND); }
    /* Step 0: an unconfigured TCB must NEVER be made runnable — it has no
     * kstack, no registry slot, no process.  Hard refuse (charter O5/S-gate). */
    if (!target->configured) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_SUPPORTED); }
    /*
     * ...and neither must a CONFIGURED thread that was never told where to
     * start.  CONFIGURE builds the storage a thread needs; WRITE_REGS builds
     * the frame it returns THROUGH, and until it has run there is nothing on
     * the kernel stack: saved_krsp is still the zero the retyped block arrived
     * with.  Making that runnable puts `movq $0, %rsp` in context_switch and
     * the next push is a ring-0 store through a null stack — a fault with no
     * stack to take it on.
     *
     * saved_krsp is the exact witness, not a proxy: every thread that has run
     * has one (context_switch saves it), and every thread that has been given
     * an entry frame has one (ktcb_write_regs sets it).  A pool-born thread is
     * born with both.  Checked BEFORE `started` is set, so a refusal does not
     * freeze the entry frame it just refused to run.
     */
    if (!target->saved_krsp) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
    /* Stage 5 Step 4: a thread that has been runnable once holds live state
     * on its kernel stack, so its entry frame is frozen from here on
     * (SYS_TCB_WRITE_REGS refuses). */
    target->started = 1;
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
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
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
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return 0; /* already gone */ }
    /* Step 0: nothing is executing in an unconfigured TCB — refuse. */
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
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
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
