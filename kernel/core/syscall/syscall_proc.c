#include "syscall_priv.h"
#include <iris/fault_proto.h>

uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    /* Stage 7 Step 10: the code belongs to the execution that produced it.
     * Step 15: and only to it — the process copy was kept while
     * SYS_PROCESS_EXIT_CODE could still be asked, and that syscall is
     * retired. */
    if (t) t->exit_code = (uint32_t)arg0;
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


/*
 * SYS_PROCESS_SELF — RETIRED (Stage 7 Step 15).
 *
 * It handed a task a capability to its own KProcess, for the things that used
 * to need one: minting into your own CSpace, mapping into your own address
 * space, being named as a payer, watching yourself.  Every one of those was
 * re-aimed at the object it actually concerns — Step 9 gave `SYS_CSPACE_MINT`
 * a destination CNode and `SYS_VMO_MAP_INTO` a VSpace, Steps 10-13 moved
 * watching, killing and status onto the thread — and `SYS_CSPACE_SELF` /
 * `SYS_VSPACE_SELF` / `SYS_TCB_SELF` hand out the three self-capabilities that
 * are left.  Nothing in the tree called this any more.
 */
uint64_t sys_process_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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
/*
 * SYS_PROCESS_STATUS — RETIRED (Stage 7 Step 13).
 *
 * "Is it alive" answered about a PROCESS, which is a fact derived from its
 * threads: kprocess_is_alive is thread_count != 0.  Asking the derived object
 * meant a supervisor held a process capability for a question its threads
 * already answer, and answered it with less than it knew — one bit where
 * SYS_TCB_GET_INFO reports the state.
 *
 * A supervisor holds its children's first thread (svc_load_minted_ws's
 * `keep_tcb_dest`) because that is what it watches, kills and arms faults on.
 * Reading that thread's state is the same question asked of an object it
 * already has.
 */
uint64_t sys_process_status(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/*
 * SYS_PROCESS_WATCH (29) — RETIRED (Stage 7 Step 10).  Number
 * permanently reserved; answers IRIS_ERR_NOT_SUPPORTED.
 *
 * It reported a process's death notification, which meant a supervisor needed
 * authority over an object it did not create to learn about an execution it
 * did.  A supervisor HAS the thread — it retyped the TCB and configured it —
 * and every service in the tree is single-threaded, so SYS_TCB_WATCH and
 * SYS_TCB_EXIT_CODE are not approximations of the process event: they are that
 * event, named by the thing that produces it.
 *
 * The array of four watch slots went with it.  A process could be watched by
 * several unrelated holders, so the kernel kept room for them; a thread is
 * watched by whoever holds its TCB, and a second watcher is a second
 * capability rather than a second slot.
 */
uint64_t sys_process_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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
/*
 * SYS_PROCESS_KILL — RETIRED (Stage 7 Step 13).
 *
 * It did two things, and Stage 7 took them apart one at a time.
 *
 * The first was stopping the executions: task_kill_process walked every thread
 * of the target.  A supervisor holds its children's threads now and stops them
 * with SYS_TCB_EXIT, which is the same act named by a capability to the thing
 * it acts on.
 *
 * The second was RECLAMATION, and that is the half that took three steps to
 * remove.  Step 11 moved the address-space teardown into the VSpace's own
 * destructor, so a walk comes down when its last capability does rather than
 * when a process is declared dead.  What was left was a process created and
 * NEVER STARTED: kill found no threads, and its special case tore the object
 * down because nothing else could — SYS_PROCESS_CREATE kept a reference on its
 * own behalf that only the last thread's exit released, and there was no last
 * thread.  Step 13 dropped that reference at create time and gave the join a
 * real one, so a never-started process is destroyed by deleting the last
 * capability to it.
 *
 * With both halves gone this syscall had nothing left that a capability to the
 * thing being acted on could not do.
 */
uint64_t sys_process_kill(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/*
 * SYS_PROCESS_CREATE — RETIRED (Stage 7-proc).
 *
 * It made a KProcess out of an address space and a root CSpace the caller had
 * retyped, bound both to it exclusively, and published a capability the
 * spawner then passed around.  Every reason for that object is gone:
 *
 *   - The AUTHORITY it checked (a spawn capability) was authority to make a
 *     process.  A child is a TCB, a CNode and a VSpace retyped from a budget
 *     the spawner holds, and holding that budget is what authorises retyping.
 *     seL4 has no spawn capability either.
 *   - The BINDING was exclusive because teardown was per-process and a shared
 *     CSpace would have been emptied by the first death.  Teardown is
 *     per-object now, so several threads sharing a CSpace and a VSpace is not
 *     a hazard to refuse — it is the definition of a process.
 *   - The CAPABILITY it published was what a supervisor named.  A supervisor
 *     names the THREAD: killing, watching, reading an exit code, arming faults
 *     and asking whether it is alive have all been thread operations since
 *     Steps 10 through 13, and svc_loader hands back the child's first thread.
 *
 * What is left of "create a process" is what seL4 has: retype the objects,
 * configure a thread with two of them, write its registers, resume it.
 */
uint64_t sys_process_create(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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

/*
 * SYS_PROCESS_EXIT_CODE (70) — RETIRED (Stage 7 Step 10).  Number
 * permanently reserved; answers IRIS_ERR_NOT_SUPPORTED.
 *
 * It reported a process's exit code, which meant a supervisor needed
 * authority over an object it did not create to learn about an execution it
 * did.  A supervisor HAS the thread — it retyped the TCB and configured it —
 * and every service in the tree is single-threaded, so SYS_TCB_WATCH and
 * SYS_TCB_EXIT_CODE are not approximations of the process event: they are that
 * event, named by the thing that produces it.
 *
 * The array of four watch slots went with it.  A process could be watched by
 * several unrelated holders, so the kernel kept room for them; a thread is
 * watched by whoever holds its TCB, and a second watcher is a second
 * capability rather than a second slot.
 */
uint64_t sys_process_exit_code(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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
/*
 * SYS_TCB_FAULT_INFO(tcb_cptr, out_uptr) → 0 or iris_error_t
 *
 * Stage 7 Step 8: read a fault off the THREAD that took it.
 *
 * This was SYS_PROCESS_FAULT_INFO(proc_cptr, out): the record was per-process
 * and so was the question.  Step 6 moved the record onto the execution and
 * Step 7 made the ANSWER name the thread by capability — which left a handler
 * needing a PROCESS capability for one remaining reason: to read the thing it
 * already held the thread for.  A pager held RIGHT_MANAGE over every target
 * purely to ask what had faulted.
 *
 * RIGHT_READ on the thread is the authority, and it is the whole of it.  A
 * thread with no pending fault answers WOULD_BLOCK, which is what a handler
 * polling for delivery wants and is the same answer the process-scoped form
 * gave for an empty record.
 */
uint64_t sys_tcb_fault_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    /* The destination is checked before the record is looked up, so a hostile
     * pointer is INVALID_ARG whether or not a fault happens to be pending —
     * the same order the process-scoped form has always used. */
    if (!user_range_writable(arg1, FAULT_MSG_LEN))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj; iris_rights_t rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                             RIGHT_NONE, KOBJ_TCB, &obj, &rights);
    if (r != IRIS_OK)
        return syscall_err(r == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct task *ft = (struct task *)obj;
    uint8_t buf[FAULT_MSG_LEN];
    for (uint32_t i = 0; i < FAULT_MSG_LEN; i++) buf[i] = 0;

    int      valid  = ft->fault_valid;
    uint32_t vector = ft->fault_vector, task_id = ft->id,
             error  = ft->fault_error,  seq     = ft->fault_seq;
    uint64_t rip    = ft->fault_rip,    cr2     = ft->fault_cr2;
    kobject_release(obj);

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
 * SYS_PROCESS_FAULT_INFO — RETIRED (Stage 7 Step 12).
 *
 * Step 8 kept it for a real principal: a SUPERVISOR watching a child it does
 * not resolve for, holding RIGHT_READ on the process and no capability to any
 * of its threads.  Retiring it then would have left that supervisor unable to
 * ask what had faulted, and iris_test was exactly that supervisor.
 *
 * Step 12 removed the principal rather than the question.  Arming faults is a
 * thread operation now, so a spawner that supervises keeps its child's first
 * thread (svc_load_minted_ws's `keep_tcb_dest`) — which means the supervisor
 * that had only a process capability holds a thread capability, and asks the
 * thread.  Nothing calls this any more.
 *
 * What is left without it is one question, asked of the object that has the
 * answer: SYS_TCB_FAULT_INFO on the execution that faulted.
 */
uint64_t sys_process_fault_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/*
 * SYS_RESOURCE_INFO — RETIRED (Stage 7-mem).
 *
 * It answered "what has this PROCESS spent": a VMO count against a ceiling of
 * 32, a page counter, a notification count retired back in Phase S1, and three
 * gauges that were never per-process at all.
 *
 * Every per-process number in it is gone with the owner relation — a VMO's
 * accounting is the Untyped it was carved from, which SYS_UNTYPED_INFO reports
 * to anyone holding that budget, and which is the only accounting a capability
 * system needs: a ceiling somebody delegated rather than one the kernel
 * invented.
 *
 * The three global ones — the kernel slab's occupancy and the failed-charge
 * and rollback counters — were facts about the KERNEL that happened to be
 * carried here.  They moved to SYS_UNTYPED_QUERY's GLOBAL kind, where the rest
 * of the global instrumentation already lives, so retiring this costs the
 * drift tests nothing.
 */
uint64_t sys_resource_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}
