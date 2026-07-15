/*
 * syscall_frame.c — Fase 5 / 5.1: SYS_FRAME_MAP and SYS_FRAME_UNMAP.
 *
 * SYS_FRAME_MAP(frame_cptr, vspace_cptr, user_va, flags):
 *   Resolves Frame and VSpace capabilities, validates flags and VA, then
 *   delegates to kframe_map_page() which installs the PTE and increments
 *   frame->mapped_count.
 *
 * SYS_FRAME_UNMAP(frame_cptr, vspace_cptr, user_va):
 *   Resolves Frame and VSpace capabilities, then delegates to
 *   kframe_unmap_page() which removes the PTE, issues invlpg, and
 *   decrements frame->mapped_count.
 *
 * Authority model:
 *   Frame: RIGHT_READ always; RIGHT_WRITE additionally for writable maps.
 *   VSpace: RIGHT_WRITE to modify the page tables.
 *   ACCESS_DENIED from CSpace resolution is a hard stop (no handle fallback).
 *
 * Fase 25: the VSpace argument resolves through the dual resolver (CPtr or
 * handle), the same A1 migration every other capability argument already
 * made.  Before, a handle fed here went through the raw radix walk and was
 * masked into low root slots (the Fase 8 aliasing hazard class); now the
 * handle namespace resolves honestly, so a supervisor/pager can pass a
 * SYS_PROCESS_VSPACE handle directly.
 *
 * Lifecycle invariant (Fase 5.1):
 *   frame->mapped_count must be 0 before dropping the last frame cap.
 *   kframe_obj_destroy() panics if mapped_count > 0 at destruction time.
 *   This eliminates silent stale PTEs that Fase 5 allowed.
 *
 * TLB (single-core):
 *   Map: no flush needed (new PTE; no stale entry).
 *   Unmap: invlpg issued inside paging_unmap_in().
 *   SMP TLB shootdown deferred to Fase 6.
 */
#include "syscall_priv.h"
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>

uint64_t sys_frame_map(uint64_t arg0, uint64_t arg1,
                       uint64_t arg2, uint64_t arg3)
{
    iris_cptr_t frame_cptr  = (iris_cptr_t)arg0;
    iris_cptr_t vspace_cptr = (iris_cptr_t)arg1;
    uint64_t    user_va     = arg2;
    uint64_t    map_flags   = arg3;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Fast-fail: validate flags and VA before cap resolution. */
    if (map_flags & ~3ULL) return syscall_err(IRIS_ERR_INVALID_ARG);
    if ((map_flags & 1u) && (map_flags & 2u)) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!kframe_va_valid(user_va)) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Frame cap: RIGHT_READ always; RIGHT_WRITE if writable mapping requested. */
    iris_rights_t frame_required = RIGHT_READ;
    if (map_flags & 1u) frame_required |= RIGHT_WRITE;

    struct KFrame   *frame;
    iris_rights_t    frame_rights;
    iris_error_t err = cspace_or_handle_resolve_frame(t->process, frame_cptr,
                                                       frame_required,
                                                       &frame, &frame_rights);
    if (err != IRIS_OK) return syscall_err(err);

    /* VSpace cap: RIGHT_WRITE to install PTE. */
    struct KVSpace  *vs;
    iris_rights_t    vs_rights;
    err = cspace_or_handle_resolve_vspace(t->process, vspace_cptr, RIGHT_WRITE,
                                          &vs, &vs_rights);
    if (err != IRIS_OK) {
        kobject_active_release(&frame->base);
        kobject_release(&frame->base);
        return syscall_err(err);
    }

    /* Core map: validates VSpace liveness, duplicate check, PTE install,
     * and increments frame->mapped_count. */
    err = kframe_map_page(frame, vs, user_va, map_flags);

    kobject_active_release(&frame->base);
    kobject_release(&frame->base);
    kobject_active_release(&vs->base);
    kobject_release(&vs->base);

    return (err == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(err);
}

uint64_t sys_frame_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    iris_cptr_t frame_cptr  = (iris_cptr_t)arg0;
    iris_cptr_t vspace_cptr = (iris_cptr_t)arg1;
    uint64_t    user_va     = arg2;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Fast-fail VA check before cap resolution. */
    if (!kframe_va_valid(user_va)) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Frame cap: RIGHT_READ sufficient for unmap (caller holds the mapping). */
    struct KFrame   *frame;
    iris_rights_t    frame_rights;
    iris_error_t err = cspace_or_handle_resolve_frame(t->process, frame_cptr,
                                                       RIGHT_READ,
                                                       &frame, &frame_rights);
    if (err != IRIS_OK) return syscall_err(err);

    /* VSpace cap: RIGHT_WRITE to modify page tables. */
    struct KVSpace  *vs;
    iris_rights_t    vs_rights;
    err = cspace_or_handle_resolve_vspace(t->process, vspace_cptr, RIGHT_WRITE,
                                          &vs, &vs_rights);
    if (err != IRIS_OK) {
        kobject_active_release(&frame->base);
        kobject_release(&frame->base);
        return syscall_err(err);
    }

    /* Core unmap: validates VSpace liveness, verifies VA belongs to this
     * frame, removes PTE, issues invlpg, decrements frame->mapped_count. */
    err = kframe_unmap_page(frame, vs, user_va);

    kobject_active_release(&frame->base);
    kobject_release(&frame->base);
    kobject_active_release(&vs->base);
    kobject_release(&vs->base);

    return (err == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(err);
}
