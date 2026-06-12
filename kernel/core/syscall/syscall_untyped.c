/*
 * syscall_untyped.c — Fase 3.3: KUntyped authority paths migrated to CPtr/dual resolution.
 *
 * SYS_UNTYPED_INFO:   query phys_base and available bytes.
 * SYS_UNTYPED_RETYPE: carve a typed kernel object (or sub-untyped) from a
 *   KUntyped region without touching the kernel heap.
 * SYS_UNTYPED_RESET:  reset the bump pointer to 0 when child_count == 0,
 *   making the physical region reclaimable for fresh retype operations.
 *
 * All three syscalls resolve authority via cspace_or_handle_resolve_untyped:
 *   1. CSpace-first: resolve via proc->cspace_root_h + cptr traversal.
 *   2. Handle-table fallback: if CSpace is not configured or the cptr is not
 *      found (NOT_FOUND, INVALID_ARG), fall back to the legacy handle table.
 *   3. ACCESS_DENIED from CSpace is a hard stop — no fallback.
 *
 * Ref contract: active + lifecycle (same as CNode ops).  KUntyped ops do not
 * block, so holding active_refs for the syscall duration is safe.
 */
#include "syscall_priv.h"

uint64_t sys_untyped_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t ut_cptr    = (iris_cptr_t)arg0;
    uint64_t    phys_uptr  = arg1;
    uint64_t    avail_uptr = arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KUntyped *ut;
    iris_rights_t    rights;
    iris_error_t     err = cspace_or_handle_resolve_untyped(t->process, ut_cptr,
                                                             RIGHT_READ, &ut, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    uint64_t phys  = ut->phys_base;
    uint64_t avail = kuntyped_available(ut);
    kobject_active_release(&ut->base);
    kobject_release(&ut->base);

    if (phys_uptr  && !copy_to_user_checked(phys_uptr,  &phys,  (uint32_t)sizeof(phys)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (avail_uptr && !copy_to_user_checked(avail_uptr, &avail, (uint32_t)sizeof(avail)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return 0;
}

uint64_t sys_untyped_retype(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t ut_cptr  = (iris_cptr_t)arg0;
    uint32_t    obj_type = (uint32_t)arg1;
    uint64_t    obj_arg  = arg2; /* KOBJ_UNTYPED: sub-size; KOBJ_CNODE: num_slots; else 0 */

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KUntyped *ut;
    iris_rights_t    rights;
    iris_error_t     err = cspace_or_handle_resolve_untyped(t->process, ut_cptr,
                                                             RIGHT_WRITE, &ut, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    struct KObject *new_obj  = 0;
    iris_rights_t   new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    switch (obj_type) {
        case KOBJ_ENDPOINT: {
            void *mem = kuntyped_alloc_child(ut, sizeof(struct KEndpoint));
            if (!mem) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &kendpoint_alloc_at(mem)->base;
            break;
        }
        case KOBJ_NOTIFICATION: {
            void *mem = kuntyped_alloc_child(ut, sizeof(struct KNotification));
            if (!mem) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &knotification_alloc_at(mem)->base;
            break;
        }
        case KOBJ_CNODE: {
            uint32_t num_slots = obj_arg ? (uint32_t)obj_arg : KCNODE_DEFAULT_SLOTS;
            if (num_slots == 0u || num_slots > KCNODE_MAX_SLOTS ||
                (num_slots & (num_slots - 1u)) != 0u) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_INVALID_ARG);
            }
            void *mem = kuntyped_alloc_child(ut, KCNODE_ALLOC_SIZE(num_slots));
            if (!mem) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &kcnode_alloc_at(mem, num_slots)->base;
            break;
        }
        case KOBJ_SCHED_CONTEXT: {
            void *mem = kuntyped_alloc_child(ut, sizeof(struct KSchedContext));
            if (!mem) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &kschedctx_alloc_at(mem)->base;
            break;
        }
        case KOBJ_CHANNEL: {
            void *mem = kuntyped_alloc_child(ut, sizeof(struct KChannel));
            if (!mem) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &kchannel_alloc_at(mem)->base;
            break;
        }
        case KOBJ_UNTYPED: {
            /* Sub-untyped: carve a page-aligned physical sub-region from the parent. */
            uint64_t size = obj_arg;
            if (size < 4096u || (size & 4095u)) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_INVALID_ARG);
            }
            uint64_t phys = kuntyped_bump_alloc_phys(ut, size);
            if (!phys) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            struct KUntyped *sub = kuntyped_create(phys, size, ut->is_device);
            if (!sub) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            /* Ph80: sub-untyped tracks its parent for child_count bookkeeping. */
            sub->alloc_parent = ut;
            kobject_retain(&ut->base);
            atomic_fetch_add_explicit(&ut->child_count, 1u, memory_order_relaxed);
            new_obj = &sub->base;
            break;
        }
        case KOBJ_FRAME: {
            /* Carve a PAGE_SIZE-aligned physical region from the parent KUntyped.
             * obj_arg is the frame size in bytes: must be >= 4096 and PAGE_SIZE aligned.
             * The KFrame header is slab-allocated separately; the physical region
             * is the frame content (not the header storage). */
            uint64_t fsize = obj_arg;
            if (fsize < 4096u || (fsize & 4095u)) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_INVALID_ARG);
            }
            uint64_t phys = kuntyped_bump_alloc_phys_page(ut, fsize);
            if (!phys) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            struct KFrame *frm = kframe_alloc(phys, fsize, ut);
            if (!frm) {
                /* phys is already consumed from the bump; cannot un-bump.
                 * The wasted region will be reclaimed on next SYS_UNTYPED_RESET
                 * (which still requires child_count == 0, which remains valid). */
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &frm->base;
            break;
        }
        default:
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }

    /* Release our resolve refs on the parent — the new object's alloc ref is separate. */
    kobject_active_release(&ut->base);
    kobject_release(&ut->base);

    handle_id_t h = handle_table_insert(&t->process->handle_table, new_obj, new_rights);
    kobject_release(new_obj); /* transfer alloc ref to handle table */

    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}

/*
 * SYS_UNTYPED_RESET — reset the bump pointer to 0 if no children are live.
 *
 * The caller is responsible for ensuring all objects and sub-untypeds created
 * from this untyped have been destroyed (handles closed, kobject refcounts at 0)
 * before calling RESET.  The kernel enforces this via child_count: if any child
 * is still alive, RESET returns IRIS_ERR_BUSY.
 *
 * After a successful RESET, the entire physical region can be retyped fresh.
 */
uint64_t sys_untyped_reset(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    iris_cptr_t ut_cptr = (iris_cptr_t)arg0;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KUntyped *ut;
    iris_rights_t    rights;
    iris_error_t     err = cspace_or_handle_resolve_untyped(t->process, ut_cptr,
                                                             RIGHT_WRITE, &ut, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    uint64_t flags    = irq_spinlock_lock(&ut->lock);
    uint32_t children = atomic_load_explicit(&ut->child_count, memory_order_acquire);
    if (children != 0u) {
        irq_spinlock_unlock(&ut->lock, flags);
        kobject_active_release(&ut->base);
        kobject_release(&ut->base);
        return syscall_err(IRIS_ERR_BUSY);
    }
    ut->used = 0;
    irq_spinlock_unlock(&ut->lock, flags);

    kobject_active_release(&ut->base);
    kobject_release(&ut->base);
    return 0;
}
