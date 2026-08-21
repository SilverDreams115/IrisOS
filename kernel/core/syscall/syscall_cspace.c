#include "syscall_priv.h"
#include <iris/nc/cspace.h>

/* A1.7: successful SYS_CSPACE_RESOLVE materializations (diagnostic). */
uint32_t iris_cspace_stat_resolves = 0u;

/*
 * Phase S4 (Step 3): SYS_CAP_DERIVE (78) and SYS_CAP_REVOKE (79) are RETIRED.
 *
 * They were the last consumers of the handle table's parallel derivation tree
 * (`derivation_parent[]`), which duplicated — badly — what the native CSpace
 * CDT does properly: the handle tree was per-process, invisible to CSpace,
 * and could not express a cross-process ancestry.  Every productive path now
 * uses SYS_CSPACE_MINT (derive slot→slot, installing a real MDB child) and
 * SYS_CSPACE_REVOKE (recursive, cross-CNode and cross-process).
 *
 * The syscall numbers stay permanently reserved and answer NOT_SUPPORTED;
 * the table's derived-insert, revoke-children entry points and the
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
 * Phase S1: SYS_CNODE_CREATE (80) is RETIRED — runtime CNodes are created ONLY
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
 * SYS_PROC_CSPACE_MINT (88) — RETIRED (Stage 7 Step 9).  Number permanently
 * reserved; answers NOT_SUPPORTED.
 *
 * It minted into a CSpace named by the PROCESS that owned it, and the kernel
 * read `child->cspace_root` out of that process.  So a spawner reached a
 * capability namespace it did not hold by naming something else that pointed
 * at it — the one shape left where a process capability granted access to an
 * object the holder had never been given.
 *
 * SYS_CSPACE_MINT has taken a destination CNode since Phase S3, including
 * dest_cnode 0 for the caller's own root.  Minting into a child is that, with
 * the child's root CNode as the destination — which a spawner HAS, because it
 * retyped it (Stage 6-pure Step 5) and handed it to SYS_PROCESS_CREATE.  A
 * spawner that means to keep delegating keeps it; one that does not holds no
 * authority over its child's namespace at all, which is a distinction the
 * process-shaped form could not express.
 */
uint64_t sys_proc_cspace_mint(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

/* ════════════════════════════════════════════════════════════════════════
 * Phase S3 — CSpace-only derivation syscalls (native MDB/CDT).
 *
 * These take their SOURCE exclusively from the caller's CSpace (CPtr < 1024,
 * resolved to a slot).  They never consult the handle table for the source —
 * a handle value is INVALID_ARG (charter §3.6: no new dual-namespace
 * authority).  Semantics: docs/architecture/cspace-cdt-mdb.md §4.
 * ════════════════════════════════════════════════════════════════════════ */

/* CSpace-only source guard: cspace_only_cptr lives in syscall_priv.h — the
 * IPC transfer path (Phase S4/Step 2) enforces the same rule. */

/* Resolve the caller's root CNode with active+lifecycle refs (retype2's
 * dest_cnode == 0 convention).  Shared with syscall_cap.c (device-cap
 * publication) via syscall_priv.h.
 *
 * Stage 4: the root is a structural back-reference, so this no longer reads
 * the handle table at all — the ledger entry "root CNode reachable only via
 * cspace_root_h" is retired, and with it the last reason CSpace resolution
 * depended on the namespace it was built to replace.
 *
 * Stage 7 Step 4: the back-reference it reads is the THREAD's.  "My own root"
 * used to mean my process's, which was the same CNode and a different claim. */
iris_error_t cspace_own_root(struct KCNode *root, struct KCNode **out) {
    if (!root) return IRIS_ERR_NOT_FOUND;
    struct KObject *root_obj = &root->base;
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
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(src_cptr)) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (dest_cnode != 0u && !cspace_only_cptr(dest_cnode))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (dest_slot == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *src_cn; uint32_t src_idx;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)src_cptr,
                                           &src_cn, &src_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KCNode *dst_cn = 0;
    if (dest_cnode == 0u) {
        err = cspace_own_root(t->cspace_root, &dst_cn);
    } else {
        iris_rights_t dr;
        err = cspace_resolve_cnode(t->cspace_root, (iris_cptr_t)dest_cnode,
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
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0)) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *cn; uint32_t idx;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
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
 * SYS_CSPACE_MINT_INTO (116) — RETIRED (Stage 7 Step 9).  Number permanently
 * reserved; answers NOT_SUPPORTED.
 *
 * It minted into a CSpace named by the PROCESS that owned it, and the kernel
 * read `child->cspace_root` out of that process.  So a spawner reached a
 * capability namespace it did not hold by naming something else that pointed
 * at it — the one shape left where a process capability granted access to an
 * object the holder had never been given.
 *
 * SYS_CSPACE_MINT has taken a destination CNode since Phase S3, including
 * dest_cnode 0 for the caller's own root.  Minting into a child is that, with
 * the child's root CNode as the destination — which a spawner HAS, because it
 * retyped it (Stage 6-pure Step 5) and handed it to SYS_PROCESS_CREATE.  A
 * spawner that means to keep delegating keeps it; one that does not holds no
 * authority over its child's namespace at all, which is a distinction the
 * process-shaped form could not express.
 */
uint64_t sys_cspace_mint_into(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

/*
 * ── Phase S4 (Step 6): CSpace-native capability introspection ─────────────
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
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0)) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t err = cspace_resolve_cap(t->cspace_root, (iris_cptr_t)arg0,
                                          RIGHT_NONE, &obj, &rights);
    if (err != IRIS_OK) return syscall_err(err);
    (void)rights;

    uint64_t type = (uint64_t)obj->type;
    kobject_active_release(obj);
    kobject_release(obj);
    return syscall_ok_u64(type);
}

/*
 * SYS_CSPACE_SELF — a capability to the caller's OWN root CNode.
 *
 * The CNode counterpart of SYS_TCB_SELF, and added for the same reason: the
 * root task received a capability to its own root CNode from the kernel
 * (Stage 5 Step 3), but every other process could only reach its CSpace
 * through the "arg0 == 0 means my own root" convention.  SYS_TCB_CONFIGURE
 * takes the CSpace as a CAPABILITY, so a process that cannot name its own
 * root CNode could only have been handed a convention where the signature
 * says capability.
 *
 * It confers nothing new: the caller already resolves every CPtr through this
 * CNode, and nothing here can name another process's CSpace.  What it adds is
 * the ability to SAY which CSpace you mean — including to delegate it, which
 * is how a supervisor will hand a child its CSpace root once processes are
 * composed rather than created (Stage 7).
 */
uint64_t sys_cspace_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *root = 0;
    iris_error_t err = cspace_own_root(t->cspace_root, &root);
    if (err != IRIS_OK) return syscall_err(err);

    /* cspace_own_root hands back an active+lifecycle pair; publish_slot takes
     * its own references and consumes the lifecycle one, so the active half is
     * released here. */
    kobject_active_release(&root->base);
    err = syscall_publish_slot(t, &root->base,
                               RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE |
                               RIGHT_TRANSFER,
                               arg0, 0, 0);
    if (err != IRIS_OK) return syscall_err(err);
    return syscall_ok_u64(0);
}

uint64_t sys_cap_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0) || !cspace_only_cptr(arg1))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj_a;
    struct KObject *obj_b;
    iris_rights_t   rights_a;
    iris_rights_t   rights_b;

    iris_error_t err = cspace_resolve_cap(t->cspace_root, (iris_cptr_t)arg0,
                                          RIGHT_NONE, &obj_a, &rights_a);
    if (err != IRIS_OK) return syscall_err(err);
    err = cspace_resolve_cap(t->cspace_root, (iris_cptr_t)arg1,
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
