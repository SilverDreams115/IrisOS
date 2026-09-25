/* SPDX-License-Identifier: Apache-2.0 */
#include "syscall_priv.h"
#include <iris/nc/cspace.h>

/* A1.7: successful SYS_CSPACE_RESOLVE materializations (diagnostic). */
uint32_t iris_cspace_stat_resolves = 0u;

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
 * SYS_CSPACE_MOVE(src_cptr, dest) — ledger A-28.
 *
 * seL4's `seL4_CNode_Move`.  IRIS could move a capability within one CNode
 * (swap against an empty slot) and not between them; across CNodes the only
 * route was mint-then-delete, which is not the same operation.  A copy is a
 * CHILD of its source, so mint-then-delete records — for as long as the two
 * calls take — a delegation that never happened, and a revoke arriving in that
 * window reaches something the mover meant to keep.
 *
 * `kcnode_slot_move` relocates the MDB node itself: parent, siblings and
 * children travel with it, so the tree after is the tree before with one slot
 * renamed.  It has been in the tree since Phase S3 with host coverage and no
 * way for ring 3 to reach it.
 */
uint64_t sys_cspace_move(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    iris_cptr_t dest_cnode = (iris_cptr_t)(arg1 & 0xFFFFFFFFu);
    uint32_t    dest_slot  = (uint32_t)(arg1 >> 32);

    /* The SOURCE slot, not the source object: a move is about where a
     * capability lives, and the object never learns it happened. */
    struct KCNode *src_cn = 0; uint32_t src_idx = 0;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                                           &src_cn, &src_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KCNode *dst_cn = 0;
    if (dest_cnode == 0u) err = cspace_own_root(t->cspace_root, &dst_cn);
    else                  err = cspace_resolve_cnode_for_publish(t->cspace_root,
                                        dest_cnode, &dst_cn);
    if (err != IRIS_OK) {
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return syscall_err(err);
    }

    err = kcnode_slot_move(src_cn, src_idx, dst_cn, dest_slot);

    kobject_active_release(&dst_cn->base);
    kobject_release(&dst_cn->base);
    kobject_active_release(&src_cn->base);
    kobject_release(&src_cn->base);
    return (err == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(err);
}

/*
 * CSpace_Rotate — seL4's `seL4_CNode_Rotate`.
 *   cptr = the SOURCE slot (whose capability ends up in the pivot)
 *   a1   = the PIVOT slot CPtr (whose capability ends up in dest)
 *   a2   = dest CNode CPtr (low 32; 0 = caller's root) | dest slot (high 32)
 *
 * Invoked on the source, packed like `CSpace_Move`, because a rotate IS two
 * moves and a caller that knows one should not have to learn a second
 * encoding for the other.
 *
 * What it buys over calling move twice: no spare slot and no window.  Moving
 * onto an occupied slot needs that slot emptied first, so two-call rearranging
 * needs a FOURTH slot to park the displaced capability in — and a CSpace full
 * enough to need rearranging is exactly the one without a spare.  Between two
 * calls the capability is also somewhere neither the holder nor a revoke
 * expects, and a failure halfway leaves a CSpace nobody asked for.
 *
 * `dest == src` is the swap, and is the case that cannot be expressed as
 * relocations at all because both slots are occupied.
 */
uint64_t sys_cspace_rotate(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    iris_cptr_t dest_cnode = (iris_cptr_t)(arg2 & 0xFFFFFFFFu);
    uint32_t    dest_slot  = (uint32_t)(arg2 >> 32);

    struct KCNode *src_cn = 0;   uint32_t src_idx = 0;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                                           &src_cn, &src_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KCNode *pivot_cn = 0; uint32_t pivot_idx = 0;
    err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg1,
                              &pivot_cn, &pivot_idx);
    if (err != IRIS_OK) {
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return syscall_err(err);
    }

    struct KCNode *dst_cn = 0;
    if (dest_cnode == 0u) err = cspace_own_root(t->cspace_root, &dst_cn);
    else                  err = cspace_resolve_cnode_for_publish(t->cspace_root,
                                        dest_cnode, &dst_cn);
    if (err != IRIS_OK) {
        kobject_active_release(&pivot_cn->base);
        kobject_release(&pivot_cn->base);
        kobject_active_release(&src_cn->base);
        kobject_release(&src_cn->base);
        return syscall_err(err);
    }

    err = kcnode_slot_rotate(dst_cn, dest_slot, pivot_cn, pivot_idx,
                             src_cn, src_idx);

    kobject_active_release(&dst_cn->base);
    kobject_release(&dst_cn->base);
    kobject_active_release(&pivot_cn->base);
    kobject_release(&pivot_cn->base);
    kobject_active_release(&src_cn->base);
    kobject_release(&src_cn->base);
    return (err == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(err);
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

    /*
     * Stage 9-evt / ledger D-8 — PREEMPTIBLE.
     *
     * Revoke used to run until the invoked capability's subtree was exhausted,
     * and nothing bounded the subtree: a ring-3 principal that could build a
     * wide derivation tree could hold the CPU for as long as that tree was
     * large.  It was the one in-kernel operation with no latency bound, and
     * the ledger recorded it as blocked on the event kernel because a
     * preemptible delete needs somewhere to park a continuation.
     *
     * Step 1 of that conversion built exactly that, and this is the first
     * thing to use it.  The slice is bounded; if descendants remain the
     * syscall asks to be re-executed, and the dispatcher reschedules in
     * between — which is the preemption point.  The continuation needs no
     * cursor: every slice destroys what it revoked, so the tree is strictly
     * smaller on re-entry and the same arguments mean less work each time.
     * The only thing carried across is the running total, because the caller
     * asked once and expects one answer.
     */
    if (!t->sc_reentry) t->sc_acc = 0;

    uint32_t revoked = 0;
    int more = 0;
    err = kcnode_slot_revoke_bounded(cn, idx, IRIS_REVOKE_SLICE, &revoked, &more);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (err != IRIS_OK) return syscall_err(err);

    t->sc_acc += revoked;
    if (more) {
        syscall_request_restart(t);
        return 0;
    }
    return t->sc_acc;
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
 * sys_cspace_self — RETIRED (ledger D-6 / charter A5).
 *
 * It handed the caller a capability to its own root CSpace, asking for NO
 * capability at all: ambient authority, which seL4 does not have.  It also
 * published an MDB LEGACY ROOT — a capability with no ancestor, which no
 * revoke can reach.
 *
 * A thread is given its CSpace by whoever configured it.  Every service
 * receives one at IRIS_CPTR_OWN_CSPACE before its first instruction, and the ROOT TASK
 * finds its own in BootInfo — which is exactly seL4's arrangement:
 * seL4_CapInitThreadVSpace and seL4_CapInitThreadCNode are BootInfo slots,
 * not syscalls.  The number stays permanently reserved.
 */

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

/*
 * SYS_CSPACE_SET_GUARD (127) — install a guard on a CNode capability.
 *
 * Stage 8-cap, ledger D-2.  See the contract in <iris/syscall.h>.
 *
 * The authority is holding the SLOT: a guard changes how CPtrs resolve through
 * that capability, which alters the holder's own capability address space and
 * nothing outside it.  The guard is written into the slot, so a second
 * capability to the same CNode — in this CSpace or another — keeps whatever
 * guard it had.  That is what makes a guard a property of the capability
 * rather than of the object, which is seL4's model and the reason a guard can
 * make one holder's view sparse without touching anybody else's.
 */
uint64_t sys_cspace_set_guard(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!cspace_only_cptr(arg0)) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (arg2 > KCNODE_GUARD_BITS_MAX) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *cn; uint32_t idx;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                                           &cn, &idx);
    if (err != IRIS_OK) return syscall_err(err);

    err = kcnode_slot_set_guard(cn, idx, arg1, (uint8_t)arg2);

    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    return err == IRIS_OK ? syscall_ok_u64(0) : syscall_err(err);
}
