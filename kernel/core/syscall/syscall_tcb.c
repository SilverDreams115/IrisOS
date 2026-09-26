/* SPDX-License-Identifier: Apache-2.0 */
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

/* Resolve a KOBJ_TCB cap → struct task (lifecycle ref held on success). */
static iris_error_t tcb_resolve(struct KCNode *root, iris_cptr_t cptr,
                                iris_rights_t required,
                                struct task **out, iris_rights_t *rights_out) {
    struct KObject *obj;
    /* WRONG_TYPE is reported as WRONG_TYPE.  This flattened it to INVALID_ARG
     * — "something about your argument is wrong", said by a resolver that had
     * just identified the capability exactly.  Third instance of the same
     * defect: the typed resolvers had it until A-20's type-before-rights fix,
     * and `dev_cap_budget` had it until D-5. */
    iris_error_t err = cspace_resolve_only_obj(root, cptr, RIGHT_NONE,
                                                    KOBJ_TCB, &obj, rights_out);
    if (err != IRIS_OK) return err;
    if (required != RIGHT_NONE && !rights_check(*rights_out, required)) {
        kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out = (struct task *)obj;   /* KObject at offset 0 */
    return IRIS_OK;
}

/*
 * sys_tcb_self — RETIRED (ledger A-18 / charter A5).
 *
 * It handed a thread a capability to ITSELF, asking for no capability at all:
 * ambient authority, which seL4 does not have.  It also published an MDB
 * LEGACY ROOT — no ancestor, no revoke reaches it.
 *
 * A thread is told which thread it is by whoever CREATED it.  For the first
 * thread of a process that is IRIS_CPTR_OWN_TCB, minted by its spawner before
 * it runs; for every other thread it is the entry register, which is the one
 * per-thread channel a freshly started thread has and is why this could be
 * removed at all.  The number stays permanently reserved.
 */

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
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg1) || !cspace_only_cptr(arg2))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    /*
     * arg3 — the ROOT CSpace GUARD (Stage 8-cap, ledger D-2).
     *
     * It named the PROCESS the thread would join until Stage 7-proc, and was
     * reserved and ignored after that.  It carries seL4's `cspace_root_data`
     * now, which is the same argument in the same position of the same
     * operation: the guard belonging to the CSpace capability being installed.
     *
     * Packed `guard | guard_bits << 32`.  Zero bits means no guard, which is
     * what every thread has until somebody asks otherwise, so this is additive
     * for every existing caller — including the ones still passing a stale
     * process capability, whose low bits become a guard of zero width.
     *
     * Refused rather than truncated when the guard does not fit its declared
     * width, or when the width plus the root's radix would not fit a CPtr: a
     * root guard that silently means something other than what was asked for
     * would make every CPtr in that thread's CSpace mean something else too.
     */
    uint64_t root_guard      = arg3 & 0xFFFFFFFFu;
    uint32_t root_guard_bits = (uint32_t)(arg3 >> 32);
    if (root_guard_bits > KCNODE_GUARD_BITS_MAX) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    if (root_guard_bits < 64u && (root_guard >> root_guard_bits) != 0u) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /*
     * The CSpace argument: a real KCNode capability the caller HOLDS.
     *
     * Stage 7-proc: it used to have to BE that process's root, checked by
     * identity, and the VSpace likewise.  That check was KProcess acting as an
     * authority: the capability you named was not enough, it also had to match
     * a third object's idea of what your CSpace should be.  A thread runs in
     * the CSpace and the address space its configurer NAMED and holds, which
     * is seL4's seL4_TCB_Configure, and threads sharing a pair are what a
     * process IS rather than something to be checked against one.
     *
     * The process argument survives this step for one reason, and it is not
     * authority: teardown still counts threads (`thread_count`), and that
     * count is what reclaims an address space when the last one exits.  Moving
     * reclamation off it is the remaining content of Stage 7-proc — attempting
     * both at once produced a kernel where every spawned thread faulted on its
     * own entry point, because a thread with no process is a thread whose
     * address space nothing reclaims and whose syscall guards all fail.
     */
    struct KObject *cs_obj; iris_rights_t cs_rights;
    err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg1, RIGHT_NONE,
                                  KOBJ_CNODE, &cs_obj, &cs_rights);
    if (err != IRIS_OK) {
        kobject_release(&target->base);
        return syscall_err(err);
    }
    /* cspace_resolve_only_obj hands back a LIFECYCLE-only reference — it has
     * already dropped the traversal's active ref.  Releasing an active ref
     * here would decrement a count this call never took, and on the root CNode
     * that is not a leak but a demolition: reaching zero active refs runs the
     * close callback, which empties every slot of the CSpace being used. */
    struct KCNode *cspace = (struct KCNode *)cs_obj;
    kobject_release(cs_obj);

    /* The VSpace argument: the address space the thread will run in. */
    struct KObject *vs_obj; iris_rights_t vs_rights;
    err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg2,
                                  RIGHT_NONE, KOBJ_VSPACE, &vs_obj, &vs_rights);
    if (err != IRIS_OK) {
        kobject_release(&target->base);
        return syscall_err(err);
    }
    struct KVSpace *vspace = (struct KVSpace *)vs_obj;
    kobject_release(vs_obj);

    /* Stage 7 Step 4: the CSpace travels to the thread as the capability the
     * caller named.  Safe to pass after its resolve reference was dropped: the
     * CALLER holds it in a CSpace slot for the whole of this syscall — that is
     * how it was resolved — so nothing can drop the last reference before
     * ktcb_configure takes its own pair. */
    err = ktcb_configure(target, cspace, vspace);
    if (err == IRIS_OK) {
        /* Stage 8-cap / D-2: the guard belongs to the CSpace capability just
         * installed, so it is written with it and only when the install
         * succeeded — a guard on a root the thread does not have would change
         * how a CSpace it never got resolves. */
        target->cspace_root_guard      = root_guard;
        target->cspace_root_guard_bits = (uint8_t)root_guard_bits;
    }
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
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

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

/*
 * SYS_TCB_READ_REGS(tcb_cptr, out_uptr) — ledger A-28.
 *
 * seL4's `seL4_TCB_ReadRegisters`, and the other half of an asymmetry: a
 * supervisor could point a thread anywhere it liked and never ask where it
 * was.  A fault handler gets the rip and the faulting address in the message
 * (A-22); which REGISTER held the bad pointer was unreachable from ring 3 by
 * any means.
 *
 * RIGHT_READ deliberately.  Observing a thread is not changing it, and a
 * supervisor that may only watch should be expressible — which is why seL4
 * makes this its own invocation instead of a direction flag on the write.
 */
uint64_t sys_tcb_read_regs(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(struct iris_user_ctx)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_READ, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (!target->configured || target->terminal) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
    /*
     * A RUNNING thread's registers are in the CPU, not the TCB (D-1 step 3):
     * the saved frame is whatever it looked like when it last left a core, and
     * handing that back as "the current state" would be a lie a debugger acts
     * on.  Reading YOURSELF is the same situation and is refused for the same
     * reason — the frame you would read is the one this syscall entered on.
     */
    if (target == caller || target->state == TASK_RUNNING) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_BUSY);
    }

    struct iris_user_ctx snapshot = target->user_ctx;
    kobject_release(&target->base);

    if (!copy_to_user_checked(arg1, &snapshot, (uint32_t)sizeof(snapshot)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(0);
}

/*
 * SYS_TCB_WATCH(tcb_cptr, notif_cptr, signal_bits) → 0 or iris_error_t
 *
 * Stage 7 Step 10: be told when THIS THREAD dies.
 *
 * SYS_PROCESS_WATCH asked the same question of a process, which meant a
 * supervisor needed authority over an object it did not create to learn about
 * an execution it did.  A supervisor HAS the thread — it retyped the TCB and
 * configured it — and every service in the tree is single-threaded, so this is
 * not an approximation of the process event, it is that event named by the
 * thing that produces it.
 *
 * RIGHT_READ on the thread: learning that something died confers nothing over
 * it.  RIGHT_WRITE on the notification, which is what signalling one takes.
 * Arming a thread that is ALREADY dead fires immediately rather than waiting
 * forever — a supervisor that lost the race still learns the answer.
 */
/*
 * SYS_TCB_SET_FAULT_HANDLER(tcb_cptr, ep_cptr) — ledger A-22.
 *
 * Point a thread's faults at an ENDPOINT.  When it faults, the thread CALLS
 * that endpoint: the handler receives the record as an ordinary message, gets
 * a reply capability with it, and replying is what resumes the thread.
 *
 * This used to take a notification, a signal-bit mask and a MAILBOX — a CNode
 * slot the kernel published the faulting thread's capability into on every
 * fault — and the handler then read the record with SYS_TCB_FAULT_INFO and
 * answered with SYS_EXCEPTION_RESUME plus a generation number.  Three
 * mechanisms doing what seL4 does with one, and each of the three had to
 * reinvent something IPC already had: the mailbox was a hand-rolled capability
 * delivery (with its own parent-tracking so revoke could reach it), and the
 * generation number was a hand-rolled one-shot token.  A reply capability is
 * both, and is the same object every server in the system already uses.
 *
 * The BADGE on `ep_cptr` is captured and stamped into every fault message this
 * thread produces.  That is how the handler knows WHICH client faulted, and it
 * is why nothing has to be minted into anyone's CSpace at fault time.  A
 * supervisor arming several threads mints itself several badged capabilities to
 * one endpoint, which is exactly seL4's arrangement.
 *
 * RIGHT_WRITE on the endpoint is required: arming a thread's faults means
 * arranging for messages to be SENT there.
 */
static uint64_t tcb_register_handler(uint64_t arg0, uint64_t arg1, int timeout) {
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    struct KEndpoint *ep; iris_rights_t ep_rights; uint64_t badge = 0;
    /* WRONG_TYPE travels: "that is not an endpoint" is what the caller needs
     * to hear, and the family has reported it since Step 4. */
    err = cspace_resolve_only_endpoint_badged(caller->cspace_root,
                                              (iris_cptr_t)arg1, RIGHT_WRITE,
                                              &ep, &ep_rights, &badge);
    if (err != IRIS_OK) {
        kobject_release(&target->base);
        return syscall_err(err);
    }

    struct KEndpoint *old = 0;
    uint64_t irqfl = irq_spinlock_lock(&target->obj_lock);
    /* Under the lock, not before it: thread teardown empties these fields under
     * the same lock, so a check outside it could pass just as teardown starts
     * and leave the reference installed below with nobody to release it. */
    if (target->terminal) {
        irq_spinlock_unlock(&target->obj_lock, irqfl);
        kobject_release(&ep->base);
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    if (timeout) {
        old = (target->timeout_ep == ep) ? 0 : target->timeout_ep;
        if (target->timeout_ep != ep) {
            kobject_retain(&ep->base);
            kobject_active_retain(&ep->base);
            target->timeout_ep = ep;
        }
        target->timeout_ep_badge = badge;
    } else {
        old = (target->fault_ep == ep) ? 0 : target->fault_ep;
        if (target->fault_ep != ep) {
            kobject_retain(&ep->base);
            kobject_active_retain(&ep->base);
            target->fault_ep = ep;
        }
        target->fault_ep_badge = badge;
    }
    irq_spinlock_unlock(&target->obj_lock, irqfl);

    if (old) { kobject_active_release(&old->base); kobject_release(&old->base); }

    /*
     * An outstanding fault does NOT move with a re-aimed endpoint.
     *
     * It used to: the mailbox was re-filled so a supervisor taking over from a
     * dead handler could answer the fault in flight.  It cannot move now, and
     * that is the correct answer rather than a lost feature — the fault is a
     * CALL that is already queued on, or already answered through, the old
     * endpoint, and the authority to answer it is a reply capability somebody
     * holds.  Re-aiming decides where the NEXT fault goes.  A supervisor that
     * wants the blocked thread back kills it with the TCB capability it just
     * used to re-aim it.
     */
    kobject_release(&ep->base);
    kobject_release(&target->base);
    return syscall_ok_u64(0);
}

uint64_t sys_tcb_set_fault_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                   uint64_t arg3) {
    /* A-22: the mailbox and the signal mask are gone.  Refusing a caller that
     * still passes them is deliberate — silently ignoring two arguments would
     * let code written for the old shape keep compiling and keep "working"
     * while the mailbox it names is never filled. */
    if (arg2 != 0u || arg3 != 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    return tcb_register_handler(arg0, arg1, /*timeout=*/0);
}

/*
 * SYS_TCB_SET_TIMEOUT_HANDLER (128) — Stage 8-mcs.
 *
 * Arms the thread's TIMEOUT fault handler: when its scheduling context runs
 * out of budget, the thread is suspended and the handler is told, instead of
 * the thread silently blocking until the next period refills it.
 *
 * Identical arguments and identical authority to SYS_TCB_SET_FAULT_HANDLER —
 * RIGHT_WRITE on the thread and RIGHT_WRITE on the endpoint — because it is
 * the same mechanism.  It is a SEPARATE
 * registration because it is a different authority: a temporal supervisor
 * answering "this thread overran" is not the pager answering "this thread
 * touched an unmapped page", and one server holding both would hold power over
 * the other's domain.  seL4 splits them for the same reason.
 */
uint64_t sys_tcb_set_timeout_handler(uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3) {
    if (arg2 != 0u || arg3 != 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    return tcb_register_handler(arg0, arg1, /*timeout=*/1);
}

/*
 * SYS_TCB_BIND_NOTIFICATION(tcb_cptr, notif_cptr) — ledger A-23.
 *
 * seL4's `seL4_TCB_BindNotification`.  A thread blocked receiving on an
 * endpoint is otherwise deaf to signals — it is in the endpoint's queue and
 * nothing else can reach it — which forced every server that needs both an
 * interrupt and a request queue to spend a second thread on the choice.  A
 * driver IS that server, so the absence was structural rather than a
 * convenience: the timer service (A-24) is the first thing that could not be
 * written without it.
 *
 * `notif_cptr == 0` unbinds.  RIGHT_WRITE on both: which thread a signal wakes
 * is a property of that thread, and being the target of a delivery is a write
 * to the notification.
 */
uint64_t sys_tcb_bind_notification(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (arg1 == 0u) {
        /* Unbind.  Idempotent: a thread that was not bound is already in the
         * state the caller asked for. */
        knotification_unbind_task(target);
        kobject_release(&target->base);
        return syscall_ok_u64(0);
    }

    struct KObject *n_obj; iris_rights_t n_rights;
    err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg1,
                                  RIGHT_NONE, KOBJ_NOTIFICATION, &n_obj, &n_rights);
    if (err != IRIS_OK) { kobject_release(&target->base); return syscall_err(err); }
    if (!rights_check(n_rights, RIGHT_WRITE)) {
        kobject_release(n_obj); kobject_release(&target->base);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (target->terminal) {
        kobject_release(n_obj); kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    err = knotification_bind((struct KNotification *)n_obj, target);
    kobject_release(n_obj);
    kobject_release(&target->base);
    return (err == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(err);
}

uint64_t sys_tcb_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (arg2 == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_READ, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    struct KObject *n_obj; iris_rights_t n_rights;
    err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg1,
                                  RIGHT_NONE, KOBJ_NOTIFICATION,
                                  &n_obj, &n_rights);
    if (err != IRIS_OK) {
        kobject_release(&target->base);
        return syscall_err(err);
    }
    if (!rights_check(n_rights, RIGHT_WRITE)) {
        kobject_release(n_obj); kobject_release(&target->base);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KNotification *notif = (struct KNotification *)n_obj;
    struct KNotification *old   = 0;
    int fire_now = 0;

    uint64_t irqfl = irq_spinlock_lock(&target->obj_lock);
    if (target->terminal) {
        fire_now = 1;                    /* already over: answer, do not arm */
    } else {
        old = target->exit_notif;
        kobject_retain(&notif->base);
        kobject_active_retain(&notif->base);
        target->exit_notif = notif;
        target->exit_bits  = arg2;
    }
    irq_spinlock_unlock(&target->obj_lock, irqfl);

    if (old) {
        kobject_active_release(&old->base);
        kobject_release(&old->base);
    }
    if (fire_now) knotification_signal(notif, arg2);

    kobject_release(n_obj);
    kobject_release(&target->base);
    return syscall_ok_u64(0);
}

/*
 * SYS_TCB_EXIT_CODE(tcb_cptr) → the code, or IRIS_ERR_WOULD_BLOCK
 *
 * Stage 7 Step 10: the code a thread exited with, read off that thread.
 * WOULD_BLOCK while it is still running, which is the same answer the
 * process-scoped form gives for a process that has not exited.
 */
uint64_t sys_tcb_exit_code(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_READ, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    int      done = target->terminal;
    uint32_t code = target->exit_code;
    kobject_release(&target->base);

    if (!done) return syscall_err(IRIS_ERR_WOULD_BLOCK);
    return syscall_ok_u64((uint64_t)code);
}

uint64_t sys_tcb_suspend(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    /* Re-entry after a self-suspend: somebody resumed us, and the suspension
     * is over.  Redoing the resolve would suspend the thread again. */
    if (caller->sc_reentry) return 0;

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

    /*
     * Suspending YOURSELF blocks, so it parks like every other blocking
     * syscall (Stage 9-evt).  It used to yield from inside this frame and
     * return here when somebody resumed the thread — the continuation being
     * "the rest of this function" on the thread's kernel stack, for however
     * long the suspension lasted.
     *
     * The re-entry does not redo the work above: `sc_reentry` is checked at
     * the top, because a restarted suspend that resolved and suspended again
     * would put the thread straight back to sleep the instant it was resumed.
     */
    if (is_self) syscall_request_restart(caller);
    return 0;
}

uint64_t sys_tcb_resume(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

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
     * start.  CONFIGURE builds the storage a thread needs; WRITE_REGS says
     * where it begins, and until that has happened there is nowhere to resume
     * it TO.
     *
     * The witness used to be `saved_krsp` — a thread's entry frame was pushed
     * onto its own kernel stack, so a non-zero saved stack pointer meant "it
     * has one".  Stage 9-evt step 3 moved that frame into the TCB, and the
     * witness moved with it: `resume_user` is TASK_RESUME_KERNEL on a retyped
     * block and becomes TASK_RESUME_USER_FIRST exactly when WRITE_REGS runs.
     * Checking the old field after the frame stopped living there is how this
     * failed to spot the change — a stale witness answers, it just answers
     * about the wrong thing.
     *
     * Checked BEFORE `started` is set, so a refusal does not freeze the entry
     * it just refused to run.
     */
    /*
     * A-42: the witness is read and `started` is published under the thread's
     * own obj_lock, because WRITE_REGS tests `started` to decide whether the
     * entry frame is still writable.  Unlocked on both sides, the two pass
     * each other and the frame of a thread this call is about to run gets
     * rewritten underneath it.
     */
    uint64_t tf = irq_spinlock_lock(&target->obj_lock);
    if (target->resume_user == TASK_RESUME_KERNEL && !target->kentry) {
        irq_spinlock_unlock(&target->obj_lock, tf);
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
    /* Stage 5 Step 4: a thread that has been runnable once holds live state,
     * so its entry frame is frozen from here on (SYS_TCB_WRITE_REGS refuses). */
    target->started = 1;
    irq_spinlock_unlock(&target->obj_lock, tf);
    if (target->state == TASK_SUSPENDED)
        task_wakeup(target);

    kobject_release(&target->base);
    return 0;
}

uint64_t sys_tcb_set_priority(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    /*
     * arg2 is the AUTHORITY: a TCB capability whose ceiling bounds what may be
     * granted (ledger A-20).  seL4 spells it
     * `seL4_TCB_SetPriority(service, authority, priority)` and refuses a
     * priority above the authority's MCP — priority is delegated downward and
     * never invented.  IRIS took no authority and no bound, so a holder of any
     * TCB capability could set 255 and starve everything below it.
     *
     * arg2 == 0 means "myself as the authority", which is the common case and
     * is not a loophole: a thread's own ceiling is the one it was configured
     * with, and it can never exceed that.
     */
    uint8_t prio = (uint8_t)(arg1 & 0xFFu);
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint8_t ceiling;
    if (arg2 == 0u) {
        ceiling = caller->mcp;
    } else {
        struct task *auth; iris_rights_t auth_rights;
        iris_error_t aerr = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg2,
                                        RIGHT_READ, &auth, &auth_rights);
        if (aerr != IRIS_OK) return syscall_err(aerr);
        ceiling = auth->mcp;
        kobject_release(&auth->base);
    }
    if (prio > ceiling) return syscall_err(IRIS_ERR_ACCESS_DENIED);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_FOUND); }
    target->priority = prio;
    kobject_release(&target->base);
    return 0;
}

/*
 * sys_tcb_set_mcpriority(tcb_cptr, mcp, authority_cptr)
 *   ledger A-20's other half — seL4's `seL4_TCB_SetMCPriority`.
 *
 * A thread's MCP is the CEILING on what it may grant: `TCB_SetPriority` reads
 * the authority's `mcp` and refuses anything above it, so a supervisor given
 * 100 configures threads that can grant at most 100, downward, for ever.
 *
 * Until now that ceiling could only be INHERITED — `TCB_Configure` stamps the
 * configurer's `mcp` on the thread — and inheriting it is the common case, so
 * A-20 shipped with only that and recorded the gap.  What it could not express
 * is LOWERING one afterwards: a supervisor that wants to hand a subtree less
 * authority than it holds had to have been configured with less, which means
 * deciding the whole hierarchy before building any of it.
 *
 * Same rule as the priority it bounds, and for the same reason: the new
 * ceiling may not exceed the AUTHORITY's ceiling.  Otherwise a thread could
 * raise its own MCP to 255 and then grant itself any priority — which is the
 * exact starvation A-20 closed, reached one step further round.
 *
 * `authority_cptr == 0` means "myself", which is not a loophole: a thread's
 * own ceiling bounds it, so self-authorised the operation can only ever lower
 * or preserve.
 *
 * RIGHT_WRITE on the target (it is a change), RIGHT_READ on the authority
 * (it is only being consulted) — the same split `TCB_SetPriority` uses.
 */
uint64_t sys_tcb_set_mcpriority(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint8_t mcp = (uint8_t)(arg1 & 0xFFu);
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint8_t ceiling;
    if (arg2 == 0u) {
        ceiling = caller->mcp;
    } else {
        struct task *auth; iris_rights_t auth_rights;
        iris_error_t aerr = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg2,
                                        RIGHT_READ, &auth, &auth_rights);
        if (aerr != IRIS_OK) return syscall_err(aerr);
        ceiling = auth->mcp;
        kobject_release(&auth->base);
    }
    if (mcp > ceiling) return syscall_err(IRIS_ERR_ACCESS_DENIED);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    target->mcp = mcp;
    /*
     * A running priority above the new ceiling is brought down with it.
     *
     * Leaving it would make the ceiling a rule about FUTURE grants only, and
     * the thread would keep running at an authority its supervisor has just
     * taken away — which is the difference between lowering a ceiling and
     * asking politely.
     */
    if (target->priority > mcp) target->priority = mcp;
    kobject_release(&target->base);
    return 0;
}

/*
 * sys_domain_set(auth_cptr, tcb_cptr, domain) — seL4's `seL4_DomainSet_Set`.
 *
 * Places a THREAD in a scheduling domain.  Domains are the top-level time
 * partition: a fixed schedule says which domain owns the CPU for how long, and
 * a thread runs only while its own domain is current — whatever its priority,
 * and whatever any other domain's threads are doing.
 *
 * `auth_cptr` is the DOMAIN CONTROL capability and it is checked first,
 * exactly as `SchedControl` is for a budget (A-20).  A separate authority from
 * the TCB capability on purpose: holding a thread lets you order it within the
 * time you were given, and moving it into somebody else's time is a different
 * question.  A supervisor that may configure its children must not be able to
 * move one into another partition's slot just because it can set its priority.
 *
 * RIGHT_WRITE on the target: this changes when it runs.
 */
uint64_t sys_domain_set(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!syscall_has_bootcap(caller, arg0, IRIS_BOOTCAP_DOMAIN_CONTROL))
        return syscall_err(IRIS_ERR_ACCESS_DENIED);

    if (arg2 >= IRIS_NUM_DOMAINS) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg1,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) {
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }

    int moved = (target->domain != (uint8_t)arg2);
    sched_set_domain(target, (uint8_t)arg2);

    /* A thread that moved itself out of the current domain is no longer
     * entitled to the CPU.  Mark rather than switch: the dispatcher decides. */
    if (moved && target == caller) caller->need_resched = 1;

    kobject_release(&target->base);
    return syscall_ok_u64(0);
}

uint64_t sys_tcb_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (target->terminal) { kobject_release(&target->base); return 0; /* already gone */ }
    /* Step 0: nothing is executing in an unconfigured TCB — refuse. */
    if (!target->configured) { kobject_release(&target->base); return syscall_err(IRIS_ERR_NOT_SUPPORTED); }

    int is_self = (target == caller);

    if (is_self) {
        /*
         * The reference goes first here, and it has to: `task_exit_current`
         * does not return, so anything after it never runs.  It is safe for
         * the one reason that makes the self case different — this thread IS
         * the target, and a thread executing on a processor cannot be torn
         * down under itself.  `task->on_cpu` is exactly that guarantee.
         */
        kobject_release(&target->base);
        task_exit_current(); /* does not return */
    }

    /*
     * Hold the resolve's reference ACROSS the kill (SMP roadmap §9.3 step 5).
     *
     * It used to be released first, on the line above the call, and on one
     * processor that was safe by an argument nobody wrote down: nothing else
     * could destroy the object between the release and the use, because
     * nothing else was running.  With four processors it can — another core
     * killing the same thread completes the teardown and drops the CSpace
     * slot's reference in that window, and this core then walks a TCB whose
     * storage has already gone back to its Untyped, zero-filled.
     *
     * T350 is the test: four cores calling Exit on the same four threads.  The
     * symptom was `kobject_retain: resurrect from refcount 0` on an object
     * whose type field read 0 — a type nothing creates, because the header had
     * been cleared by the free.
     *
     * The fix is the obvious one and the reason references exist: keep it
     * until the last use.  Holding it across the teardown only delays the
     * final destroy to this release.
     */
    task_kill_external(target);
    kobject_release(&target->base);
    return 0;
}

uint64_t sys_tcb_get_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t info_uptr = arg1;
    (void)arg2;
    if (!info_uptr) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

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
    info.home_cpu = target->home_cpu;
    info._pad[0]  = 0;
    irq_spinlock_unlock(&target->obj_lock, flags);
    kobject_release(&target->base);

    if (!copy_to_user_checked(info_uptr, &info, (uint32_t)sizeof(info)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return 0;
}

/*
 * SYS_TCB_SET_IPC_BUFFER(tcb_cptr, frame_cptr, uvaddr) — ledger D-4.
 *
 * seL4's `seL4_TCB_SetIPCBuffer`, and the reason IRIS needs it: a message's
 * bulk payload is staged today in 256 bytes that live INSIDE every TCB.  The
 * user did not choose that size, did not pay for that memory, and cannot name
 * it with a capability — three properties the charter denies the kernel
 * everywhere else, surviving here only because IPC predates the capability
 * model.  A registered frame fixes all three at once: the size is the frame's,
 * the memory came out of the caller's Untyped, and the buffer IS a capability.
 *
 * It also removes work from the IPC path rather than adding it.  The staging
 * path copies user → kernel at send, kernel → kernel at rendezvous, kernel →
 * user at receive, and validates a user pointer on two of those.  Two threads
 * with registered buffers exchange one copy, frame to frame, through the
 * kernel's own window — no user pointer is named, so none can be revoked
 * between the check and the copy.
 *
 * Authority: WRITE on the TCB (this changes how a thread receives messages)
 * and READ|WRITE on the frame (the kernel both reads and writes it).  A
 * registration REPLACES any previous one; passing IRIS_CPTR_NULL as the frame
 * unregisters, which is how a thread gives the page back before deleting it.
 *
 * The frame must be at least one page.  `uvaddr` must be page-aligned and a
 * user address: the kernel does not use it for the transfer, but a
 * registration whose owner cannot say where the page is mapped is a mistake,
 * and refusing it here is cheaper than debugging it in a service.
 */
/*
 * How many threads currently hold a registered IPC buffer.
 *
 * A live gauge, not a counter of events: it rises on registration and falls on
 * replacement, unregistration and thread teardown.  It exists because the
 * migration off `ipc_kbuf` is the kind that fails SILENTLY — a service whose
 * registration is refused simply keeps working on the staging path, and the
 * whole suite passes either way.  That happened, on the first service tried,
 * and only a deliberate check caught it.  So the fact is measured from ring 3
 * and asserted (T313), and a service that stops registering has to explain
 * itself instead of quietly regressing.
 */
static _Atomic uint32_t ipc_buffers_live = 0;

uint32_t ipc_buffers_registered(void) {
    return atomic_load_explicit(&ipc_buffers_live, memory_order_relaxed);
}

void ipc_buffer_gauge_drop(void) {
    atomic_fetch_sub_explicit(&ipc_buffers_live, 1u, memory_order_relaxed);
}

uint64_t sys_tcb_set_ipc_buffer(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *caller = task_current();
    if (!caller || !caller->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *target; iris_rights_t rights;
    iris_error_t err = tcb_resolve(caller->cspace_root, (iris_cptr_t)arg0,
                                   RIGHT_WRITE, &target, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    struct KFrame *fr = 0;
    if (arg1 != IRIS_CPTR_NULL) {
        struct KObject *f_obj; iris_rights_t f_rights;
        err = cspace_resolve_only_obj(caller->cspace_root, (iris_cptr_t)arg1,
                                      RIGHT_NONE, KOBJ_FRAME,
                                      &f_obj, &f_rights);
        if (err != IRIS_OK) {
            kobject_release(&target->base);
            return syscall_err(err);
        }
        if (!rights_check(f_rights, RIGHT_READ | RIGHT_WRITE)) {
            kobject_release(f_obj); kobject_release(&target->base);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        fr = (struct KFrame *)f_obj;
        if (fr->size < 4096u || (arg2 & 0xFFFu) != 0u || arg2 == 0u ||
            arg2 >= 0x0000800000000000ULL) {
            kobject_release(f_obj); kobject_release(&target->base);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
    } else if (arg2 != 0u) {
        /* Unregistering names no address. */
        kobject_release(&target->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    struct KFrame *old;
    uint64_t irqfl = irq_spinlock_lock(&target->obj_lock);
    old = target->ipc_buffer;
    if (fr) {
        kobject_retain(&fr->base);
        kobject_active_retain(&fr->base);
    }
    target->ipc_buffer        = fr;
    target->ipc_buffer_uvaddr = fr ? arg2 : 0u;
    irq_spinlock_unlock(&target->obj_lock, irqfl);

    if (fr && !old)  atomic_fetch_add_explicit(&ipc_buffers_live, 1u,
                                               memory_order_relaxed);
    if (!fr && old)  atomic_fetch_sub_explicit(&ipc_buffers_live, 1u,
                                               memory_order_relaxed);

    if (old) { kobject_active_release(&old->base); kobject_release(&old->base); }
    if (fr)  kobject_release(&fr->base);   /* the resolve's ref */
    kobject_release(&target->base);
    return syscall_ok_u64(0);
}
