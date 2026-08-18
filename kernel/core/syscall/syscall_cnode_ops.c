/*
 * syscall_cnode_ops.c — Block 6 (Ph82-84): CNode slot operations.
 *
 * SYS_CNODE_DELETE: clear a CNode slot, releasing its capability reference.
 * SYS_CNODE_SWAP:   swap two slots within the same CNode (no refcount change).
 *
 * SYS_CNODE_MOVE and SYS_CNODE_FETCH are RETIRED (Stage 4): both crossed
 * between a CNode slot and the handle table, which is the direction that stage
 * deletes.  Slot-to-slot is SYS_CSPACE_MINT (+ SYS_CNODE_DELETE for a move),
 * which also records the derivation edge.
 *
 * arg0 (the CNode target) resolves through the CSpace; the resolver returns an
 * active+lifecycle retained KCNode *, and callers release both.
 */
#include "syscall_priv.h"
#include <iris/nc/cspace.h>

/*
 * SYS_CNODE_MOVE (89) — RETIRED (Stage 4).  Number permanently reserved;
 * returns NOT_SUPPORTED.  Its SOURCE was a handle, which is the direction this
 * stage deletes: SYS_CSPACE_MINT followed by SYS_CNODE_DELETE expresses the
 * same move between slots and keeps the derivation tree consistent.
 */
uint64_t sys_cnode_move(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

uint64_t sys_cnode_fetch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

uint64_t sys_cnode_delete(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t cptr_or_h = (iris_cptr_t)arg0;
    uint32_t    slot_idx  = (uint32_t)arg1;
    (void)arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    struct KCNode  *cn;
    iris_rights_t   cn_rights;
    iris_error_t    err;

    /* Fase S1: arg0 == 0 names the CALLER'S OWN root CNode (mirrors the
     * SYS_UNTYPED_RETYPE2 destination convention).  Deleting a slot of your
     * own CSpace only discards authority you already hold — no amplification. */
    if (cptr_or_h == 0u) {
        /* Stage 4: structural root read. */
        if (!proc->cspace_root)
            return syscall_err(IRIS_ERR_NOT_FOUND);
        struct KObject *root_obj = &proc->cspace_root->base;
        kobject_retain(root_obj);
        kobject_active_retain(root_obj);
        cn = (struct KCNode *)root_obj;
    } else {
        err = cspace_resolve_only_cnode(proc, cptr_or_h,
                                             RIGHT_WRITE, &cn, &cn_rights);
        if (err != IRIS_OK)
            return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
    }

    err = kcnode_delete(cn, slot_idx);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}

uint64_t sys_cnode_swap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t cptr_or_h = (iris_cptr_t)arg0;
    uint32_t    slot_a    = (uint32_t)arg1;
    uint32_t    slot_b    = (uint32_t)arg2;

    if (!cptr_or_h || slot_a == slot_b) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    struct KCNode  *cn;
    iris_rights_t   cn_rights;
    iris_error_t err = cspace_resolve_only_cnode(proc, cptr_or_h,
                                                       RIGHT_WRITE, &cn, &cn_rights);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);

    /* Fase S3: swap goes through the canonical primitive — the MDB links of
     * both capabilities (parent, siblings, children) travel with them. */
    err = kcnode_swap(cn, slot_a, slot_b);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (err != IRIS_OK) return syscall_err(err);
    return 0;
}
