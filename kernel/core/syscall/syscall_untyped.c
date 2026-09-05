/*
 * syscall_untyped.c — KUntyped authority paths.
 *
 * Phase S1 (seL4 Architectural Convergence):
 *
 * SYS_UNTYPED_INFO:    query phys_base and available bytes.
 * SYS_UNTYPED_RETYPE:  RETIRED (Stage 4).  The number stays permanently
 *   reserved and answers NOT_SUPPORTED.  It was the LEGACY single-object
 *   retype that published the new capability as a HANDLE.  Phase S1 already
 *   refused the migrated family (Endpoint / Notification / Reply / CNode) on
 *   it; Stage 4 refuses the remaining three (KUntyped sub-regions, KFrame,
 *   KSchedContext) too, because RETYPE2 accepts all of them into a CSpace
 *   slot.  There is now exactly ONE way to create an object from an Untyped,
 *   and it puts the result where the object model says capabilities live.
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
 * cspace_resolve_only_untyped (a CPtr resolves through CSpace only, a
 * handle value through the handle table only; ACCESS_DENIED is a hard stop,
 * no fallback).
 *
 * Atomicity note (U14/U15): IRIS is uniprocessor with IRQ-off spinlocks and a
 * non-preemptive kernel (no yield inside retype).  RETYPE2 validates
 * everything before mutating: capacity+carve are one critical section
 * (kuntyped_alloc_children_atomic) and slot publication re-checks occupancy;
 * a publication conflict rolls back every object and un-bumps the carve
 * exactly, so a failed batch consumes nothing.
 */
#include "syscall_priv.h"
#include <iris/pmm.h>
#include <iris/kslab.h>
#include <iris/idt.h>
#include <iris/nc/kreply.h>

uint64_t sys_untyped_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    iris_cptr_t ut_cptr    = (iris_cptr_t)arg0;
    uint64_t    phys_uptr  = arg1;
    uint64_t    avail_uptr = arg2;

    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KUntyped *ut;
    iris_rights_t    rights;
    iris_error_t     err = cspace_resolve_only_untyped(t->cspace_root, ut_cptr,
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
_Static_assert(_Alignof(struct task)          <= KUNTYPED_ALIGN, "KTCB alignment");
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
_Static_assert(IRIS_KOBJ_PAGE_TABLE    == (uint32_t)KOBJ_PAGE_TABLE,    "KOBJ ABI");
_Static_assert(IRIS_KOBJ_VSPACE        == (uint32_t)KOBJ_VSPACE,        "KOBJ ABI");
_Static_assert(IRIS_KOBJ_TCB           == (uint32_t)KOBJ_TCB,           "KOBJ ABI");

/*
 * SYS_UNTYPED_RETYPE (87) — LEGACY, handle-publishing, single object.
 * Phase S1: migrated types are rejected with NOT_SUPPORTED (they must be born
 * via RETYPE2 into CSpace).  Remaining legal types: KOBJ_UNTYPED (sub-region),
 * KOBJ_FRAME, KOBJ_SCHED_CONTEXT — all still untyped-funded, ledger-tracked
 * as MIGRATING until their own convergence phase.
 */
uint64_t sys_untyped_retype(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

/*
 * The two physical-region types, carved.
 *
 * Stage 6 Step 1/4: BOTH halves come from this Untyped — the region from the
 * bottom and the object's header from the TOP, so paying for the header does
 * not push the page-aligned carve onto the next boundary, and the header can
 * never land inside a page that is about to be mapped into ring 3.  The header
 * carve is what holds the child_count entry, so it is taken first: if the
 * region carve then fails, releasing the header undoes the accounting exactly.
 *
 * DEVICE untypeds are the exception, and U11 is the rule they follow: a
 * kernel object header carved from an MMIO window would put a spinlock and a
 * refcount where loads and stores reach a device, and zero-filling the block
 * would drive that write into the device's registers.  Their sidecar headers
 * stay on the kernel slab — which is what a device region cost before Stage 6
 * and still costs, because charging is for memory a holder can spend and a
 * device window is not that.
 */
static iris_error_t retype_sub_untyped(struct KUntyped *ut, uint64_t obj_arg,
                                       struct KObject **out) {
    /*
     * The region is carved PAGE-aligned, not merely 64-byte aligned.
     *
     * A sub-untyped's size is already required to be a page multiple, so a
     * base that is not page-aligned makes the region agree with nothing: every
     * frame, page table and VSpace retyped out of it has to re-align upward,
     * so the FIRST such retype silently burns up to a page that the child's
     * `available` still counted.  The waste is invisible from ring 3 — a
     * caller who buys 256 KiB and spends it on frames simply gets one fewer
     * than arithmetic says — and it moves whenever an unrelated kernel struct
     * changes size, because that shifts the parent's bump pointer.
     *
     * seL4 has no such case: an untyped object is always naturally aligned to
     * its own size, which is what makes the retype arithmetic there exact.
     * Page granularity is the weaker property IRIS needs and can afford.
     */
    if (ut->is_device) {
        /*
         * D-9: the header comes from the RAM budget this device Untyped is
         * PAIRED with, never from the kernel slab.  A device region is MMIO —
         * a struct written into a framebuffer is pixels — so the header has to
         * be RAM, and the only answer that does not put the kernel back in the
         * business of allocating on somebody's behalf is that the holder named
         * it.  Unpaired, this refuses: the kernel does not know whose memory to
         * spend and will not guess.
         */
        if (!ut->hdr_budget) return IRIS_ERR_INVALID_ARG;
        void *hdr = kuntyped_alloc_child_top(ut->hdr_budget,
                                             sizeof(struct KUntyped));
        if (!hdr) return IRIS_ERR_NO_MEMORY;
        uint64_t phys = kuntyped_bump_alloc_phys_page(ut, obj_arg);
        if (!phys) {
            kuntyped_release_child(hdr, sizeof(struct KUntyped));
            return IRIS_ERR_NO_MEMORY;
        }
        struct KUntyped *sub = kuntyped_create_at(hdr, phys, obj_arg, 1);
        if (!sub) {
            kuntyped_release_child(hdr, sizeof(struct KUntyped));
            return IRIS_ERR_NO_MEMORY;
        }
        /* The device region's own child accounting still belongs to the device
         * Untyped: it is what RESET on the device region must refuse against.
         * The header's accounting rides on the block, in the RAM budget. */
        sub->alloc_parent = 0;
        kobject_retain(&ut->base);
        atomic_fetch_add_explicit(&ut->child_count, 1u, memory_order_relaxed);
        *out = &sub->base;
        return IRIS_OK;
    }

    void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KUntyped));
    if (!hdr) return IRIS_ERR_NO_MEMORY;
    uint64_t phys = kuntyped_bump_alloc_phys_page(ut, obj_arg);
    if (!phys) {
        kuntyped_release_child(hdr, sizeof(struct KUntyped));
        return IRIS_ERR_NO_MEMORY;
    }
    struct KUntyped *sub = kuntyped_create_at(hdr, phys, obj_arg, ut->is_device);
    if (!sub) {
        kuntyped_release_child(hdr, sizeof(struct KUntyped));
        return IRIS_ERR_NO_MEMORY;
    }
    *out = &sub->base;
    return IRIS_OK;
}

/*
 * Stage 6-pure Step 1 — a paging level, retyped.
 *
 * Same two-ended shape as a frame: the 4 KiB region from the bottom, the
 * header block from the top.  Two differences, both consequences of what the
 * region becomes.  It is ALWAYS exactly one page — a paging level is 512
 * entries of 8 bytes and nothing else — and it is zeroed here rather than at
 * install time, because a table is walked by hardware the instant it is
 * installed and stale bytes are a walk into whatever the region held before.
 * Device memory is refused outright: a page table must be RAM the MMU can
 * read as a table.
 */
static iris_error_t retype_page_table(struct KUntyped *ut, uint64_t obj_arg,
                                      struct KObject **out) {
    if (obj_arg != 4096u) return IRIS_ERR_INVALID_ARG;
    if (ut->is_device)    return IRIS_ERR_NOT_SUPPORTED;

    void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KPageTable));
    if (!hdr) return IRIS_ERR_NO_MEMORY;
    uint64_t phys = kuntyped_bump_alloc_phys_page(ut, 4096u);
    if (!phys) {
        kuntyped_release_child(hdr, sizeof(struct KPageTable));
        return IRIS_ERR_NO_MEMORY;
    }
    struct KPageTable *pt = kpagetable_alloc_at(hdr, phys);
    if (!pt) {
        kuntyped_release_child(hdr, sizeof(struct KPageTable));
        return IRIS_ERR_NO_MEMORY;
    }
    kpagetable_zero(pt);
    *out = &pt->base;
    return IRIS_OK;
}

/*
 * Stage 6-pure Step 4 — an address space, retyped.
 *
 * The top level of a walk is a page like any other level, so a VSpace is
 * carved exactly like a page table: the PML4 from the bottom, the header from
 * the top.  What makes it a VSpace rather than a KPageTable is what the kernel
 * writes into that page — the shared low window and the higher half — and the
 * bookkeeping the header carries for the levels the holder will hang under it.
 *
 * The Untyped it came from becomes its pool, so the address space's mapping
 * records come from the region the address space itself lives in.  That is not
 * a convenience: it means a holder that RESETs the region gets ALL of it back,
 * with no second budget to remember.
 */
static iris_error_t retype_vspace(struct KUntyped *ut, uint64_t obj_arg,
                                  struct KObject **out) {
    if (obj_arg != 4096u) return IRIS_ERR_INVALID_ARG;
    if (ut->is_device)    return IRIS_ERR_NOT_SUPPORTED;

    void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KVSpace));
    if (!hdr) return IRIS_ERR_NO_MEMORY;
    /* A page CHILD, not a bare carve: the PML4 outlives every level under it,
     * so the region must refuse RESET for as long as the address space does. */
    uint64_t phys = kuntyped_alloc_page_child(ut);
    if (!phys) {
        kuntyped_release_child(hdr, sizeof(struct KVSpace));
        return IRIS_ERR_NO_MEMORY;
    }
    struct KVSpace *vs = kvspace_retype_at(hdr, phys, ut);
    if (!vs) {
        kuntyped_release_page_child(ut);
        kuntyped_release_child(hdr, sizeof(struct KVSpace));
        return IRIS_ERR_NO_MEMORY;
    }
    *out = &vs->base;
    return IRIS_OK;
}

static iris_error_t retype_frame(struct KUntyped *ut, uint64_t obj_arg,
                                 struct KObject **out) {
    if (ut->is_device) {
        /* D-9, same rule as a sub-untyped: the frame's header is RAM the
         * holder named, or there is no frame. */
        if (!ut->hdr_budget) return IRIS_ERR_INVALID_ARG;
        void *hdr = kuntyped_alloc_child_top(ut->hdr_budget,
                                             sizeof(struct KFrame));
        if (!hdr) return IRIS_ERR_NO_MEMORY;
        uint64_t phys = kuntyped_bump_alloc_phys_page(ut, obj_arg);
        if (!phys) {
            kuntyped_release_child(hdr, sizeof(struct KFrame));
            return IRIS_ERR_NO_MEMORY;
        }
        struct KFrame *frm = kframe_alloc_at(hdr, phys, obj_arg);
        if (!frm) {
            kuntyped_release_child(hdr, sizeof(struct KFrame));
            return IRIS_ERR_NO_MEMORY;
        }
        atomic_fetch_add_explicit(&ut->child_count, 1u, memory_order_relaxed);
        kobject_retain(&ut->base);
        *out = &frm->base;
        return IRIS_OK;
    }

    void *hdr = kuntyped_alloc_child_top(ut, sizeof(struct KFrame));
    if (!hdr) return IRIS_ERR_NO_MEMORY;
    uint64_t phys = kuntyped_bump_alloc_phys_page(ut, obj_arg);
    if (!phys) {
        kuntyped_release_child(hdr, sizeof(struct KFrame));
        return IRIS_ERR_NO_MEMORY;
    }
    struct KFrame *frm = kframe_alloc_at(hdr, phys, obj_arg);
    if (!frm) {
        kuntyped_release_child(hdr, sizeof(struct KFrame));
        return IRIS_ERR_NO_MEMORY;
    }
    *out = &frm->base;
    return IRIS_OK;
}

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
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

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
            /*
             * Stage 8-mcs: `obj_arg` is the REFILL DEPTH, seL4's `refill_max`,
             * and it sizes the object.  A passive server woken per request
             * needs many pending replenishments; a periodic task needs two.
             * Making it a kernel constant would put that memory back in the
             * kernel's hands and charge every SC for the worst case, which is
             * the arrangement Stage 6 exists to remove.  0 asks for the
             * default, so a caller that does not care does not have to decide.
             */
            if (obj_arg != 0u && (obj_arg < KSCHEDCTX_REFILL_MIN ||
                                  obj_arg > KSCHEDCTX_REFILL_LIMIT)) {
                kuntyped_stat_retype_failure();
                return syscall_err(IRIS_ERR_INVALID_ARG);
            }
            payload    = (uint32_t)kschedctx_bytes((uint32_t)obj_arg);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        case KOBJ_TCB:
            /* Phase S2 Step 0: canonical TCB birth.  The object is INACTIVE
             * (configured = 0): observable, delegable, destroyable — but not
             * runnable until TCB_CONFIGURE (roadmap Step 5/6).  Execution
             * syscalls refuse it with NOT_SUPPORTED. */
            payload    = sizeof(struct task);
            new_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            break;
        case KOBJ_UNTYPED:
        case KOBJ_FRAME:
        case KOBJ_PAGE_TABLE:
        case KOBJ_VSPACE:
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
    iris_error_t err = cspace_resolve_only_untyped(t->cspace_root, ut_cptr,
                                                        RIGHT_WRITE, &ut, &ut_rights);
    if (err != IRIS_OK) { kuntyped_stat_retype_failure(); return syscall_err(err); }

    /* Phase S3 (D.1): the MDB ancestry of the created caps is the SLOT of the
     * source untyped — resolved just before publication (see below). */

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
        /* Stage 4: structural root read — retype into the caller's root CNode
         * no longer goes through the handle table. */
        if (!t->cspace_root) {
            kobject_active_release(&ut->base);
            kobject_release(&ut->base);
            kuntyped_stat_retype_failure();
            return syscall_err(IRIS_ERR_NOT_FOUND);
        }
        struct KObject *root_obj = &t->cspace_root->base;
        kobject_retain(root_obj);
        kobject_active_retain(root_obj); /* match resolve_cnode contract */
        cn = (struct KCNode *)root_obj;
    } else {
        iris_rights_t cn_rights;
        err = cspace_resolve_only_cnode(t->cspace_root, dest_cnode, RIGHT_WRITE,
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

    if (obj_type == KOBJ_UNTYPED || obj_type == KOBJ_FRAME ||
        obj_type == KOBJ_PAGE_TABLE || obj_type == KOBJ_VSPACE) {
        /* Single physical-region object (count == 1, validated above). */
        if      (obj_type == KOBJ_UNTYPED)    err = retype_sub_untyped(ut, obj_arg, &objs[0]);
        else if (obj_type == KOBJ_FRAME)      err = retype_frame(ut, obj_arg, &objs[0]);
        else if (obj_type == KOBJ_PAGE_TABLE) err = retype_page_table(ut, obj_arg, &objs[0]);
        else                                  err = retype_vspace(ut, obj_arg, &objs[0]);
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
                    case KOBJ_TCB:
                        objs[i] = &ktcb_alloc_at(ptrs[i])->base;                  break;
                    default: /* KOBJ_SCHED_CONTEXT */
                        objs[i] = &kschedctx_alloc_at(ptrs[i], (uint32_t)obj_arg)->base;             break;
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

    /* ── publish through the canonical slot primitive (Phase S3) ──
     * Each created cap is installed as an MDB CHILD of the source untyped's
     * slot.  Exclusive installs re-verify occupancy; a conflict unwinds the
     * slots installed so far (fresh leaves — their delete has no reparent
     * effect) and then rolls back objects + carve exactly (U14/U15).
     *
     * The predicate is `cspace_value_is_cptr`, and it used to be an open-coded
     * `< 1024`.  That number was the handle-namespace split, and cspace.h says
     * in words that nobody may open-code it — but it also means something
     * else here: a SECOND-LEVEL CPtr is `(leaf << 8) | root_slot`, which is
     * >= 1024 for every leaf above 3.  So every object retyped from an Untyped
     * held below the root became a LEGACY ROOT: no MDB parent, unreachable by
     * any revoke, and reclaimable only by resetting a region whose children
     * nothing could destroy.  The suite holds all its budgets in a second-level
     * CNode, so this was most of what it retyped.  T321 is the test. */
    struct KCNode *ut_slot_cn  = 0;
    uint32_t       ut_slot_idx = 0;
    if (cspace_value_is_cptr(ut_cptr)) {
        if (cspace_resolve_slot(t->cspace_root, ut_cptr, &ut_slot_cn, &ut_slot_idx)
                != IRIS_OK)
            ut_slot_cn = 0;   /* defensive: fall back to legacy root */
    }

    err = IRIS_OK;
    uint32_t installed = 0;
    for (uint32_t i = 0; i < count; i++) {
        err = kcnode_slot_install_linked(cn, dest_slot + i, objs[i],
                                         new_rights, 0,
                                         ut_slot_cn, ut_slot_idx,
                                         /*exclusive=*/1,
                                         /*legacy=*/ut_slot_cn ? 0 : 1);
        if (err != IRIS_OK) break;
        installed++;
    }

    if (ut_slot_cn) {
        kobject_active_release(&ut_slot_cn->base);
        kobject_release(&ut_slot_cn->base);
    }

    if (err != IRIS_OK) {
        /* Unwind the freshly installed slots (leaves, no children yet), then
         * destroy every created object (returns its block to the region and
         * drops child_count) and un-bump the carve exactly. */
        for (uint32_t j = 0; j < installed; j++)
            (void)kcnode_slot_delete(cn, dest_slot + j);
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
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KUntyped *ut;
    iris_rights_t    rights;
    iris_error_t     err = cspace_resolve_only_untyped(t->cspace_root, ut_cptr,
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
    /* Stage 6 Step 1: both ends are reclaimed.  A reset with live children is
     * BUSY (checked above), so the header region at the top cannot be taken
     * back from an object that is still alive. */
    uint64_t reclaimed = ut->used + ut->used_top;
    ut->used     = 0;
    ut->used_top = 0;
    ut->generation++;
    irq_spinlock_unlock(&ut->lock, flags);
    kuntyped_stat_reset(reclaimed, reclaimed != 0u);

    kobject_active_release(&ut->base);
    kobject_release(&ut->base);
    return 0;
}

/*
 * Phase S2 C.1 — versioned user-buffer copy hardening.
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
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
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
            /* Stage 7-mem: the global gauges SYS_RESOURCE_INFO happened to
             * carry, moved to where the rest of the global instrumentation
             * already lives. */
            q.kslab_used_bytes      = kslab_used_bytes();
            q.kslab_total_bytes     = kslab_total_bytes();
            q.kslab_failed_allocs   = kslab_fail_count();
            q.global_failed_charges = kprocess_quota_failed_count();
            q.global_rollbacks      = kprocess_quota_rollback_count();
            q.syscall_restarts      = syscall_restart_count();
            q.syscall_abandons      = syscall_abandon_count();
            q.irq_ctx_saves         = (uint32_t)irq_user_ctx_saves();
            q.ipc_buffers           = ipc_buffers_registered();
            q.kernel_free_pages     = (uint32_t)pmm_free_pages();
            q.kernel_heap_sealed    = (uint32_t)kslab_is_sealed();
            return syscall_err(copy_versioned_to_user(buf_uptr, user_size, user_version,
                               &q, (uint32_t)sizeof(q), IRIS_UNTYPED_QUERY_VERSION));
        }
        case IRIS_UNTYPED_QUERY_ONE: {
            struct KUntyped *ut;
            iris_rights_t    rights;
            iris_error_t err = cspace_resolve_only_untyped(t->cspace_root,
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
            kcnode_mdb_stats(&q.mdb_nodes_live, &q.mdb_nodes_hwm,
                             &q.mdb_legacy_roots, &q.mdb_orphan_promotions,
                             &q.mdb_reparents, &q.mdb_revoked_nodes,
                             &q.mdb_moves, &q.mdb_max_depth);
            return syscall_err(copy_versioned_to_user(buf_uptr, user_size, user_version,
                               &q, (uint32_t)sizeof(q), IRIS_UNTYPED_QUERY_VERSION));
        }
        default:
            return syscall_err(IRIS_ERR_INVALID_ARG);
    }
}

/*
 * SYS_UNTYPED_SET_DEVICE_BUDGET — ledger D-9.
 *
 * See the ABI note in syscall.h for why a device Untyped needs a RAM one at
 * all.  What is worth saying here is the ordering rule the pairing exists to
 * protect: the budget is RETAINED while paired, so the RESET that would
 * reclaim it already refuses while any header carved from it is alive, and the
 * pairing cannot move, so no header can be stranded in a region that is then
 * reset.  Both properties come from the retain; neither needs bookkeeping.
 */
uint64_t sys_untyped_set_device_budget(uint64_t arg0, uint64_t arg1,
                                       uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KUntyped *dev; iris_rights_t dr;
    iris_error_t err = cspace_resolve_only_untyped(t->cspace_root,
                           (iris_cptr_t)arg0, RIGHT_WRITE, &dev, &dr);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);

    struct KUntyped *ram; iris_rights_t rr;
    err = cspace_resolve_only_untyped(t->cspace_root, (iris_cptr_t)arg1,
                                      RIGHT_WRITE, &ram, &rr);
    if (err != IRIS_OK) {
        kobject_active_release(&dev->base);
        kobject_release(&dev->base);
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
    }

    err = kuntyped_set_hdr_budget(dev, ram);

    kobject_active_release(&ram->base);
    kobject_release(&ram->base);
    kobject_active_release(&dev->base);
    kobject_release(&dev->base);
    return err == IRIS_OK ? syscall_ok_u64(0) : syscall_err(err);
}
