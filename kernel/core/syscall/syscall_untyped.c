/*
 * syscall_untyped.c — KUntyped authority paths.
 *
 * Fase S1 (seL4 Architectural Convergence):
 *
 * SYS_UNTYPED_INFO:    query phys_base and available bytes.
 * SYS_UNTYPED_RETYPE:  LEGACY single-object retype returning a handle.
 *   Restricted to the NON-migrated types (KOBJ_UNTYPED sub-regions,
 *   KOBJ_FRAME, KOBJ_SCHED_CONTEXT).  The migrated object family
 *   (Endpoint / Notification / Reply / CNode) can no longer be born through
 *   a handle-publishing path (S20) — use SYS_UNTYPED_RETYPE2.
 * SYS_UNTYPED_RETYPE2: canonical batch retype.  Objects are stored INSIDE the
 *   source untyped region and their capabilities are published DIRECTLY into
 *   CSpace destination slots — no handle, no quota, no hidden allocator.
 * SYS_UNTYPED_RESET:   reset the bump pointer when child_count == 0, making
 *   the region reusable; bumps the untyped generation (stale-reuse defense
 *   witness) and the reclaim/reuse counters.
 * SYS_UNTYPED_QUERY:   versioned, read-only instrumentation (global counters,
 *   per-untyped state, per-type object gauges).  Diagnostics only — never a
 *   source of authority.
 *
 * Authority: all syscalls resolve the untyped via
 * cspace_or_handle_resolve_untyped (CPtr < 1024 → CSpace only; >= 1024 →
 * handle table only; ACCESS_DENIED is a hard stop, no fallback).
 *
 * Atomicity note (U14/U15): IRIS is uniprocessor with IRQ-off spinlocks and a
 * non-preemptive kernel (no yield inside retype).  RETYPE2 validates
 * everything before mutating: capacity+carve are one critical section
 * (kuntyped_alloc_children_atomic) and slot publication re-checks occupancy;
 * a publication conflict rolls back every object and un-bumps the carve
 * exactly, so a failed batch consumes nothing.
 */
#include "syscall_priv.h"
#include <iris/nc/kreply.h>

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

/* Canonical per-type payload sizes for the migrated family (S1 object-size
 * contract).  The retyped block is KUNTYPED_ALIGN (parent header) +
 * align_up(payload, KUNTYPED_ALIGN); alignment is KUNTYPED_ALIGN (64B),
 * which every struct below satisfies. */
_Static_assert(_Alignof(struct KEndpoint)     <= KUNTYPED_ALIGN, "KEndpoint alignment");
_Static_assert(_Alignof(struct KNotification) <= KUNTYPED_ALIGN, "KNotification alignment");
_Static_assert(_Alignof(struct KReply)        <= KUNTYPED_ALIGN, "KReply alignment");
_Static_assert(_Alignof(struct KCNode)        <= KUNTYPED_ALIGN, "KCNode alignment");
_Static_assert(_Alignof(struct KSchedContext) <= KUNTYPED_ALIGN, "KSchedContext alignment");
/* The KObject header (type/refcounts/lock/ops) lives INSIDE the retyped
 * storage — first field of every canonical object. */
_Static_assert(__builtin_offsetof(struct KEndpoint,     base) == 0u, "header in payload");
_Static_assert(__builtin_offsetof(struct KNotification, base) == 0u, "header in payload");
_Static_assert(__builtin_offsetof(struct KReply,        base) == 0u, "header in payload");
_Static_assert(__builtin_offsetof(struct KCNode,        base) == 0u, "header in payload");
/* Userland IRIS_KOBJ_* ABI codes must mirror the kernel enum. */
_Static_assert(IRIS_KOBJ_NOTIFICATION  == (uint32_t)KOBJ_NOTIFICATION,  "KOBJ ABI");
_Static_assert(IRIS_KOBJ_ENDPOINT      == (uint32_t)KOBJ_ENDPOINT,      "KOBJ ABI");
_Static_assert(IRIS_KOBJ_CNODE         == (uint32_t)KOBJ_CNODE,         "KOBJ ABI");
_Static_assert(IRIS_KOBJ_SCHED_CONTEXT == (uint32_t)KOBJ_SCHED_CONTEXT, "KOBJ ABI");
_Static_assert(IRIS_KOBJ_UNTYPED       == (uint32_t)KOBJ_UNTYPED,       "KOBJ ABI");
_Static_assert(IRIS_KOBJ_REPLY         == (uint32_t)KOBJ_REPLY,         "KOBJ ABI");
_Static_assert(IRIS_KOBJ_FRAME         == (uint32_t)KOBJ_FRAME,         "KOBJ ABI");

/*
 * SYS_UNTYPED_RETYPE (87) — LEGACY, handle-publishing, single object.
 * Fase S1: migrated types are rejected with NOT_SUPPORTED (they must be born
 * via RETYPE2 into CSpace).  Remaining legal types: KOBJ_UNTYPED (sub-region),
 * KOBJ_FRAME, KOBJ_SCHED_CONTEXT — all still untyped-funded, ledger-tracked
 * as MIGRATING until their own convergence phase.
 */
uint64_t sys_untyped_retype(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t ut_cptr  = (iris_cptr_t)arg0;
    uint32_t    obj_type = (uint32_t)arg1;
    uint64_t    obj_arg  = arg2; /* KOBJ_UNTYPED: sub-size; KOBJ_FRAME: size; else 0 */

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* S20: the migrated family can no longer be born through a handle. */
    if (obj_type == KOBJ_ENDPOINT || obj_type == KOBJ_NOTIFICATION ||
        obj_type == KOBJ_REPLY    || obj_type == KOBJ_CNODE) {
        kuntyped_stat_retype_failure();
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }

    struct KUntyped *ut;
    iris_rights_t    rights;
    iris_error_t     err = cspace_or_handle_resolve_untyped(t->process, ut_cptr,
                                                             RIGHT_WRITE, &ut, &rights);
    if (err != IRIS_OK) return syscall_err(err);

    struct KObject *new_obj  = 0;
    iris_rights_t   new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;

    switch (obj_type) {
        case KOBJ_SCHED_CONTEXT: {
            if (ut->is_device) { /* U11: no kernel objects in device memory */
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_NOT_SUPPORTED);
            }
            void *mem = kuntyped_alloc_child(ut, sizeof(struct KSchedContext));
            if (!mem) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &kschedctx_alloc_at(mem)->base;
            break;
        }
        /* KOBJ_CHANNEL retype retired — Fase 13/Track G (KChannel removed). */
        case KOBJ_UNTYPED: {
            /* Sub-untyped: carve a physical sub-region from the parent. */
            uint64_t size = obj_arg;
            if (size < 4096u || (size & 4095u)) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_INVALID_ARG);
            }
            uint64_t phys = kuntyped_bump_alloc_phys(ut, size);
            if (!phys) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            struct KUntyped *sub = kuntyped_create(phys, size, ut->is_device);
            if (!sub) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
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
             * obj_arg is the frame size in bytes: must be >= 4096 and PAGE_SIZE
             * aligned.  The KFrame header is slab-allocated separately; the
             * physical region is the frame content (not the header storage).
             * Ledger: KFrame header sidecar = MIGRATING (frame/page-table
             * object-model phase). */
            uint64_t fsize = obj_arg;
            if (fsize < 4096u || (fsize & 4095u)) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_INVALID_ARG);
            }
            uint64_t phys = kuntyped_bump_alloc_phys_page(ut, fsize);
            if (!phys) {
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            struct KFrame *frm = kframe_alloc(phys, fsize, ut);
            if (!frm) {
                /* phys is already consumed from the bump; cannot un-bump.
                 * The wasted region will be reclaimed on next SYS_UNTYPED_RESET
                 * (which still requires child_count == 0, which remains valid). */
                kobject_active_release(&ut->base);
                kobject_release(&ut->base);
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            new_obj = &frm->base;
            break;
        }
        default:
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            kuntyped_stat_retype_failure();
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }

    /* Release our resolve refs on the parent — the new object's alloc ref is separate. */
    kobject_active_release(&ut->base);
    kobject_release(&ut->base);

    handle_id_t h = handle_table_insert(&t->process->handle_table, new_obj, new_rights);
    kobject_release(new_obj); /* transfer alloc ref to handle table */

    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    kuntyped_stat_retype(1u);
    return (uint64_t)h;
}

/*
 * SYS_UNTYPED_RETYPE2 (111) — Fase S1 canonical batch retype.
 *
 *   arg0 = source untyped (CPtr < 1024 or handle >= 1024)
 *   arg1 = obj_type (low 32) | count (high 32; 0 → 1)
 *   arg2 = dest CNode (low 32; 0 → caller's root CNode) | first dest slot (high 32)
 *   arg3 = obj_arg (KOBJ_CNODE: num_slots; KOBJ_UNTYPED/KOBJ_FRAME: bytes; else 0)
 *
 * On success every created capability sits in
 * dest_cnode[slot .. slot+count-1] and 0 is returned.  On ANY failure no
 * capability is published, no object is live, no untyped range is consumed,
 * and no CSpace slot changed (U13–U15, S5).
 *
 * The object storage IS the retyped untyped memory: header, refcounts,
 * lifecycle state and payload all live inside the source region (regla
 * central de Fase S1); destruction returns the block zeroed to the region.
 */
uint64_t sys_untyped_retype2(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                             uint64_t arg3) {
    iris_cptr_t ut_cptr    = (iris_cptr_t)arg0;
    uint32_t    obj_type   = (uint32_t)(arg1 & 0xFFFFFFFFu);
    uint32_t    count      = (uint32_t)(arg1 >> 32);
    iris_cptr_t dest_cnode = (iris_cptr_t)(arg2 & 0xFFFFFFFFu);
    uint32_t    dest_slot  = (uint32_t)(arg2 >> 32);
    uint64_t    obj_arg    = arg3;

    if (count == 0u) count = 1u;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KProcess *proc = t->process;

    /* ── validate type & compute payload size (before touching state) ── */
    uint64_t      payload = 0;
    iris_rights_t new_rights;
    switch (obj_type) {
        case KOBJ_ENDPOINT:
            payload    = sizeof(struct KEndpoint);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        case KOBJ_NOTIFICATION:
            payload    = sizeof(struct KNotification);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                         RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        case KOBJ_REPLY:
            /* Explicit MCS-style reply object (S16/S18).  DUPLICATE is
             * granted so a supervisor can mint the reply into the serving
             * child; the supervisor must then DELETE its own copy (a
             * retained copy would suppress the close-wakes-caller path). */
            payload    = sizeof(struct KReply);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER | RIGHT_DUPLICATE;
            break;
        case KOBJ_CNODE: {
            uint64_t num_slots = obj_arg ? obj_arg : KCNODE_DEFAULT_SLOTS;
            if (num_slots == 0u || num_slots > KCNODE_MAX_SLOTS ||
                (num_slots & (num_slots - 1u)) != 0u)
                { kuntyped_stat_retype_failure(); return syscall_err(IRIS_ERR_INVALID_ARG); }
            payload    = KCNODE_ALLOC_SIZE((uint32_t)num_slots);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        }
        case KOBJ_SCHED_CONTEXT:
            payload    = sizeof(struct KSchedContext);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        case KOBJ_UNTYPED:
        case KOBJ_FRAME:
            /* Physical-region types keep count == 1 in S1 (their sidecar
             * headers are not yet untyped-backed — ledger MIGRATING). */
            if (count != 1u)
                { kuntyped_stat_retype_failure(); return syscall_err(IRIS_ERR_INVALID_ARG); }
            if (obj_arg < 4096u || (obj_arg & 4095u))
                { kuntyped_stat_retype_failure(); return syscall_err(IRIS_ERR_INVALID_ARG); }
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        default:
            kuntyped_stat_retype_failure();
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
    if (count > KUNTYPED_RETYPE_MAX_COUNT) {
        kuntyped_stat_retype_failure();
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* ── resolve source untyped (WRITE) ── */
    struct KUntyped *ut;
    iris_rights_t    ut_rights;
    iris_error_t err = cspace_or_handle_resolve_untyped(proc, ut_cptr,
                                                        RIGHT_WRITE, &ut, &ut_rights);
    if (err != IRIS_OK) { kuntyped_stat_retype_failure(); return syscall_err(err); }

    /* U11/U12: device untyped only produces physical-region types. */
    if (ut->is_device && obj_type != KOBJ_UNTYPED && obj_type != KOBJ_FRAME) {
        kobject_active_release(&ut->base);
        kobject_release(&ut->base);
        kuntyped_stat_retype_failure();
        return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }

    /* ── resolve destination CNode (WRITE); 0 = caller's root ── */
    struct KCNode *cn = 0;
    if (dest_cnode == 0u) {
        if (proc->cspace_root_h == HANDLE_INVALID) {
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            kuntyped_stat_retype_failure();
            return syscall_err(IRIS_ERR_NOT_FOUND);
        }
        struct KObject *root_obj;
        iris_rights_t   root_r;
        err = handle_table_get_object(&proc->handle_table, proc->cspace_root_h,
                                      &root_obj, &root_r);
        if (err == IRIS_OK && root_obj->type != KOBJ_CNODE) {
            kobject_release(root_obj);
            err = IRIS_ERR_INTERNAL;
        }
        if (err != IRIS_OK) {
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            kuntyped_stat_retype_failure();
            return syscall_err(err);
        }
        kobject_active_retain(root_obj); /* match resolve_cnode contract */
        cn = (struct KCNode *)root_obj;
    } else {
        iris_rights_t cn_rights;
        err = cspace_or_handle_resolve_cnode(proc, dest_cnode, RIGHT_WRITE,
                                             &cn, &cn_rights);
        if (err != IRIS_OK) {
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            kuntyped_stat_retype_failure();
            return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
        }
    }

    /* ── destination slot range: in bounds and currently empty ──
     * Slot 0 is CPTR_NULL when addressing the root: refuse it so every
     * published cap is actually invocable. */
    err = IRIS_OK;
    if (dest_slot == 0u || (uint64_t)dest_slot + count > cn->slot_count)
        err = IRIS_ERR_INVALID_ARG;
    if (err == IRIS_OK) {
        uint64_t cfl = irq_spinlock_lock(&cn->lock);
        for (uint32_t i = 0; i < count; i++) {
            if (cn->slots[dest_slot + i].object) { err = IRIS_ERR_ALREADY_EXISTS; break; }
        }
        irq_spinlock_unlock(&cn->lock, cfl);
    }
    if (err != IRIS_OK) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        kobject_active_release(&ut->base);
        kobject_release(&ut->base);
        kuntyped_stat_retype_failure();
        kuntyped_stat_overlap_denial();
        return syscall_err(err);
    }

    /* ── create the object(s) ── */
    struct KObject *objs[KUNTYPED_RETYPE_MAX_COUNT];
    uint64_t carve_start = 0, carve_end = 0;

    if (obj_type == KOBJ_UNTYPED || obj_type == KOBJ_FRAME) {
        /* Single physical-region object (count == 1, validated above). */
        if (obj_type == KOBJ_UNTYPED) {
            uint64_t phys = kuntyped_bump_alloc_phys(ut, obj_arg);
            if (!phys) err = IRIS_ERR_NO_MEMORY;
            else {
                struct KUntyped *sub = kuntyped_create(phys, obj_arg, ut->is_device);
                if (!sub) err = IRIS_ERR_NO_MEMORY;
                else {
                    sub->alloc_parent = ut;
                    kobject_retain(&ut->base);
                    atomic_fetch_add_explicit(&ut->child_count, 1u, memory_order_relaxed);
                    objs[0] = &sub->base;
                }
            }
        } else {
            uint64_t phys = kuntyped_bump_alloc_phys_page(ut, obj_arg);
            if (!phys) err = IRIS_ERR_NO_MEMORY;
            else {
                struct KFrame *frm = kframe_alloc(phys, obj_arg, ut);
                if (!frm) err = IRIS_ERR_NO_MEMORY;
                else objs[0] = &frm->base;
            }
        }
    } else {
        void *ptrs[KUNTYPED_RETYPE_MAX_COUNT];
        {
            /* Record the carve window for exact rollback (U15). */
            uint64_t f = irq_spinlock_lock(&ut->lock);
            carve_start = ut->used;
            irq_spinlock_unlock(&ut->lock, f);
        }
        err = kuntyped_alloc_children_atomic(ut, payload, count, ptrs);
        if (err == IRIS_OK) {
            uint64_t f = irq_spinlock_lock(&ut->lock);
            carve_end = ut->used;
            irq_spinlock_unlock(&ut->lock, f);
            for (uint32_t i = 0; i < count; i++) {
                switch (obj_type) {
                    case KOBJ_ENDPOINT:
                        objs[i] = &kendpoint_alloc_at(ptrs[i])->base;             break;
                    case KOBJ_NOTIFICATION:
                        objs[i] = &knotification_alloc_at(ptrs[i])->base;         break;
                    case KOBJ_REPLY:
                        objs[i] = &kreply_alloc_at(ptrs[i])->base;                break;
                    case KOBJ_CNODE:
                        objs[i] = &kcnode_alloc_at(ptrs[i],
                                    (uint32_t)(obj_arg ? obj_arg
                                                       : KCNODE_DEFAULT_SLOTS))->base; break;
                    default: /* KOBJ_SCHED_CONTEXT */
                        objs[i] = &kschedctx_alloc_at(ptrs[i])->base;             break;
                }
            }
        }
    }
    if (err != IRIS_OK) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        kobject_active_release(&ut->base);
        kobject_release(&ut->base);
        kuntyped_stat_retype_failure();
        return syscall_err(err);
    }

    /* ── publish: all destination slots in one critical section ── */
    err = IRIS_OK;
    {
        uint64_t cfl = irq_spinlock_lock(&cn->lock);
        for (uint32_t i = 0; i < count; i++) {
            if (cn->slots[dest_slot + i].object) { err = IRIS_ERR_ALREADY_EXISTS; break; }
        }
        if (err == IRIS_OK) {
            for (uint32_t i = 0; i < count; i++) {
                kobject_retain(objs[i]);
                kobject_active_retain(objs[i]);
                cn->slots[dest_slot + i].object = objs[i];
                cn->slots[dest_slot + i].rights = new_rights;
                cn->slots[dest_slot + i].badge  = 0;
            }
        }
        irq_spinlock_unlock(&cn->lock, cfl);
    }

    if (err != IRIS_OK) {
        /* Roll back: destroy every created object (returns its block to the
         * region and drops child_count), then un-bump the carve exactly. */
        for (uint32_t i = 0; i < count; i++)
            kobject_release(objs[i]);
        if (carve_end > carve_start)
            kuntyped_unbump_exact(ut, carve_start, carve_end);
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        kobject_active_release(&ut->base);
        kobject_release(&ut->base);
        kuntyped_stat_retype_failure();
        kuntyped_stat_overlap_denial();
        return syscall_err(err);
    }

    /* Commit: drop the alloc refs (each slot holds retain + active_retain). */
    for (uint32_t i = 0; i < count; i++)
        kobject_release(objs[i]);

    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    kobject_active_release(&ut->base);
    kobject_release(&ut->base);
    kuntyped_stat_retype(count);
    return 0;
}

/*
 * SYS_UNTYPED_RESET — reset the bump pointer to 0 if no children are live.
 *
 * The caller is responsible for ensuring all objects and sub-untypeds created
 * from this untyped have been destroyed (capabilities deleted, kobject
 * refcounts at 0) before calling RESET.  The kernel enforces this via
 * child_count: if any child is still alive, RESET returns IRIS_ERR_BUSY (S13).
 *
 * After a successful RESET the entire physical region can be retyped fresh;
 * the generation bump is the reuse witness (S12/S28).
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
    uint64_t reclaimed = ut->used;
    ut->used = 0;
    ut->generation++;
    irq_spinlock_unlock(&ut->lock, flags);
    kuntyped_stat_reset(reclaimed, reclaimed != 0u);

    kobject_active_release(&ut->base);
    kobject_release(&ut->base);
    return 0;
}

/*
 * Fase S2 C.1 — versioned user-buffer copy hardening.
 *
 * The kernel must never write beyond the buffer the caller declared, and must
 * never depend on the caller's struct matching the kernel's exact size.  The
 * caller declares (version, size); the kernel:
 *   - rejects a size below the mandatory 8-byte header (version+struct_size);
 *   - rejects a version it does not support (0 = don't-care/prefix);
 *   - fills a zeroed kernel struct (reserved fields stay 0);
 *   - writes at most min(declared_size, kernel_size) — prefix-compatible;
 *   - validates the user range for exactly those bytes;
 *   - copies nothing on any failure (no partial write, no state change).
 * The kernel struct's own struct_size field always advertises the supported
 * size so a prefix caller learns there is more.
 */
#define IRIS_QUERY_HEADER_MIN 8u   /* version(u32) + struct_size(u32) */

static iris_error_t copy_versioned_to_user(uint64_t uptr, uint32_t user_size,
                                           uint32_t user_version,
                                           const void *ksrc, uint32_t ksize,
                                           uint32_t kversion) {
    if (!uptr) return IRIS_ERR_INVALID_ARG;
    if (user_size < IRIS_QUERY_HEADER_MIN) return IRIS_ERR_INVALID_ARG;
    if (user_version != 0u && user_version != kversion) return IRIS_ERR_INVALID_ARG;
    uint32_t n = user_size < ksize ? user_size : ksize;   /* prefix clamp */
    if (!user_range_writable(uptr, n)) return IRIS_ERR_INVALID_ARG;
    if (!copy_to_user_checked(uptr, ksrc, n)) return IRIS_ERR_INVALID_ARG;
    return IRIS_OK;
}

/*
 * SYS_UNTYPED_QUERY (112) — versioned, read-only instrumentation.
 *
 *   arg0 = kind (low 16) | version (bits 16..31) | user_size (high 32)
 *          version 0 = don't-care (prefix compatible)
 *   arg1 = user buffer
 *   arg2 = untyped CPtr/handle (kind 2 only)
 *
 * Diagnostics only: no authority flows through this syscall and it never
 * mutates state.  C.1: the kernel knows the caller's buffer size and never
 * writes past it (see copy_versioned_to_user).
 */
uint64_t sys_untyped_query(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint32_t kind         = (uint32_t)(arg0 & 0xFFFFu);
    uint32_t user_version = (uint32_t)((arg0 >> 16) & 0xFFFFu);
    uint32_t user_size    = (uint32_t)(arg0 >> 32);
    uint64_t buf_uptr     = arg1;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!buf_uptr) return syscall_err(IRIS_ERR_INVALID_ARG);

    switch (kind) {
        case IRIS_UNTYPED_QUERY_GLOBAL: {
            struct iris_untyped_query_global q;
            for (uint32_t i = 0; i < (uint32_t)sizeof(q); i++) ((uint8_t *)&q)[i] = 0;
            struct kuntyped_stats st;
            kuntyped_stats_get(&st);
            q.version         = IRIS_UNTYPED_QUERY_VERSION;
            q.struct_size     = (uint32_t)sizeof(q);
            q.live_untypeds   = kuntyped_live_count();
            q.retype_count    = st.retype_count;
            q.retype_failures = st.retype_failures;
            q.reset_count     = st.reset_count;
            q.reclaimed_bytes = st.reclaimed_bytes;
            q.reuse_count     = st.reuse_count;
            q.overlap_denials = st.overlap_denials;
            return syscall_err(copy_versioned_to_user(buf_uptr, user_size, user_version,
                               &q, (uint32_t)sizeof(q), IRIS_UNTYPED_QUERY_VERSION));
        }
        case IRIS_UNTYPED_QUERY_ONE: {
            struct KUntyped *ut;
            iris_rights_t    rights;
            iris_error_t err = cspace_or_handle_resolve_untyped(t->process,
                                    (iris_cptr_t)arg2, RIGHT_READ, &ut, &rights);
            if (err != IRIS_OK) return syscall_err(err);
            struct iris_untyped_query_one q;
            for (uint32_t i = 0; i < (uint32_t)sizeof(q); i++) ((uint8_t *)&q)[i] = 0;
            uint64_t f = irq_spinlock_lock(&ut->lock);
            q.phys_base   = ut->phys_base;
            q.total_bytes = ut->total_size;
            q.used_bytes  = ut->used;
            q.generation  = ut->generation;
            q.child_count = atomic_load_explicit(&ut->child_count, memory_order_relaxed);
            q.is_device   = (uint32_t)ut->is_device;
            irq_spinlock_unlock(&ut->lock, f);
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            q.version     = IRIS_UNTYPED_QUERY_VERSION;
            q.struct_size = (uint32_t)sizeof(q);
            return syscall_err(copy_versioned_to_user(buf_uptr, user_size, user_version,
                               &q, (uint32_t)sizeof(q), IRIS_UNTYPED_QUERY_VERSION));
        }
        case IRIS_UNTYPED_QUERY_OBJECTS: {
            struct iris_untyped_query_objects q;
            for (uint32_t i = 0; i < (uint32_t)sizeof(q); i++) ((uint8_t *)&q)[i] = 0;
            q.version        = IRIS_UNTYPED_QUERY_VERSION;
            q.struct_size    = (uint32_t)sizeof(q);
            q.endpoints_live     = kendpoint_live_count();
            q.notifications_live = knotification_live_count();
            q.replies_live       = kreply_live_count();
            q.cnodes_live        = kcnode_live_count();
            return syscall_err(copy_versioned_to_user(buf_uptr, user_size, user_version,
                               &q, (uint32_t)sizeof(q), IRIS_UNTYPED_QUERY_VERSION));
        }
        case IRIS_UNTYPED_QUERY_TASKOBJ: {
            struct iris_untyped_query_taskobj q;
            for (uint32_t i = 0; i < (uint32_t)sizeof(q); i++) ((uint8_t *)&q)[i] = 0;
            q.version     = IRIS_UNTYPED_QUERY_VERSION;
            q.struct_size = (uint32_t)sizeof(q);
            ktcb_stats(&q.tcb_live, &q.tcb_hwm, &q.tcb_retyped, &q.tcb_destroyed);
            kschedctx_stats(&q.sc_live, &q.sc_hwm, &q.sc_retyped, &q.sc_destroyed);
            kcnode_cdt_stats(&q.cdt_derivation_count, &q.cdt_derivation_hwm,
                             &q.cdt_revoke_count, &q.cdt_delete_count,
                             &q.cdt_cross_cnode_descendants,
                             &q.cdt_ipc_transfer_count,
                             &q.legacy_handle_derivation_migrated);
            task_registry_stats(&q.tcb_registry_active, &q.tcb_registry_hwm,
                                &q.tcb_registry_exhaustions,
                                &q.tcb_registry_generation_mismatch);
            return syscall_err(copy_versioned_to_user(buf_uptr, user_size, user_version,
                               &q, (uint32_t)sizeof(q), IRIS_UNTYPED_QUERY_VERSION));
        }
        default:
            return syscall_err(IRIS_ERR_INVALID_ARG);
    }
}
