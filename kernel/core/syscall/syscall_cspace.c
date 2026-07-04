#include "syscall_priv.h"
#include <iris/nc/cspace.h>

uint64_t sys_cap_derive(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t   src_h     = (handle_id_t)arg0;
    iris_rights_t new_rights = (iris_rights_t)arg1;
    (void)arg2;

    if (!src_h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    HandleTable *ht = &t->process->handle_table;

    struct KObject *obj;
    iris_rights_t   cur_rights;
    iris_error_t err = handle_table_get_object(ht, src_h, &obj, &cur_rights);
    if (err != IRIS_OK) return syscall_err(err);

    if (!rights_check(cur_rights, RIGHT_DUPLICATE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t effective = rights_reduce(cur_rights, new_rights);
    if (effective == RIGHT_NONE) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* handle_table_insert_derived bumps refcount via handle_entry_init. */
    handle_id_t new_h = handle_table_insert_derived(ht, obj, effective, src_h);
    kobject_release(obj);   /* drop the get_object-retained ref */

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)new_h;
}

uint64_t sys_cap_revoke(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t h = (handle_id_t)arg0;
    (void)arg1; (void)arg2;

    if (!h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    HandleTable *ht = &t->process->handle_table;

    /* Validate h before revoking its children. */
    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t err = handle_table_get_object(ht, h, &obj, &rights);
    if (err != IRIS_OK) return syscall_err(err);
    kobject_release(obj);

    handle_table_revoke_children(ht, h);
    return 0;
}

uint64_t sys_cnode_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint32_t num_slots = (uint32_t)arg0;
    (void)arg1; (void)arg2;

    if (num_slots == 0u || num_slots > KCNODE_MAX_SLOTS ||
        (num_slots & (num_slots - 1u)) != 0u)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *cn = kcnode_alloc(num_slots);
    if (!cn) return syscall_err(IRIS_ERR_NO_MEMORY);

    handle_id_t h = handle_table_insert(
        &t->process->handle_table, &cn->base,
        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    kobject_release(&cn->base);

    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

uint64_t sys_cspace_resolve(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t cptr = (iris_cptr_t)arg0;
    (void)arg1; (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    struct KObject *obj;
    iris_rights_t   rights;
    uint64_t        badge = 0;
    iris_error_t err = cspace_resolve_cap_badged(proc, cptr, RIGHT_NONE,
                                                 &obj, &rights, &badge);
    if (err != IRIS_OK) return syscall_err(err);

    /* Fase 9: materialization preserves the slot badge. */
    handle_id_t h = handle_table_insert_badged(&proc->handle_table, obj,
                                               rights, badge);
    kobject_active_release(obj);
    kobject_release(obj);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

uint64_t sys_cnode_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    iris_cptr_t   cptr_or_h = (iris_cptr_t)arg0;
    uint32_t      slot_idx  = (uint32_t)arg1;
    handle_id_t   src_h     = (handle_id_t)arg2;
    iris_rights_t new_rights = (iris_rights_t)arg3;

    if (!cptr_or_h || !src_h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;
    HandleTable     *ht   = &proc->handle_table;

    struct KCNode  *cn;
    iris_rights_t   cn_rights;
    iris_error_t err = cspace_or_handle_resolve_cnode(proc, cptr_or_h,
                                                       RIGHT_WRITE, &cn, &cn_rights);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);

    struct KObject *src_obj;
    iris_rights_t   src_rights;
    err = handle_table_get_object(ht, src_h, &src_obj, &src_rights);
    if (err != IRIS_OK) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return syscall_err(err);
    }

    if (!rights_check(src_rights, RIGHT_DUPLICATE)) {
        kobject_release(src_obj);
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t effective = rights_reduce(src_rights, new_rights);
    if (effective == RIGHT_NONE) {
        kobject_release(src_obj);
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* kcnode_mint takes its own refs (active_retain+retain) on src_obj;
     * we always drop our get_object lifecycle ref regardless of result. */
    err = kcnode_mint(cn, slot_idx, src_obj, effective);
    kobject_release(src_obj);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}

/*
 * SYS_PROC_CSPACE_MINT — Fase 8: mint a caller capability into a CHILD
 * process's root CNode so the child can invoke it CPtr-first (no KChannel
 * handle transfer). Mirrors sys_cnode_mint's reduction semantics; the only
 * new authority is RIGHT_WRITE on the child process capability.
 *
 * Fase 9 — badge packing: arg3 low 32 bits = rights mask, high 32 bits =
 * badge.  Badge semantics:
 *   badge == 0          → INHERIT the source cap's badge (preservation).
 *   badge != 0, src unbadged
 *                       → assign the new badge.  Only ENDPOINT and
 *                         NOTIFICATION caps may carry a fresh badge
 *                         (INVALID_ARG otherwise).
 *   badge != 0, src already badged with a DIFFERENT value
 *                       → ACCESS_DENIED: a badged cap can NEVER be
 *                         re-badged — holders cannot forge identities.
 */
uint64_t sys_proc_cspace_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3) {
    handle_id_t   proc_h     = (handle_id_t)arg0;
    uint32_t      slot_idx   = (uint32_t)arg1;
    handle_id_t   src_h      = (handle_id_t)arg2;
    iris_rights_t new_rights = (iris_rights_t)(arg3 & 0xFFFFFFFFu);
    uint64_t      new_badge  = arg3 >> 32;

    if (!proc_h || !src_h || slot_idx == 0u)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    HandleTable *ht = &t->process->handle_table;

    /* Child process capability — spawner authority required.
     * A1 Increment 2a: dual resolver — the target process may be a CPtr slot
     * or a handle.  The RIGHT_WRITE requirement below is unchanged. */
    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    iris_error_t err = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)proc_h,
                                 RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(proc_rights, RIGHT_WRITE)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    struct KProcess *child = (struct KProcess *)proc_obj;

    /* Child's root CNode (created at kprocess_create; soft-fail = absent). */
    if (child->cspace_root_h == HANDLE_INVALID) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    struct KObject *cn_obj;
    iris_rights_t   cn_rights;
    err = handle_table_get_object(&child->handle_table, child->cspace_root_h,
                                  &cn_obj, &cn_rights);
    if (err != IRIS_OK) {
        kobject_release(proc_obj);
        return syscall_err(err);
    }
    if (cn_obj->type != KOBJ_CNODE) {
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INTERNAL);
    }

    /* Source capability from the CALLER's table. */
    struct KObject *src_obj;
    iris_rights_t   src_rights;
    err = handle_table_get_object(ht, src_h, &src_obj, &src_rights);
    if (err != IRIS_OK) {
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(err);
    }
    if (!rights_check(src_rights, RIGHT_DUPLICATE)) {
        kobject_release(src_obj);
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t effective = rights_reduce(src_rights, new_rights);
    if (effective == RIGHT_NONE) {
        kobject_release(src_obj);
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Fase 9: badge derivation rules (no escalation, no forging). */
    uint64_t src_badge = handle_table_get_badge(ht, src_h);
    uint64_t effective_badge;
    if (new_badge == 0u) {
        effective_badge = src_badge;          /* inherit (0 stays 0) */
    } else if (src_badge != 0u && src_badge != new_badge) {
        /* A badged cap can never be re-badged. */
        kobject_release(src_obj);
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    } else {
        /* Fresh badge: only identity-bearing IPC objects accept one. */
        if (src_obj->type != KOBJ_ENDPOINT &&
            src_obj->type != KOBJ_NOTIFICATION) {
            kobject_release(src_obj);
            kobject_release(cn_obj);
            kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
        effective_badge = new_badge;
    }

    err = kcnode_mint_excl_badged((struct KCNode *)cn_obj, slot_idx, src_obj,
                                  effective, effective_badge);
    kobject_release(src_obj);
    kobject_release(cn_obj);
    kobject_release(proc_obj);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}
