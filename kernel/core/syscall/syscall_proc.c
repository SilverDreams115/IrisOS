#include "syscall_priv.h"
#include <iris/fault_proto.h>

uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (t && t->process)
        t->process->exit_code = (uint32_t)arg0;
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
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    handle_id_t h;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    const iris_rights_t rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    /* Stage 4: arg0 is a destination slot (RETYPE2 packing).  The process
     * object is borrowed from the caller's own task, so publish_slot's
     * consuming release needs a reference of its own. */
    /* Stage 4: a destination slot is REQUIRED — the handle result is retired. */
    if (arg0 == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    (void)h;
    kobject_retain(&t->process->base);
    iris_error_t pe = syscall_publish_slot(t, &t->process->base, rights,
                                           arg0, 0, 0);
    if (pe != IRIS_OK) return syscall_err(pe);
    return syscall_ok_u64(0);
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
    /* A1 Increment 2a: dual resolver — the process may be a CPtr slot or a
     * handle.  RIGHT_NONE defers to the existing rights checks below. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_PROCESS, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    int alive = kprocess_is_alive((struct KProcess *)obj);
    kobject_release(obj);
    return syscall_ok_u64(alive ? 1 : 0);
}


/*
 * sys_process_watch(proc_handle, notify_handle, signal_bits) → 0 or iris_error_t
 *
 * Registers a single process-exit watch for proc_handle. When the target
 * process tears down, the kernel signals signal_bits on notify_handle
 * (Phase 13 / Track B — death is a KNotification signal, no longer a
 * PROC_EVENT_MSG_EXIT KChannel message).  The watcher identifies the dead
 * process by which bit is set and queries SYS_PROCESS_EXIT_CODE / STATUS
 * for detail.  signal_bits must be non-zero.
 */
uint64_t sys_process_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    struct KObject *proc_obj;
    struct KObject *notif_obj;
    iris_rights_t proc_rights;
    iris_rights_t notif_rights;
    iris_error_t r;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* A1 Increment 2a: dual resolver on the watched process. */
    r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                     RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(proc_rights, RIGHT_READ)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Step 4: the watched process already resolved either way while the
     * notification stayed handle-only — half a migration, so a caller holding
     * its notification in CSpace could not arm a watch at all.
     *
     * No extra release is needed and adding one is a refcount underflow:
     * unlike its sibling resolvers, the dual object resolver drops the
     * traversal's active ref itself and hands back a LIFECYCLE-ONLY reference,
     * exactly what the handle read yielded.  The type check moves into the
     * resolver, which returns WRONG_TYPE for the same case. */
    r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg1, RIGHT_NONE,
                                     KOBJ_NOTIFICATION, &notif_obj, &notif_rights);
    if (r != IRIS_OK) {
        kobject_release(proc_obj);
        return syscall_err(r);
    }
    if (!rights_check(notif_rights, RIGHT_WRITE)) {
        kobject_release(proc_obj);
        kobject_release(notif_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    r = kprocess_watch_exit((struct KProcess *)proc_obj,
                            (struct KNotification *)notif_obj,
                            arg2);
    kobject_release(proc_obj);
    kobject_release(notif_obj);
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
    /* A1 Increment 2a: dual resolver on the kill target. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_PROCESS, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

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

    /*
     * No live threads.  Two different states share that description, and only
     * one of them is this syscall's to clean up.
     *
     * Stage 6 Step 2: a process created and NEVER STARTED used to be
     * unreclaimable — kill found no threads to stop, and the creation
     * reference that outlives capabilities was only ever dropped by the LAST
     * THREAD exiting.  Its address space, its PML4 and (since page tables are
     * charged to an Untyped) its budget stayed pinned for the life of the
     * system.  Tearing it down here is what kill already means for a running
     * process, applied to one that never ran.
     *
     * A process whose last thread is EXITING looks identical through
     * thread_count, and tearing that one down here is not a second cleanup
     * but a concurrent one: task_execution_teardown_off_cpu decrements
     * thread_count with interrupts enabled and only then runs
     * kprocess_teardown, which sets teardown_complete at its very end.  A kill
     * landing in that window passes both checks and races the exiting thread
     * through kprocess_reap_address_space — both read aspace_reaped == 0, both
     * release every bootstrap KFrame, and both destroy the same cr3.  So the
     * gate is threads_ever, which distinguishes the two states that
     * thread_count cannot: a process that has never had a thread has no other
     * teardown path, and nothing can be racing us through one.
     */
    if (!kprocess_is_alive(target)) {
        if (!target->threads_ever && !kprocess_teardown_complete(target)) {
            kprocess_teardown(target, 0);
            kprocess_reap_address_space(target);
            kprocess_free(target);
        }
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
    uint64_t dest        = arg1;   /* Step 4: cnode | slot<<32 */
    uint64_t vspace_cptr = arg2;   /* Stage 6-pure Step 4: the address space */
    uint64_t cnode_cptr  = arg3;   /* Stage 6-pure Step 5: the root CSpace */
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject   *auth_obj;
    iris_rights_t     auth_rights;
    /* Stage 5 Step 2: the authority is the PROCESS CONTROL capability, and
     * nothing else — creating a process no longer travels with initrd access,
     * debug authority or the framebuffer. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_is((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_PROC_CONTROL)) {
        kobject_release(auth_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth_obj);

    /*
     * Stage 6-pure Step 4: the caller supplies the ADDRESS SPACE.
     *
     * It used to supply a budget and the kernel built one out of it — carving
     * a PML4, a VSpace header, and every level underneath.  The holder paid
     * for an address space it could not name until the process existed, could
     * not inspect, and could not have built differently.  Now it retypes a
     * KOBJ_VSPACE from its own Untyped (RETYPE2, 4096) and hands that over,
     * which is seL4's shape: a process is COMPOSED from objects its creator
     * made, not conjured from a quota.
     *
     * The budget for what remains kernel-side — the KProcess object and its
     * root CNode — is the one the address space itself came from.  Deriving it
     * rather than taking a second argument keeps one region per child, so a
     * holder that RESETs it gets all of the child back with nothing to
     * remember.
     */
    struct KVSpace *vs = 0;
    {
        iris_rights_t vr;
        iris_error_t  ve = cspace_resolve_only_vspace(t->cspace_root,
                               (iris_cptr_t)vspace_cptr, RIGHT_WRITE, &vs, &vr);
        if (ve != IRIS_OK)
            return syscall_err(ve == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : ve);
    }
    /* One address space, one process: teardown is per-process, so two
     * processes sharing a walk would each tear down the other's. */
    iris_error_t be = kvspace_bind(vs);
    if (be != IRIS_OK) {
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        return syscall_err(be);
    }
    struct KUntyped *pool = vs->pt_pool;
    if (!pool) {
        /* A VSpace with no pool is the root task's, which is not something a
         * caller can name — but the check is cheap and the alternative is a
         * NULL deref in kprocess_alloc_from. */
        kvspace_unbind(vs);
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /*
     * Stage 6-pure Step 5: the caller supplies the root CSpace too.
     *
     * With this, everything a process IS comes from objects its creator
     * retyped — an address space and a CSpace — which is the shape seL4 gives
     * seL4_TCB_Configure.  The spawner also chooses how wide its child's CSpace
     * is, where the kernel used to pick 256 slots for everyone.
     */
    struct KCNode *cn = 0;
    {
        iris_rights_t cr;
        iris_error_t  ce = cspace_resolve_only_cnode(t->cspace_root,
                               (iris_cptr_t)cnode_cptr, RIGHT_WRITE, &cn, &cr);
        if (ce != IRIS_OK) {
            kvspace_unbind(vs);
            kobject_active_release(&vs->base); kobject_release(&vs->base);
            return syscall_err(ce == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : ce);
        }
    }
    /* One CNode, one root: teardown empties a root's slots before dropping its
     * refs (a CSpace may name itself), so a second process sharing it would
     * have its CSpace emptied by the first one's death. */
    iris_error_t cbe = kcnode_bind_root(cn);
    if (cbe != IRIS_OK) {
        kvspace_unbind(vs);
        kobject_active_release(&cn->base); kobject_release(&cn->base);
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        return syscall_err(cbe);
    }

    /*
     * Past here the process OWNS both, and both are spent whatever happens
     * next: a KProcess that reaches teardown invalidates its address space and
     * empties its root CSpace, so neither can serve another process again.
     * Before here nothing owns them, and a claim taken for a composition that
     * did not happen has to go back — otherwise a caller whose create failed
     * for want of memory is left holding two capabilities that will answer
     * BUSY for the rest of the system's life, with no syscall able to clear
     * the flags.
     */
    struct KProcess *proc = kprocess_alloc_from(pool, cn);
    if (!proc) {
        kcnode_unbind_root(cn);
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        kvspace_unbind(vs);
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);

    /* The VSpace is the process's now: it holds the reference that keeps the
     * address space alive, and drops it in kprocess_reap_address_space. */
    kobject_retain(&vs->base);
    proc->vspace   = vs;
    proc->cr3      = vs->cr3;
    kobject_active_release(&vs->base);
    kobject_release(&vs->base);

    /* Step 4: arg1 names a destination CSpace slot; the new process cap is
     * published there as an MDB child of the spawn-cap slot that authorised
     * the create, so it is revocable by the grantor.
     *
     * Stage 4: it is REQUIRED — the handle result is retired, so 0 names no
     * destination and is an error rather than a fall back to the other
     * namespace. */
    if (dest == 0u) { kprocess_free(proc); return syscall_err(IRIS_ERR_INVALID_ARG); }
    {
        struct KCNode *auth_cn = 0; uint32_t auth_idx = 0;
        if (cspace_value_is_cptr((iris_cptr_t)arg0) &&
            cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                                &auth_cn, &auth_idx) != IRIS_OK)
            auth_cn = 0;
        kobject_retain(&proc->base);   /* publish consumes a ref; keep the initial one */
        iris_error_t pe = syscall_publish_slot(t, &proc->base,
                                               RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE |
                                               RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_ROUTE,
                                               dest, auth_cn, auth_idx);
        if (auth_cn) {
            kobject_active_release(&auth_cn->base);
            kobject_release(&auth_cn->base);
        }
        if (pe != IRIS_OK) { kprocess_free(proc); return syscall_err(pe); }
        /* The initial ref is the thread-lifecycle reference and is NOT
         * released here; reap_dead_task_off_cpu drops it via kprocess_free
         * once the last thread exits. */
        return syscall_ok_u64(0);
    }
}


/*
 * SYS_THREAD_START (58) — RETIRED (Stage 7).  Number permanently reserved;
 * returns IRIS_ERR_NOT_SUPPORTED.
 *
 * It carved a spawned process's FIRST thread out of the kernel's static task
 * pool — the last execution path where a thread existed because the kernel had
 * a free slot rather than because somebody held the memory and the authority.
 * It survived Stage 5's retirement of SYS_THREAD_CREATE for one reason: a
 * spawner could not name the CSpace and VSpace its child would run in, because
 * the kernel carved both inside SYS_PROCESS_CREATE and never handed them out.
 *
 * Stage 6-pure Step 4/5 made the spawner RETYPE both and pass them in, so it
 * holds them from before the child exists.  What replaces this is the same
 * four steps a thread of your own takes, and every one names a capability:
 * RETYPE2(KOBJ_TCB) out of the child's budget, SYS_TCB_CONFIGURE with the
 * child's CSpace and VSpace, SYS_TCB_WRITE_REGS to say where it starts,
 * SYS_TCB_RESUME to start it.
 *
 * With it goes the last caller of task_thread_create.
 */
uint64_t sys_thread_start(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/* ── Threading (D2) ──────────────────────────────────────────────── */

/*
 * SYS_THREAD_CREATE (48) — RETIRED (Stage 5 Step 4).  Number permanently
 * reserved; returns IRIS_ERR_NOT_SUPPORTED.
 *
 * It carved a thread out of the kernel's static task pool and returned a
 * global thread id.  Nothing about it was a capability operation: no
 * capability authorised it, no Untyped paid for the storage, and the identity
 * it handed back was an index into a kernel array — the shape charter §3.4/§3.5
 * forbid (a global identifier standing in for a capability).  A thread existed
 * because the kernel had a free slot, not because the caller held the memory
 * and the authority.
 *
 * Its replacement is the seL4 sequence, and every step names a capability:
 * RETYPE2(KOBJ_TCB) carves the storage out of an Untyped the caller holds,
 * SYS_TCB_CONFIGURE gives it the CSpace and VSpace it runs in — as capability
 * arguments — SYS_TCB_WRITE_REGS says where it starts, and SYS_TCB_RESUME
 * starts it.  What comes back is a capability in a slot, not an id.
 *
 * SYS_THREAD_START (58) remains: it starts the FIRST thread of a freshly
 * created process, which still comes from the pool because a spawner cannot
 * yet name its child's CSpace and VSpace (process server, Stage 7).
 */
uint64_t sys_thread_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


uint64_t sys_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_exit_current();
    return 0;  /* unreachable */
}


uint64_t sys_process_exit_code(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    /* A1 Increment 2a: dual resolver on the queried process. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_PROCESS, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *proc = (struct KProcess *)obj;
    if (kprocess_is_alive(proc)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WOULD_BLOCK);
    }
    uint32_t code = proc->exit_code;
    kobject_release(obj);
    return syscall_ok_u64((uint64_t)code);
}


/*
 * sys_process_fault_info(proc_handle, out_uptr) → 0 or iris_error_t
 *
 * Phase 13 (Track I): reads the last fault recorded for proc_handle (or self when
 * proc_handle == HANDLE_INVALID) into a 32-byte user buffer laid out per
 * iris/fault_proto.h (FAULT_OFF_VECTOR/TASK_ID/RIP/ERROR/CR2).  The exception
 * handler calls this after its KNotification fires.  Returns IRIS_ERR_WOULD_BLOCK
 * if no fault is pending.  Requires RIGHT_READ on a non-self proc_handle.
 */
uint64_t sys_process_fault_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, FAULT_MSG_LEN))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KProcess *proc;
    struct KObject  *obj = 0;
    if ((handle_id_t)arg0 == HANDLE_INVALID) {
        proc = t->process;
        kobject_retain(&proc->base);
    } else {
        iris_rights_t rights;
        /* A1 Increment 2a: dual resolver on the non-self process.  The self
         * path above owns arg0 == 0 (HANDLE_INVALID == CPTR_NULL). */
        iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                     RIGHT_NONE, KOBJ_PROCESS, &obj, &rights);
        if (r != IRIS_OK) return syscall_err(r);
        if (!rights_check(rights, RIGHT_READ)) {
            kobject_release(obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        proc = (struct KProcess *)obj;
    }

    uint8_t buf[FAULT_MSG_LEN];
    for (uint32_t i = 0; i < FAULT_MSG_LEN; i++) buf[i] = 0;

    spinlock_lock(&proc->base.lock);
    int valid = proc->fault_valid;
    uint32_t vector = proc->fault_vector, task_id = proc->fault_task_id,
             error = proc->fault_error, seq = proc->fault_seq;
    uint64_t rip = proc->fault_rip, cr2 = proc->fault_cr2;
    spinlock_unlock(&proc->base.lock);
    kobject_release(&proc->base);

    if (!valid) return syscall_err(IRIS_ERR_WOULD_BLOCK);

    for (uint32_t i = 0; i < 4u; i++) buf[FAULT_OFF_VECTOR + i]  = (uint8_t)(vector >> (i * 8));
    for (uint32_t i = 0; i < 4u; i++) buf[FAULT_OFF_TASK_ID + i] = (uint8_t)(task_id >> (i * 8));
    for (uint32_t i = 0; i < 8u; i++) buf[FAULT_OFF_RIP + i]     = (uint8_t)(rip >> (i * 8));
    for (uint32_t i = 0; i < 4u; i++) buf[FAULT_OFF_ERROR + i]   = (uint8_t)(error >> (i * 8));
    for (uint32_t i = 0; i < 4u; i++) buf[FAULT_OFF_SEQ + i]     = (uint8_t)(seq >> (i * 8));
    for (uint32_t i = 0; i < 8u; i++) buf[FAULT_OFF_CR2 + i]     = (uint8_t)(cr2 >> (i * 8));

    if (!copy_to_user_checked(arg1, buf, FAULT_MSG_LEN))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(IRIS_OK);
}

/*
 * sys_resource_info(proc_handle, out_uptr) → 0 or iris_error_t   (Phase 29)
 *
 * Read-only resource-accounting snapshot: per-domain usage/limit/high-water for
 * a KProcess (self when proc_handle == HANDLE_INVALID) plus system-wide
 * failed-charge / rollback / kslab gauges.  A read-only oracle — any rights on a
 * non-self process cap suffice.  Additive and versioned: the caller sets
 * struct_size; the kernel writes at most that many bytes.
 */
uint64_t sys_resource_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    /* Phase S2 C.1: arg2 = caller's declared buffer size (0 = legacy full).
     * The kernel writes at most min(user_size, sizeof(info)) — prefix
     * compatible — so a smaller/older struct can never be overflowed. */
    uint32_t user_size = (uint32_t)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct iris_resource_info info;
    for (uint32_t i = 0; i < (uint32_t)sizeof(info); i++) ((uint8_t *)&info)[i] = 0;

    struct KProcess *proc;
    struct KObject  *obj = 0;
    if ((handle_id_t)arg0 == HANDLE_INVALID) {
        proc = t->process;
        kobject_retain(&proc->base);
    } else {
        iris_rights_t rights;
        iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                     RIGHT_NONE, KOBJ_PROCESS, &obj, &rights);
        if (r != IRIS_OK) return syscall_err(r);
        proc = (struct KProcess *)obj;
    }

    spinlock_lock(&proc->base.lock);
    info.vmos_usage    = proc->owned_vmos;
    info.vmos_hwm      = proc->owned_vmos_hwm;
    /* Phase S1: notification quota retired — notifications are Untyped-funded.
     * The ABI fields remain (additive contract) and report 0. */
    info.notifs_usage  = 0u;
    info.notifs_hwm    = 0u;
    info.pages_usage   = proc->phys_pages_charged;
    info.pages_limit   = proc->phys_pages_limit;
    info.pages_hwm     = proc->phys_pages_hwm;
    spinlock_unlock(&proc->base.lock);
    kobject_release(&proc->base);

    info.version       = IRIS_RESOURCE_INFO_VERSION;
    info.struct_size   = (uint32_t)sizeof(info);
    info.vmos_limit    = KPROCESS_VMO_QUOTA;
    info.notifs_limit  = 0u; /* Phase S1: no notification quota — Untyped is the budget */
    info.global_failed_charges = kprocess_quota_failed_count();
    info.global_rollbacks      = kprocess_quota_rollback_count();
    info.kslab_used_bytes  = kslab_used_bytes();
    info.kslab_total_bytes = kslab_total_bytes();
    info.kslab_hwm_bytes   = kslab_used_bytes();   /* bump-only: used == hwm */
    info.kslab_alloc_failures = kslab_fail_count();

    /* Phase S2 C.1: prefix-compatible, bounded by the caller's declared size.
     * user_size == 0 keeps the legacy full-struct contract for callers that
     * pre-date C.1 (they must size their buffer to sizeof(info)); a non-zero
     * user_size clamps the write so an older/smaller struct cannot overflow. */
    uint32_t ksize = (uint32_t)sizeof(info);
    uint32_t want  = (user_size != 0u && user_size < ksize) ? user_size : ksize;
    if (!user_range_writable(arg1, want))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!copy_to_user_checked(arg1, (const uint8_t *)&info, want))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_ok_u64(IRIS_OK);
}
