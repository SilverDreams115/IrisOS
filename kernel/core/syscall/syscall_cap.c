#include "syscall_priv.h"



uint64_t sys_handle_close(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(handle_table_close(&t->process->handle_table, (handle_id_t)arg0));
}


/* ── Handle duplication ───────────────────────────────────────────── */

/*
 * sys_handle_dup(src_handle, new_rights) → new_handle_id
 *
 * Duplicates src_handle into a new handle in the caller's own table.
 * new_rights must be a subset of the caller's existing rights on src_handle.
 * Pass RIGHT_SAME_RIGHTS to keep the same rights (rights_reduce handles this).
 * The reduced rights set must not collapse to RIGHT_NONE.
 * Requires RIGHT_DUPLICATE on the source handle.
 */
uint64_t sys_handle_dup(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (!rights_check(rights, RIGHT_DUPLICATE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t new_rights = rights_reduce(rights, (iris_rights_t)arg1);
    if (new_rights == RIGHT_NONE) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    /* Fase 9: duplication preserves the badge (a copy keeps its identity). */
    uint64_t      src_badge  = handle_table_get_badge(&t->process->handle_table,
                                                      (handle_id_t)arg0);
    handle_id_t   new_h      = handle_table_insert_badged(&t->process->handle_table,
                                                          obj, new_rights, src_badge);
    kobject_release(obj);

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)new_h);
}


/* ── Handle transfer ──────────────────────────────────────────────── */

/*
 * sys_handle_transfer(src_handle, dest_proc_handle, new_rights) → new_handle_id
 *
 * Moves src_handle from caller's table into dest process's table.
 * Requires RIGHT_TRANSFER on src_handle.
 * Requires RIGHT_MANAGE on dest_proc_handle.
 * new_rights must be a subset of src rights (or RIGHT_SAME_RIGHTS).
 * The reduced rights set must not collapse to RIGHT_NONE.
 * Consumes (closes) src_handle in caller's table on success.
 * Returns new handle_id in dest's table, or iris_error_t cast to uint64_t on failure.
 */
uint64_t sys_handle_transfer(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Get source object — requires RIGHT_TRANSFER */
    struct KObject *src_obj;
    iris_rights_t   src_rights;
    iris_error_t r = handle_table_get_object(&caller->process->handle_table, (handle_id_t)arg0,
                                             &src_obj, &src_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(src_rights, RIGHT_TRANSFER)) {
        kobject_release(src_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Get destination KProcess — requires RIGHT_MANAGE.
     * A1 Increment 2a: dual resolver — CPtr slot or handle. */
    struct KObject *dest_obj;
    iris_rights_t   dest_rights;
    r = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)arg1,
                                     RIGHT_NONE, KOBJ_PROCESS, &dest_obj, &dest_rights);
    if (r != IRIS_OK) { kobject_release(src_obj); return syscall_err(r); }
    if (!rights_check(dest_rights, RIGHT_MANAGE)) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *dest_proc = (struct KProcess *)dest_obj;
    if (!kprocess_is_alive(dest_proc)) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    iris_rights_t new_rights = rights_reduce(src_rights, (iris_rights_t)arg2);
    if (new_rights == RIGHT_NONE) {
        kobject_release(src_obj);
        kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    handle_id_t   new_h      = handle_table_insert(&dest_proc->handle_table,
                                                   src_obj, new_rights);
    kobject_release(src_obj);
    kobject_release(dest_obj);

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);

    /* Consume the source handle — transfer is move, not copy */
    handle_table_close(&caller->process->handle_table, (handle_id_t)arg0);
    return syscall_ok_u64((uint64_t)new_h);
}


/* ── Hardware capability creation (C2: policy moved to svcmgr) ──────── */

uint64_t sys_cap_create_irqcap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t    cap_h   = (handle_id_t)arg0;
    uint8_t        irq_num = (uint8_t)(arg1 & 0xFFu);
    struct task   *t       = task_current();
    struct KObject *auth   = 0;
    iris_rights_t   auth_r;
    struct KIrqCap *irqcap;
    handle_id_t     out_h;
    (void)arg2;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (irq_num > 15u)     return syscall_err(IRIS_ERR_INVALID_ARG);

    {
        /* Fase 13: dual resolver — HW cap may be a CPtr slot or a handle. */
        iris_error_t ar = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)cap_h,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth, &auth_r);
        if (ar == IRIS_ERR_WRONG_TYPE) ar = IRIS_ERR_ACCESS_DENIED;
        if (ar != IRIS_OK) return syscall_err(ar);
    }
    if (!kbootcap_allows((struct KBootstrapCap *)auth, IRIS_BOOTCAP_HW_ACCESS)) {
        kobject_release(auth);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth);

    irqcap = kirqcap_alloc(irq_num);
    if (!irqcap) return syscall_err(IRIS_ERR_NO_MEMORY);

    out_h = handle_table_insert(&t->process->handle_table, &irqcap->base,
                                RIGHT_ROUTE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (out_h == HANDLE_INVALID) {
        kirqcap_free(irqcap);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&irqcap->base);
    return syscall_ok_u64((uint64_t)out_h);
}


uint64_t sys_cap_create_ioport(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t    cap_h = (handle_id_t)arg0;
    uint16_t       base  = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t       count = (uint16_t)(arg2 & 0xFFFFu);
    struct task   *t     = task_current();
    struct KObject *auth = 0;
    iris_rights_t   auth_r;
    struct KIoPort *ioport;
    handle_id_t     out_h;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (count == 0u || (uint32_t)base + count > 0x10000u)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!kioport_in_whitelist(base, count))
        return syscall_err(IRIS_ERR_ACCESS_DENIED);

    {
        /* Fase 13: dual resolver — HW cap may be a CPtr slot or a handle. */
        iris_error_t ar = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)cap_h,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth, &auth_r);
        if (ar == IRIS_ERR_WRONG_TYPE) ar = IRIS_ERR_ACCESS_DENIED;
        if (ar != IRIS_OK) return syscall_err(ar);
    }
    if (!kbootcap_allows((struct KBootstrapCap *)auth, IRIS_BOOTCAP_HW_ACCESS)) {
        kobject_release(auth);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth);

    ioport = kioport_alloc(base, count);
    if (!ioport) return syscall_err(IRIS_ERR_NO_MEMORY);

    out_h = handle_table_insert(&t->process->handle_table, &ioport->base,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (out_h == HANDLE_INVALID) {
        kioport_free(ioport);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&ioport->base);
    return syscall_ok_u64((uint64_t)out_h);
}


/*
 * sys_handle_insert(proc_h, obj_h, rights, _) → new_handle_id or iris_error_t
 *
 * Copies obj_h into the target process's handle table with the specified rights
 * (a subset of obj_h's current rights). Requires RIGHT_MANAGE on proc_h and
 * RIGHT_TRANSFER on obj_h. The source handle is NOT consumed.
 * Returns the new handle_id assigned in the target process.
 */
uint64_t sys_handle_insert(uint64_t arg0, uint64_t arg1,
                                  uint64_t arg2, uint64_t arg3) {
    (void)arg3;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    /* A1 Increment 2a: dual resolver on the target process. */
    iris_error_t r = cspace_or_handle_resolve_obj(caller->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
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

    struct KObject *src_obj;
    iris_rights_t   src_rights;
    r = handle_table_get_object(&caller->process->handle_table,
                                (handle_id_t)arg1, &src_obj, &src_rights);
    if (r != IRIS_OK) { kobject_release(proc_obj); return syscall_err(r); }
    if (!rights_check(src_rights, RIGHT_TRANSFER)) {
        kobject_release(src_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t new_rights = rights_reduce(src_rights, (iris_rights_t)arg2);
    if (new_rights == RIGHT_NONE) {
        kobject_release(src_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    handle_id_t new_h = handle_table_insert(&proc->handle_table, src_obj, new_rights);
    kobject_release(src_obj);
    kobject_release(proc_obj);

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)new_h);
}


/* ── I/O port sub-delegation (A4) ───────────────────────────────────── */

/*
 * sys_ioport_restrict(ioport_h, offset, count) → new_handle or iris_error_t
 *
 * Creates a narrower KIoPort from an existing one.  Requires RIGHT_READ |
 * RIGHT_DUPLICATE on ioport_h.  offset + count must fit within the parent range.
 * The derived cap is granted READ|WRITE|DUPLICATE|TRANSFER so it can do both IN and OUT.
 */
uint64_t sys_ioport_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_IOPORT, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ | RIGHT_DUPLICATE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KIoPort *parent = (struct KIoPort *)obj;
    uint16_t offset = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t count  = (uint16_t)(arg2 & 0xFFFFu);

    if (count == 0 || (uint32_t)offset + count > (uint32_t)parent->count) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint16_t new_base = (uint16_t)(parent->base_port + offset);
    kobject_release(obj);

    struct KIoPort *sub = kioport_alloc(new_base, count);
    if (!sub) return syscall_err(IRIS_ERR_NO_MEMORY);

    handle_id_t h = handle_table_insert(&t->process->handle_table, &sub->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        kioport_free(sub);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&sub->base);
    return syscall_ok_u64((uint64_t)h);
}


/* ── B3: bootstrap cap permission restriction ─────────────────────── */

uint64_t sys_bootcap_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t rights;
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    struct KBootstrapCap *restricted =
        kbootcap_clone_restricted((struct KBootstrapCap *)obj, (uint32_t)arg1);
    if (!restricted) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    r = handle_table_replace(&t->process->handle_table, (handle_id_t)arg0, &restricted->base);
    kobject_release(&restricted->base);
    kobject_release(obj);
    if (r != IRIS_OK) return syscall_err(r);
    return syscall_ok_u64(0);
}


uint64_t sys_handle_type(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    struct KObject *obj;
    iris_rights_t rights;
    iris_error_t r;
    uint64_t type;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    (void)rights;
    type = (uint64_t)obj->type;
    kobject_release(obj);
    return syscall_ok_u64(type);
}


uint64_t sys_handle_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    struct KObject *obj_a;
    struct KObject *obj_b;
    iris_rights_t rights_a;
    iris_rights_t rights_b;
    iris_error_t r;
    uint64_t same;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0, &obj_a, &rights_a);
    if (r != IRIS_OK) return syscall_err(r);
    r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg1, &obj_b, &rights_b);
    if (r != IRIS_OK) {
        kobject_release(obj_a);
        return syscall_err(r);
    }
    (void)rights_a;
    (void)rights_b;
    same = (obj_a == obj_b) ? 1u : 0u;
    kobject_release(obj_b);
    kobject_release(obj_a);
    return syscall_ok_u64(same);
}
