#include "syscall_priv.h"
#include <iris/nc/cspace.h>

/* A1.7: successful SYS_CSPACE_RESOLVE materializations (diagnostic). */
uint32_t iris_cspace_stat_resolves = 0u;

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

    /* Fase S2: cuenta las derivaciones handle-tree de tipos canónicos
     * migrados — este contador debe converger a 0 (S2.33) cuando la
     * derivación de esos tipos pase al CDT nativo (Bloque D). */
    switch (obj->type) {
        case KOBJ_ENDPOINT: case KOBJ_NOTIFICATION: case KOBJ_REPLY:
        case KOBJ_CNODE:    case KOBJ_TCB:          case KOBJ_SCHED_CONTEXT:
            kcnode_cdt_note_legacy_migrated_derivation();
            break;
        default: break;
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

/*
 * Fase S1: SYS_CNODE_CREATE (80) is RETIRED — runtime CNodes are created ONLY
 * via SYS_UNTYPED_RETYPE2.  The single remaining kslab CNode is the per-process
 * root CNode fabricated at kprocess_alloc (bootstrap exception, ledger-tracked).
 */
uint64_t sys_cnode_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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
    __atomic_fetch_add(&iris_cspace_stat_resolves, 1u, __ATOMIC_RELAXED);
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

    /* Fase 9 rules, Fase S3 centralization: badge derivation lives in ONE
     * place (mdb_badge_derive) — inherit on 0, never re-badge, fresh badges
     * only for identity-bearing IPC objects. */
    uint64_t src_badge = handle_table_get_badge(ht, src_h);
    uint64_t effective_badge;
    err = mdb_badge_derive(src_badge, new_badge, (uint32_t)src_obj->type,
                           &effective_badge);
    if (err != IRIS_OK) {
        kobject_release(src_obj);
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(err);
    }

    err = kcnode_mint_excl_badged((struct KCNode *)cn_obj, slot_idx, src_obj,
                                  effective, effective_badge);
    kobject_release(src_obj);
    kobject_release(cn_obj);
    kobject_release(proc_obj);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * Fase S3 — CSpace-only derivation syscalls (native MDB/CDT).
 *
 * These take their SOURCE exclusively from the caller's CSpace (CPtr < 1024,
 * resolved to a slot).  They never consult the handle table for the source —
 * a handle value is INVALID_ARG (charter §3.6: no new dual-namespace
 * authority).  Semantics: docs/architecture/cspace-cdt-mdb.md §4.
 * ════════════════════════════════════════════════════════════════════════ */

/* CSpace-only source guard: nonzero direct CPtr territory. */
static inline int cspace_only_cptr(uint64_t v) {
    return v != 0u && v < 1024u;
}

/* Resolve the caller's root CNode with active+lifecycle refs (retype2's
 * dest_cnode == 0 convention). */
static iris_error_t cspace_own_root(struct KProcess *proc, struct KCNode **out) {
    if (proc->cspace_root_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;
    struct KObject *root_obj;
    iris_rights_t   root_r;
    iris_error_t err = handle_table_get_object(&proc->handle_table,
                                               proc->cspace_root_h,
                                               &root_obj, &root_r);
    if (err != IRIS_OK) return err;
    if (root_obj->type != KOBJ_CNODE) {
        kobject_release(root_obj);
        return IRIS_ERR_INTERNAL;
    }
    kobject_active_retain(root_obj);
    *out = (struct KCNode *)root_obj;
    return IRIS_OK;
}

/*
 * SYS_CSPACE_MINT (114) — copy/mint slot→slot within the caller's CSpace.
 *   arg0 = source CPtr (CSpace only)
 *   arg1 = dest CNode CPtr (low 32; 0 = caller's root; CSpace only) |
 *          dest slot (high 32)
 *   arg2 = rights (low 32; RIGHT_SAME_RIGHTS ⇒ copy) | badge (high 32)
 * The new capability is an MDB CHILD of the source slot.  Exclusive install.
 */
uint64_t sys_cspace_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t      src_cptr   = arg0;
    uint64_t      dest_cnode = arg1 & 0xFFFFFFFFu;
    uint32_t      dest_slot  = (uint32_t)(arg1 >> 32);
    iris_rights_t req_rights = (iris_rights_t)(arg2 & 0xFFFFFFFFu);
    uint64_t      req_badge  = arg2 >> 32;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    if (!cspace_only_cptr(src_cptr)) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (dest_cnode != 0u && !cspace_only_cptr(dest_cnode))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (dest_slot == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *src_cn; uint32_t src_idx;
    iris_error_t err = cspace_resolve_slot(proc, (iris_cptr_t)src_cptr,
                                           &src_cn, &src_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KCNode *dst_cn = 0;
    if (dest_cnode == 0u) {
        err = cspace_own_root(proc, &dst_cn);
    } else {
        iris_rights_t dr;
        err = cspace_resolve_cnode(proc, (iris_cptr_t)dest_cnode,
                                   RIGHT_WRITE, &dst_cn, &dr);
        if (err == IRIS_ERR_WRONG_TYPE) err = IRIS_ERR_INVALID_ARG;
    }
    if (err != IRIS_OK) {
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return syscall_err(err);
    }

    err = kcnode_slot_derive(src_cn, src_idx, dst_cn, dest_slot,
                             req_rights, req_badge);

    kobject_active_release(&dst_cn->base);
    kobject_release(&dst_cn->base);
    kobject_active_release(&src_cn->base);
    kobject_release(&src_cn->base);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}

/*
 * SYS_CSPACE_REVOKE (115) — revoke every MDB descendant of the slot named by
 * arg0 (CSpace only), across CNodes and processes.  The invoked capability
 * survives; siblings survive.  Returns the number of capabilities destroyed.
 */
uint64_t sys_cspace_revoke(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0)) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *cn; uint32_t idx;
    iris_error_t err = cspace_resolve_slot(t->process, (iris_cptr_t)arg0,
                                           &cn, &idx);
    if (err != IRIS_OK) return syscall_err(err);

    uint32_t revoked = 0;
    err = kcnode_slot_revoke(cn, idx, &revoked);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (err != IRIS_OK) return syscall_err(err);
    return (uint64_t)revoked;
}

/*
 * SYS_CSPACE_MINT_INTO (116) — cross-process CSpace-sourced mint.
 *   arg0 = target process (CPtr/handle dual — target-authority pattern;
 *          RIGHT_WRITE required, mirrors SYS_PROC_CSPACE_MINT)
 *   arg1 = destination slot in the target's root CNode (0 refused)
 *   arg2 = source CPtr in the CALLER's CSpace (CSpace only)
 *   arg3 = rights (low 32; RIGHT_SAME_RIGHTS ⇒ copy) | badge (high 32)
 * The installed capability is an MDB CHILD of the caller's source slot, so
 * the caller (or any ancestor) can later revoke the delegation — this is the
 * derivation edge that crosses processes.
 */
uint64_t sys_cspace_mint_into(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3) {
    uint32_t      dest_slot  = (uint32_t)arg1;
    uint64_t      src_cptr   = arg2;
    iris_rights_t req_rights = (iris_rights_t)(arg3 & 0xFFFFFFFFu);
    uint64_t      req_badge  = arg3 >> 32;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    if (!arg0 || dest_slot == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!cspace_only_cptr(src_cptr)) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Target process — spawner authority (same contract as PROC_CSPACE_MINT). */
    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    iris_error_t err = cspace_or_handle_resolve_obj(proc, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(proc_rights, RIGHT_WRITE)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    struct KProcess *child = (struct KProcess *)proc_obj;

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

    /* Source slot in the CALLER's CSpace. */
    struct KCNode *src_cn; uint32_t src_idx;
    err = cspace_resolve_slot(proc, (iris_cptr_t)src_cptr, &src_cn, &src_idx);
    if (err != IRIS_OK) {
        kobject_release(cn_obj);
        kobject_release(proc_obj);
        return syscall_err(err);
    }

    err = kcnode_slot_derive(src_cn, src_idx, (struct KCNode *)cn_obj,
                             dest_slot, req_rights, req_badge);

    kobject_active_release(&src_cn->base);
    kobject_release(&src_cn->base);
    kobject_release(cn_obj);
    kobject_release(proc_obj);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}
