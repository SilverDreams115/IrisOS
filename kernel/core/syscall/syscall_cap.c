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

/* sys_handle_transfer RETIRED — A1.8.  Zero in-tree callers survived the A1
 * arc: cross-process placement is SYS_PROC_CSPACE_MINT (CSpace-canonical,
 * badge-capable, fail-fast on occupied slots) or an IPC receive-slot.  The
 * dispatcher falls to default (NOT_SUPPORTED); syscall number 23 is
 * permanently reserved.  See docs/architecture/handle-table-freeze.md. */


/* ── Hardware capability creation (C2: policy moved to svcmgr) ──────── */


/* ── Fase S4 (Etapa 3 prep): CSpace-native device capabilities ────────────
 *
 * SYS_CAP_CREATE_IRQCAP / _IOPORT used to be handle producers: the only way
 * to hold a KIrqCap/KIoPort was a handle, which left the legacy handle tree
 * (SYS_CAP_DERIVE/SYS_CAP_REVOKE) as the ONLY derive+cascade-revoke mechanism
 * for device authority — the blocker that kept Stage 3 shut.
 *
 * They now publish into a CSpace slot, and — this is the point — as an MDB
 * CHILD of the bootstrap-cap SLOT that authorised the creation.  Device
 * authority therefore has a real CSpace ancestor: SYS_CSPACE_REVOKE on the
 * bootstrap cap recursively destroys every device cap issued under it, which
 * is seL4's IRQControl semantics.  The authority argument must consequently
 * be a CPtr — a handle cannot be an MDB parent (charter §3.6/A9).
 *
 * dev_cap_publish installs `obj` into the caller's root CNode at dest_slot
 * parented to (auth_cn, auth_idx).  It consumes the caller's reference to
 * obj on every path.
 */
static iris_error_t dev_cap_publish(struct task *t, struct KObject *obj,
                                    iris_rights_t rights, uint32_t dest_slot,
                                    struct KCNode *auth_cn, uint32_t auth_idx) {
    struct KCNode *root = 0;
    iris_error_t err = cspace_own_root(t->process, &root);
    if (err != IRIS_OK) return err;

    err = kcnode_slot_install_linked(root, dest_slot, obj, rights, 0,
                                     auth_cn, auth_idx,
                                     /*exclusive*/1, /*legacy*/0);
    kobject_active_release(&root->base);
    kobject_release(&root->base);
    return err;
}

/* Resolve the bootstrap-cap CPtr to BOTH its object (to check the permission
 * bit) and its slot (to become the MDB parent).  CSpace only. */
static iris_error_t dev_cap_auth(struct task *t, uint64_t auth_cptr,
                                 struct KCNode **out_cn, uint32_t *out_idx) {
    if (!cspace_only_cptr(auth_cptr)) return IRIS_ERR_INVALID_ARG;

    struct KCNode *cn; uint32_t idx;
    iris_error_t err = cspace_resolve_slot(t->process, (iris_cptr_t)auth_cptr,
                                           &cn, &idx);
    if (err != IRIS_OK) return err;

    struct KObject *auth; iris_rights_t ar;
    err = kcnode_fetch(cn, idx, &auth, &ar);
    if (err != IRIS_OK) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return err;
    }
    int ok = (auth->type == KOBJ_BOOTSTRAP_CAP) &&
             kbootcap_allows((struct KBootstrapCap *)auth, IRIS_BOOTCAP_HW_ACCESS);
    kobject_active_release(auth);
    kobject_release(auth);
    if (!ok) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out_cn = cn; *out_idx = idx;   /* active+lifecycle held by caller */
    return IRIS_OK;
}

static void dev_cap_auth_release(struct KCNode *cn) {
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
}

uint64_t sys_cap_create_irqcap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    uint8_t      irq_num   = (uint8_t)(arg1 & 0xFFu);
    uint32_t     dest_slot = (uint32_t)arg3;
    struct task *t         = task_current();
    (void)arg2;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (irq_num > 15u)     return syscall_err(IRIS_ERR_INVALID_ARG);
    if (dest_slot == 0u || dest_slot >= 1024u)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *auth_cn; uint32_t auth_idx;
    iris_error_t err = dev_cap_auth(t, arg0, &auth_cn, &auth_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KIrqCap *irqcap = kirqcap_alloc(irq_num);
    if (!irqcap) {
        dev_cap_auth_release(auth_cn);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    err = dev_cap_publish(t, &irqcap->base,
                          RIGHT_ROUTE | RIGHT_DUPLICATE | RIGHT_TRANSFER,
                          dest_slot, auth_cn, auth_idx);
    dev_cap_auth_release(auth_cn);
    if (err != IRIS_OK) {
        kirqcap_free(irqcap);
        return syscall_err(err);
    }
    kobject_release(&irqcap->base);   /* the slot holds its own refs */
    return syscall_ok_u64(0);
}


uint64_t sys_cap_create_ioport(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    uint16_t     base      = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t     count     = (uint16_t)(arg2 & 0xFFFFu);
    uint32_t     dest_slot = (uint32_t)arg3;
    struct task *t         = task_current();

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (count == 0u || (uint32_t)base + count > 0x10000u)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (dest_slot == 0u || dest_slot >= 1024u)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!kioport_in_whitelist(base, count))
        return syscall_err(IRIS_ERR_ACCESS_DENIED);

    struct KCNode *auth_cn; uint32_t auth_idx;
    iris_error_t err = dev_cap_auth(t, arg0, &auth_cn, &auth_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KIoPort *ioport = kioport_alloc(base, count);
    if (!ioport) {
        dev_cap_auth_release(auth_cn);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    err = dev_cap_publish(t, &ioport->base,
                          RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER,
                          dest_slot, auth_cn, auth_idx);
    dev_cap_auth_release(auth_cn);
    if (err != IRIS_OK) {
        kioport_free(ioport);
        return syscall_err(err);
    }
    kobject_release(&ioport->base);
    return syscall_ok_u64(0);
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
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    struct KObject *obj;
    iris_rights_t rights;
    iris_error_t r = cspace_or_handle_resolve_obj(proc, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* A CPtr source publishes the restricted clone into a destination SLOT as
     * an MDB child of the source slot — the same shape retype2 and mint use.
     *
     * The source is deliberately left intact.  Narrowing is derive-then-delete
     * here, not mutation in place: a capability is not edited, a weaker one is
     * derived from it and the strong one dropped.  A caller that wants the
     * in-place effect deletes the source slot afterwards, and because the
     * clone is a real MDB child, revoking the source still reaches it.
     *
     * The clone is a NEW object, so it cannot be produced with slot_derive
     * (which copies the source object); install_linked with an explicit parent
     * is the primitive that expresses "new object, real ancestor".  The
     * derived cap inherits the source slot's rights; callers reduce at the
     * point of delegation. */
    if (cspace_value_is_cptr((iris_cptr_t)arg0)) {
        uint32_t dest_slot = (uint32_t)arg2;
        if (dest_slot == 0u) {          /* CPTR_NULL is never a destination */
            kobject_release(obj);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }

        struct KCNode *src_cn = 0;
        uint32_t       src_idx = 0;
        r = cspace_resolve_slot(proc, (iris_cptr_t)arg0, &src_cn, &src_idx);
        if (r != IRIS_OK) {
            kobject_release(obj);
            return syscall_err(r);
        }

        struct KCNode *root = 0;
        r = cspace_own_root(proc, &root);
        if (r != IRIS_OK) {
            kobject_active_release(&src_cn->base);
            kobject_release(&src_cn->base);
            kobject_release(obj);
            return syscall_err(r);
        }

        struct KBootstrapCap *restricted =
            kbootcap_clone_restricted((struct KBootstrapCap *)obj, (uint32_t)arg1);
        if (!restricted) {
            r = IRIS_ERR_NO_MEMORY;
        } else {
            r = kcnode_slot_install_linked(root, dest_slot, &restricted->base,
                                           rights, 0,
                                           src_cn, src_idx,
                                           /*exclusive=*/1, /*legacy=*/0);
            kobject_release(&restricted->base);
        }

        kobject_active_release(&root->base);
        kobject_release(&root->base);
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        kobject_release(obj);
        if (r != IRIS_OK) return syscall_err(r);
        return syscall_ok_u64(0);
    }

    /* Handle source: rebind the caller's handle in place (legacy contract). */
    struct KBootstrapCap *restricted =
        kbootcap_clone_restricted((struct KBootstrapCap *)obj, (uint32_t)arg1);
    if (!restricted) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    r = handle_table_replace(&proc->handle_table, (handle_id_t)arg0, &restricted->base);
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
