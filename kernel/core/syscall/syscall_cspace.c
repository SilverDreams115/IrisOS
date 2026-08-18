#include "syscall_priv.h"
#include <iris/nc/cspace.h>

/* A1.7: successful SYS_CSPACE_RESOLVE materializations (diagnostic). */
uint32_t iris_cspace_stat_resolves = 0u;

/*
 * Fase S4 (Etapa 3): SYS_CAP_DERIVE (78) and SYS_CAP_REVOKE (79) are RETIRED.
 *
 * They were the last consumers of the handle table's parallel derivation tree
 * (`derivation_parent[]`), which duplicated — badly — what the native CSpace
 * CDT does properly: the handle tree was per-process, invisible to CSpace,
 * and could not express a cross-process ancestry.  Every productive path now
 * uses SYS_CSPACE_MINT (derive slot→slot, installing a real MDB child) and
 * SYS_CSPACE_REVOKE (recursive, cross-CNode and cross-process).
 *
 * The syscall numbers stay permanently reserved and answer NOT_SUPPORTED;
 * `handle_table_insert_derived`, `handle_table_revoke_children` and the
 * `derivation_parent[]` array are deleted.  There is now exactly ONE
 * derivation tree in the system (charter A9/A10).
 */
uint64_t sys_cap_derive(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

uint64_t sys_cap_revoke(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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

/*
 * SYS_CSPACE_RESOLVE (95) — RETIRED (Stage 4).  Number permanently reserved;
 * returns NOT_SUPPORTED.
 *
 * It was the sanctioned CSpace→handle bridge: materialise the capability in a
 * slot as a handle so it could be used by a path that only spoke handles.
 * There are no such paths.  Every question it was used to answer is answered
 * natively — SYS_CAP_IDENTIFY for a type, SYS_CAP_SAME_OBJECT for identity,
 * SYS_CSPACE_MINT for a second reference — and each of those is strictly
 * weaker than handing out authority.
 *
 * `iris_cspace_stat_resolves` stays in the SYS_SCHED_INFO layout as the
 * retirement witness: a structural zero, like the IPC handle-delivery and
 * TOCTOU counters beside it.
 */
uint64_t sys_cspace_resolve(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

/*
 * SYS_CNODE_MINT (81) — RETIRED (Stage 4).  Number permanently reserved;
 * returns NOT_SUPPORTED.  Its SOURCE was a handle, so the capability it
 * installed had no MDB relationship to anything: an independent LEGACY_ROOT
 * that a revoke of the "original" could not reach.  SYS_CSPACE_MINT is the
 * slot-to-slot form and records the derivation edge.
 */
uint64_t sys_cnode_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
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

    /* Child's root CNode (created at kprocess_create; soft-fail = absent).
     * Stage 4: read structurally — the kernel no longer reaches into ANOTHER
     * process's handle table to find that process's CSpace root. */
    if (!child->cspace_root) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    struct KObject *cn_obj = &child->cspace_root->base;
    kobject_retain(cn_obj);

    /* Source capability from the CALLER.  Etapa 4: the destination process
     * always resolved either way while the SOURCE was handle-only — the same
     * half-migration as SYS_PROCESS_WATCH and friends, and it means a caller
     * that keeps its capabilities in CSpace cannot mint from them at all.
     * The CPtr leg resolves through CSpace and drops the traversal's active
     * ref, matching the lifecycle-only reference the handle read yields. */
    struct KObject *src_obj;
    iris_rights_t   src_rights;
    if (cspace_value_is_cptr((iris_cptr_t)src_h)) {
        err = cspace_resolve_cap(t->process, (iris_cptr_t)src_h, RIGHT_NONE,
                                 &src_obj, &src_rights);
        if (err == IRIS_OK) kobject_active_release(src_obj);
    } else {
        err = handle_table_get_object(ht, src_h, &src_obj, &src_rights);
    }
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

/* CSpace-only source guard: cspace_only_cptr lives in syscall_priv.h — the
 * IPC transfer path (Fase S4/Etapa 2) enforces the same rule. */

/* Resolve the caller's root CNode with active+lifecycle refs (retype2's
 * dest_cnode == 0 convention).  Shared with syscall_cap.c (device-cap
 * publication) via syscall_priv.h.
 *
 * Stage 4: the root is a structural back-reference on KProcess, so this no
 * longer reads the handle table at all — the ledger entry "root CNode
 * reachable only via cspace_root_h" is retired, and with it the last reason
 * CSpace resolution depended on the namespace it was built to replace. */
iris_error_t cspace_own_root(struct KProcess *proc, struct KCNode **out) {
    if (!proc || !proc->cspace_root) return IRIS_ERR_NOT_FOUND;
    struct KObject *root_obj = &proc->cspace_root->base;
    kobject_retain(root_obj);
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

    /* Stage 4: structural read of the child's CSpace root — no cross-process
     * handle-table access. */
    if (!child->cspace_root) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_NOT_FOUND);
    }
    struct KObject *cn_obj = &child->cspace_root->base;
    kobject_retain(cn_obj);

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

/*
 * ── Fase S4 (Etapa 6): CSpace-native capability introspection ─────────────
 *
 * SYS_CAP_IDENTIFY (117) and SYS_CAP_SAME_OBJECT (118) are the CSpace-native
 * replacements for SYS_HANDLE_TYPE (52) and SYS_HANDLE_SAME_OBJECT (53).
 *
 * Why they exist at all.  Both questions — "what type is the capability in
 * this slot" and "do these two slots name the same object" — are authority
 * properties that survive the handle namespace: a supervisor must narrow its
 * protocol on the type of a cap that was just delivered to it, and the
 * adversarial suite must prove that a transferred capability is the SAME
 * kernel object the sender held, not a copy of its rights.  Until now the
 * only way to ask either was to materialize the slot into a handle
 * (SYS_CSPACE_RESOLVE) and interrogate the handle, which is precisely the
 * bridge Stage 4 retires.  Every remaining productive use of that bridge —
 * svcmgr's delivered-cap dispatch — is one of these two questions.
 *
 * Why they are not new authority.  Both are strictly WEAKER than the bridge
 * they replace: SYS_CSPACE_RESOLVE produced a handle, which IS authority and
 * consumed a handle-table entry; these produce a scalar and retain nothing
 * past the call.  Neither confers a right, and neither reaches outside the
 * caller's own CSpace — a CPtr is resolved against the invoker's root, so a
 * process can only ask about capabilities it already holds.
 *
 * They do let a process observe which of ITS OWN slots are occupied, and that
 * is deliberate and not a leak: a caller can already learn the same thing by
 * invoking any slot and reading NOT_FOUND, in IRIS and in seL4 alike (seL4
 * answers seL4_InvalidCapability).  A CSpace's layout is chosen by whoever
 * built that CSpace; it is not a secret kept from its owner.  What must never
 * be observable — another process's CSpace, or authority the caller does not
 * hold — is not reachable here.
 *
 * They are the invocation-time equivalent of seL4's seL4_DebugCapIdentify,
 * minus the debug-build restriction, and they consume no handle: charter
 * §3.1/§3.2/§3.6 are all satisfied — CPtr only, no dual resolution, no
 * fallback (charter §3.7).
 */
uint64_t sys_cap_identify(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0)) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t err = cspace_resolve_cap(t->process, (iris_cptr_t)arg0,
                                          RIGHT_NONE, &obj, &rights);
    if (err != IRIS_OK) return syscall_err(err);
    (void)rights;

    uint64_t type = (uint64_t)obj->type;
    kobject_active_release(obj);
    kobject_release(obj);
    return syscall_ok_u64(type);
}

uint64_t sys_cap_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0) || !cspace_only_cptr(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj_a;
    struct KObject *obj_b;
    iris_rights_t   rights_a;
    iris_rights_t   rights_b;

    iris_error_t err = cspace_resolve_cap(t->process, (iris_cptr_t)arg0,
                                          RIGHT_NONE, &obj_a, &rights_a);
    if (err != IRIS_OK) return syscall_err(err);
    err = cspace_resolve_cap(t->process, (iris_cptr_t)arg1,
                             RIGHT_NONE, &obj_b, &rights_b);
    if (err != IRIS_OK) {
        kobject_active_release(obj_a);
        kobject_release(obj_a);
        return syscall_err(err);
    }
    (void)rights_a; (void)rights_b;

    /* Identity only — rights and badge are deliberately NOT compared: two
     * slots holding differently-minted caps to one endpoint are the same
     * object, and that is exactly the property the transfer tests assert. */
    uint64_t same = (obj_a == obj_b) ? 1u : 0u;
    kobject_active_release(obj_b);
    kobject_release(obj_b);
    kobject_active_release(obj_a);
    kobject_release(obj_a);
    return syscall_ok_u64(same);
}
