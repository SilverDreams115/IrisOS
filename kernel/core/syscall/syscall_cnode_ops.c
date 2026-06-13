/*
 * syscall_cnode_ops.c — Block 6 (Ph82-84): CNode slot operations.
 *
 * SYS_CNODE_MOVE:   move a handle from the caller's HandleTable into a CNode slot
 *                   (destructive — handle is removed from the HT).
 * SYS_CNODE_FETCH:  copy a CNode slot into a new HandleTable entry (non-destructive).
 * SYS_CNODE_DELETE: clear a CNode slot, releasing its capability reference.
 * SYS_CNODE_SWAP:   swap two slots within the same CNode (no refcount change).
 *
 * Fase 3.1: arg0 (cnode target) is now resolved via cspace_or_handle_resolve_cnode,
 * which tries CSpace traversal first and falls back to the handle table.  Both paths
 * return an active+lifecycle retained KCNode *; callers must release both.
 */
#include "syscall_priv.h"
#include <iris/nc/cspace.h>

uint64_t sys_cnode_move(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t cptr_or_h = (iris_cptr_t)arg0;
    uint32_t    slot_idx  = (uint32_t)arg1;
    handle_id_t src_h     = (handle_id_t)arg2;

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

    /* kcnode_mint takes its own active+lifecycle refs; we drop our temp ref.
     * Fase 9: MOVE preserves the badge across the handle→slot crossing. */
    err = kcnode_mint_badged(cn, slot_idx, src_obj, src_rights,
                             handle_table_get_badge(ht, src_h));
    kobject_release(src_obj);
    if (err != IRIS_OK) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return syscall_err(err);
    }

    handle_table_close(ht, src_h); /* MOVE: remove from caller's HT */
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    return 0;
}

uint64_t sys_cnode_fetch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t cptr_or_h = (iris_cptr_t)arg0;
    uint32_t    slot_idx  = (uint32_t)arg1;
    (void)arg2;

    if (!cptr_or_h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;
    HandleTable     *ht   = &proc->handle_table;

    struct KCNode  *cn;
    iris_rights_t   cn_rights;
    iris_error_t err = cspace_or_handle_resolve_cnode(proc, cptr_or_h,
                                                       RIGHT_READ, &cn, &cn_rights);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);

    struct KObject *slot_obj;
    iris_rights_t   slot_rights;
    uint64_t        slot_badge = 0;
    err = kcnode_fetch_badged(cn, slot_idx, &slot_obj, &slot_rights, &slot_badge);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    if (err != IRIS_OK) return syscall_err(err);

    /* slot_obj: kcnode_fetch gave us active+lifecycle working refs.
     * handle_table_insert → handle_entry_init adds another active+lifecycle pair.
     * We drop our working refs after the insert.  Fase 9: the badge follows
     * the cap from the slot into the handle table. */
    handle_id_t h = handle_table_insert_badged(ht, slot_obj, slot_rights,
                                               slot_badge);
    kobject_active_release(slot_obj);
    kobject_release(slot_obj);

    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

uint64_t sys_cnode_delete(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t cptr_or_h = (iris_cptr_t)arg0;
    uint32_t    slot_idx  = (uint32_t)arg1;
    (void)arg2;

    if (!cptr_or_h) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    struct KCNode  *cn;
    iris_rights_t   cn_rights;
    iris_error_t err = cspace_or_handle_resolve_cnode(proc, cptr_or_h,
                                                       RIGHT_WRITE, &cn, &cn_rights);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);

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
    iris_error_t err = cspace_or_handle_resolve_cnode(proc, cptr_or_h,
                                                       RIGHT_WRITE, &cn, &cn_rights);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);

    uint64_t flags = irq_spinlock_lock(&cn->lock);

    if (slot_a >= cn->slot_count || slot_b >= cn->slot_count) {
        irq_spinlock_unlock(&cn->lock, flags);
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Swap is a pure in-place exchange of slot contents; no refcount change. */
    struct KCSlot tmp  = cn->slots[slot_a];
    cn->slots[slot_a]  = cn->slots[slot_b];
    cn->slots[slot_b]  = tmp;

    irq_spinlock_unlock(&cn->lock, flags);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    return 0;
}
