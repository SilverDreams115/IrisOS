/*
 * syscall_iospace.c — what a DEVICE may reach (Stage 10-dma, §10.2 steps 4/5).
 *
 * Four invocations, and between them they are the whole of the stage's claim.
 * An IOSpace names one device; page-table levels the holder paid for build its
 * address space; a frame mapped into it is a frame that device may touch; and
 * unmapping one takes that back — from the hardware's cache as well as from
 * the table, because a revoke the unit has not been told about is a revoke
 * that did not happen.
 *
 * ── Why binding takes an authority and mapping does not ────────────────────
 *
 * Retyping an IOSpace is paying for an object out of your own memory: anyone
 * holding an Untyped may.  Naming a DEVICE is saying which piece of hardware
 * this address space belongs to, and that is authority — IOSPACE_CONTROL, the
 * same shape as configuring a scheduling context against SchedControl.
 *
 * Once it is bound, mapping into it needs only the IOSpace capability and the
 * frame capability, exactly as mapping into a VSpace needs the VSpace and the
 * frame.  The authority was spent when the device was named; after that the
 * question is the ordinary one about two objects the caller holds.
 */

#include "syscall_priv.h"
#include <iris/nc/kiospace.h>
#include <iris/nc/kiopagetable.h>
#include <iris/nc/kframe.h>
#include <iris/iommu.h>

/* Resolve one of the two new types.  Written out rather than shared with a
 * type parameter, because each call site knows exactly which type it wants and
 * a resolver that takes the type as an argument is one where passing the wrong
 * one compiles. */
static iris_error_t resolve_iospace(struct task *t, uint64_t cptr,
                                    iris_rights_t required,
                                    struct KIOSpace **out) {
    struct KObject *obj; iris_rights_t r;
    iris_error_t err = cspace_resolve_cap(t->cspace_root, (iris_cptr_t)cptr,
                                          RIGHT_NONE, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_IOSPACE) {
        kobject_active_release(obj); kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_active_release(obj); kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out = (struct KIOSpace *)obj;
    return IRIS_OK;
}

static iris_error_t resolve_iopt(struct task *t, uint64_t cptr,
                                 iris_rights_t required,
                                 struct KIOPageTable **out) {
    struct KObject *obj; iris_rights_t r;
    iris_error_t err = cspace_resolve_cap(t->cspace_root, (iris_cptr_t)cptr,
                                          RIGHT_NONE, &obj, &r);
    if (err != IRIS_OK) return err;
    if (obj->type != KOBJ_IO_PAGE_TABLE) {
        kobject_active_release(obj); kobject_release(obj);
        return IRIS_ERR_WRONG_TYPE;
    }
    if (required != RIGHT_NONE && !rights_check(r, required)) {
        kobject_active_release(obj); kobject_release(obj);
        return IRIS_ERR_ACCESS_DENIED;
    }
    *out = (struct KIOPageTable *)obj;
    return IRIS_OK;
}

static void release(struct KObject *o) {
    kobject_active_release(o);
    kobject_release(o);
}

/*
 * IOSpace_Bind(iospace, auth_cptr, source_id)
 *
 * `source_id` is the PCI bus:device:function the device puts on the bus with
 * every DMA request.  The kernel does not discover it and never will — that is
 * ring 3's to find, and passing it here is what keeps a bus scanner out of the
 * kernel.
 */
uint64_t sys_iospace_bind(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!syscall_has_bootcap(t, arg1, IRIS_BOOTCAP_IOSPACE_CONTROL))
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    if (arg2 > 0xFFFFu) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KIOSpace *io;
    iris_error_t err = resolve_iospace(t, arg0, RIGHT_WRITE, &io);
    if (err != IRIS_OK) return syscall_err(err);

    err = kiospace_bind(io, (uint16_t)arg2);
    release(&io->base);
    return err == IRIS_OK ? syscall_ok_u64(0) : syscall_err(err);
}

/*
 * IOSpace_MapTable(iospace, iopt_cptr, dma_addr)
 *
 * The FIRST call installs the top level and is what stops the device being
 * blocked: it claims a translation domain and writes the context entry.  Later
 * calls install whatever level the walk is missing, which the caller discovers
 * the same way it does for a CPU page table — by being told MISSING_TABLE.
 */
uint64_t sys_iospace_map_table(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KIOSpace *io;
    iris_error_t err = resolve_iospace(t, arg0, RIGHT_WRITE, &io);
    if (err != IRIS_OK) return syscall_err(err);

    struct KIOPageTable *pt;
    err = resolve_iopt(t, arg1, RIGHT_WRITE, &pt);
    if (err != IRIS_OK) { release(&io->base); return syscall_err(err); }

    err = kiospace_map_table(io, pt, arg2);
    if (err == IRIS_OK) {
        /* The space holds the level now; this reference is what keeps it alive
         * while it is part of a live walk, and `kiospace_obj_close` is what
         * drops it again. */
        kobject_retain(&pt->base);
    }
    release(&pt->base);
    release(&io->base);
    return err == IRIS_OK ? syscall_ok_u64(0) : syscall_err(err);
}

/*
 * IOSpace_MapFrame(iospace, frame_cptr, dma_addr, rights)
 *
 * The operation the stage exists for.  After it the device may reach that
 * frame and nothing else that was not mapped the same way.
 */
uint64_t sys_iospace_map_frame(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KIOSpace *io;
    iris_error_t err = resolve_iospace(t, arg0, RIGHT_WRITE, &io);
    if (err != IRIS_OK) return syscall_err(err);

    struct KFrame *fr; iris_rights_t fr_rights;
    err = cspace_resolve_only_frame(t->cspace_root, (iris_cptr_t)arg1,
                                    RIGHT_READ, &fr, &fr_rights);
    if (err != IRIS_OK) { release(&io->base); return syscall_err(err); }

    /*
     * The device gets what the caller asked for, narrowed by what the caller
     * HOLDS.  A holder that could hand a device more than its own frame
     * capability carries would be laundering authority through hardware — the
     * one path in the system where the grantee cannot be asked what it holds.
     */
    iris_rights_t want = (iris_rights_t)(arg3 & (RIGHT_READ | RIGHT_WRITE));
    iris_rights_t give = (iris_rights_t)(want & fr_rights);

    err = kiospace_map_frame(io, fr, arg2, give);
    if (err == IRIS_OK) {
        /* The device's mapping is a reference to the frame: while it stands,
         * the frame cannot be destroyed out from under a bus master. */
        kobject_retain(&fr->base);
    }
    release(&fr->base);
    release(&io->base);
    return err == IRIS_OK ? syscall_ok_u64(0) : syscall_err(err);
}

/*
 * IOSpace_Unmap(iospace, dma_addr)
 *
 * The device stops reaching that address, and the unit's translation cache is
 * told.  Both, in that order — see kiospace.c.
 */
uint64_t sys_iospace_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KIOSpace *io;
    iris_error_t err = resolve_iospace(t, arg0, RIGHT_WRITE, &io);
    if (err != IRIS_OK) return syscall_err(err);

    struct KFrame *fr = 0;
    err = kiospace_unmap(io, arg1, &fr);
    if (err == IRIS_OK && fr) kobject_release(&fr->base);
    release(&io->base);
    return err == IRIS_OK ? syscall_ok_u64(0) : syscall_err(err);
}
