#include "syscall_priv.h"
#include <iris/nc/kasidpool.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/fbcon.h>
#include <stddef.h>



/*
 * sys_vspace_self — RETIRED (ledger D-6 / charter A5).
 *
 * It handed the caller a capability to its own address space, asking for NO
 * capability at all: ambient authority, which seL4 does not have.  It also
 * published an MDB LEGACY ROOT — a capability with no ancestor, which no
 * revoke can reach.
 *
 * A thread is given its address space by whoever configured it.  Every service
 * receives one at IRIS_CPTR_OWN_VSPACE before its first instruction, and the ROOT TASK
 * finds its own in BootInfo — which is exactly seL4's arrangement:
 * seL4_CapInitThreadVSpace and seL4_CapInitThreadCNode are BootInfo slots,
 * not syscalls.  The number stays permanently reserved.
 */


/*
 * SYS_PROCESS_VSPACE — hand a RIGHT_MANAGE holder a capability to the target
 * process's address space (Phase 25, user-pager groundwork).
 *
 * Authority: RIGHT_MANAGE over the process cap — the same authority that
 * already implies address-space control via SYS_VMO_MAP_INTO.  The returned
 * cap is the delegable/attenuable form of that control: a supervisor mints it
 * (typically down to RIGHT_WRITE) into a pager's CSpace so the pager can
 * drive SYS_FRAME_MAP/SYS_FRAME_UNMAP on the target WITHOUT holding process
 * MANAGE-for-mapping — fault-info (READ) + resume (MANAGE) stay separately
 * scoped on the process cap.  Rights mirror SYS_VSPACE_SELF (no TRANSFER).
 */
/*
 * SYS_VSPACE_MAP_TABLE(pt_cptr, vspace_cptr, vaddr) — seL4_X86_PageTable_Map.
 *
 * Stage 6-pure Step 1.  The holder retyped a paging level out of its own
 * Untyped; this puts it into an address space.  What the kernel contributes is
 * the walk — which level is missing for this address — because that is a fact
 * about the address space, not a choice the holder gets to make.  What it no
 * longer contributes is the memory, or the decision that the table should
 * exist at all.
 *
 * Both capabilities are named and both need RIGHT_WRITE: installing a table
 * changes what the address space can map, and consumes the table.
 */
uint64_t sys_vspace_map_table(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *pt_obj;  iris_rights_t pt_rights;
    iris_error_t err = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                            RIGHT_NONE, KOBJ_PAGE_TABLE, &pt_obj, &pt_rights);
    if (err != IRIS_OK)
        return syscall_err(err);
    if (!rights_check(pt_rights, RIGHT_WRITE)) {
        kobject_release(pt_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* The SAME resolver SYS_VMO_MAP_PAGE and SYS_FRAME_MAP use for their
     * VSpace argument, with the same required right.  A capability that can
     * install a PTE must be able to install the level that PTE hangs off, or a
     * pager holding exactly what it needs to map would be unable to supply
     * what mapping now requires. */
    struct KVSpace *vs;  iris_rights_t vs_rights;
    err = cspace_resolve_only_vspace(t->cspace_root, (iris_cptr_t)arg1,
                                     RIGHT_WRITE, &vs, &vs_rights);
    if (err != IRIS_OK) {
        kobject_release(pt_obj);
        return syscall_err(err);
    }

    err = kvspace_map_table(vs, (struct KPageTable *)pt_obj, arg2);
    kobject_active_release(&vs->base);
    kobject_release(&vs->base);
    kobject_release(pt_obj);
    if (err != IRIS_OK) return syscall_err(err);
    return syscall_ok_u64(0);
}

/*
 * sys_vspace_unmap_table(pt_cptr, vspace_cptr) — seL4's
 * `seL4_X86_PageTable_Unmap`, and the counterpart `PageTable_Map` shipped
 * without.
 *
 * A level went into a walk and came out only when the whole address space
 * died.  So a holder that wanted to rearrange its own address space — or
 * reclaim a table it had installed for a mapping it then abandoned — had to
 * destroy the VSpace to do it, which is not reclamation.
 *
 * Both capabilities, both RIGHT_WRITE, exactly as the map requires: taking a
 * level out changes what the address space can map just as much as putting one
 * in.  The VSpace is named rather than read out of the table's `mapped_vs`,
 * for the reason every invocation in this kernel names what it acts on — a
 * holder that may edit a table must also hold the address space it is editing,
 * or a table capability alone would let one process reach into another's walk.
 *
 * BUSY while anything is mapped under the level (see kvspace_unmap_table);
 * NOT_FOUND when the table is not installed in the VSpace named.
 */
uint64_t sys_vspace_unmap_table(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *pt_obj;  iris_rights_t pt_rights;
    iris_error_t err = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                            RIGHT_NONE, KOBJ_PAGE_TABLE, &pt_obj, &pt_rights);
    if (err != IRIS_OK)
        return syscall_err(err);
    if (!rights_check(pt_rights, RIGHT_WRITE)) {
        kobject_release(pt_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KVSpace *vs;  iris_rights_t vs_rights;
    err = cspace_resolve_only_vspace(t->cspace_root, (iris_cptr_t)arg1,
                                     RIGHT_WRITE, &vs, &vs_rights);
    if (err != IRIS_OK) {
        kobject_release(pt_obj);
        return syscall_err(err);
    }

    err = kvspace_unmap_table(vs, (struct KPageTable *)pt_obj);
    kobject_active_release(&vs->base);
    kobject_release(&vs->base);
    kobject_release(pt_obj);
    if (err != IRIS_OK) return syscall_err(err);
    return syscall_ok_u64(0);
}

/* ── VMO syscalls ─────────────────────────────────────────────────── */











/*
 * sys_frame_get_address(frame_cptr) → physical base address, or iris_error_t
 *
 * seL4's `seL4_X86_Page_GetAddress`, and the reason it exists there is the
 * reason it exists here: a holder that has to program a device needs the
 * PHYSICAL address of the memory it is pointing that device at, and nothing
 * else in the system can tell it.  A DMA descriptor takes a physical address;
 * a page table the holder builds for a device takes physical addresses; the
 * framebuffer's stride calculations take one.  Without this, a ring-3 driver
 * has to be handed its physical address out of band by whoever retyped the
 * frame — which is a fact travelling outside the capability that carries the
 * authority, and the whole point of the model is that those do not separate.
 *
 * RIGHT_READ, and that is the whole check: learning where a frame IS confers
 * nothing over it.  A holder that may not read the frame may not ask either,
 * because "which physical page is this" is exactly the question that turns an
 * opaque capability into an address somebody can correlate.
 *
 * IRIS answers the address only; seL4's version reports it alone too.  The
 * SIZE is `Frame_Size`, because a frame here can be many pages (D-10) and the
 * two are different questions.
 */
uint64_t sys_frame_get_address(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_FRAME, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    uint64_t pa = ((struct KFrame *)obj)->paddr;
    kobject_release(obj);
    return syscall_ok_u64(pa);
}

/*
 * sys_frame_size(frame_cptr) → uint64_t byte size or iris_error_t
 */
uint64_t sys_frame_size(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_FRAME, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    uint64_t size = ((struct KFrame *)obj)->size;
    kobject_release(obj);
    return syscall_ok_u64(size);
}


/* ── Initrd/spawn syscalls: retired Phase 29 ─────────────────────── */

/* SYS_INITRD_LOOKUP(41) and SYS_SPAWN_ELF(42) are permanently retired.
 * Ring-3 loaders use SYS_INITRD_VMO(55) + SYS_PROCESS_CREATE(56) +
 * SYS_VMO_MAP_INTO(57) + SYS_THREAD_START(58) + SYS_HANDLE_INSERT(59). */

/* ── Phase 29 composable spawn primitives ────────────────────────── */

/*
 * sys_initrd_vmo is DELETED (Stage 6, ledger D-5).
 *
 * It handed a boot image over as a KVMO — one of the three object types seL4
 * has no equivalent for — which meant the loader and vfs both had to speak a
 * second memory ABI to read a file the kernel already had, and had to ask a
 * separate syscall how big it was.  `SYS_INITRD_FRAME` hands over a FRAME and
 * answers the size, and one map covers it (D-10).
 *
 * The NUMBER stays reserved; the code does not, because a second way to obtain
 * a boot image that nothing tests is worse than none.
 */


/*
 * SYS_INITRD_FRAME — a boot image, as a frame.
 *
 * See the ABI note in syscall.h for why this replaced `SYS_INITRD_VMO`.  The
 * shape is the same as a frame RETYPE and deliberately so: a contiguous
 * page-aligned region from the bottom of the budget, its header from the top,
 * the image copied in and the tail zeroed.  What makes it not a retype is that
 * the CONTENT comes from the kernel — it is the one thing here only the kernel
 * can see — and the memory it lands in is the caller's.
 */
uint64_t sys_initrd_frame(uint64_t arg0, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t dest      = arg2;
    uint64_t pool_cptr = arg3;
    if (dest == 0u || pool_cptr == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *auth_obj; iris_rights_t auth_rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                             RIGHT_NONE, KOBJ_BOOTSTRAP_CAP,
                                             &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    int ok = kbootcap_is((struct KBootstrapCap *)auth_obj,
                         IRIS_BOOTCAP_INITRD_CONTROL);
    kobject_release(auth_obj);
    if (!ok) return syscall_err(IRIS_ERR_ACCESS_DENIED);

    const void *elf_data = 0;
    uint32_t    elf_size = 0;
    if (!initrd_get((uint32_t)arg1, &elf_data, &elf_size))
        return syscall_err(IRIS_ERR_NOT_FOUND);
    if (elf_size == 0u) return syscall_err(IRIS_ERR_NOT_FOUND);

    struct KUntyped *pool = 0;
    {
        iris_rights_t nr;
        iris_error_t ne = cspace_resolve_only_untyped(t->cspace_root,
                              (iris_cptr_t)pool_cptr, RIGHT_WRITE, &pool, &nr);
        if (ne != IRIS_OK)
            return syscall_err(ne);
    }

    uint64_t bytes = ((uint64_t)elf_size + 0xFFFu) & ~0xFFFULL;
    void *hdr = kuntyped_alloc_child_top(pool, sizeof(struct KFrame));
    uint64_t phys = hdr ? kuntyped_bump_alloc_phys_page(pool, bytes) : 0;
    struct KFrame *frm = 0;
    if (phys) frm = kframe_alloc_at(hdr, phys, bytes);
    if (!frm) {
        if (hdr) kuntyped_release_child(hdr, sizeof(struct KFrame));
        kobject_active_release(&pool->base); kobject_release(&pool->base);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }
    kobject_active_release(&pool->base); kobject_release(&pool->base);

    /* Copy through the kernel's own window, and zero the tail: a frame handed
     * to ring 3 must not carry whatever the region held before. */
    {
        uint8_t       *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        const uint8_t *src = (const uint8_t *)elf_data;
        for (uint64_t i = 0; i < elf_size; i++)      dst[i] = src[i];
        for (uint64_t i = elf_size; i < bytes; i++)  dst[i] = 0;
    }

    struct KCNode *auth_cn = 0; uint32_t auth_idx = 0;
    if (cspace_value_is_cptr((iris_cptr_t)arg0) &&
        cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                            &auth_cn, &auth_idx) != IRIS_OK)
        auth_cn = 0;
    iris_error_t pe = syscall_publish_slot(t, &frm->base, RIGHT_READ,
                                           dest, auth_cn, auth_idx);
    if (auth_cn) {
        kobject_active_release(&auth_cn->base);
        kobject_release(&auth_cn->base);
    }
    if (pe != IRIS_OK) return syscall_err(pe);
    return syscall_ok_u64((uint64_t)elf_size);
}


/*
 * sys_initrd_count(auth_h) → uint32_t count or iris_error_t
 */
uint64_t sys_initrd_count(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject   *auth_obj;
    iris_rights_t     auth_rights;
    /* Stage 5 Step 2: the authority is the INITRD capability — reading boot
     * images, and nothing else.  vfs holds it and no longer carries the
     * authority to create processes as a side effect. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_is((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_INITRD_CONTROL)) {
        kobject_release(auth_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth_obj);
    return syscall_ok_u64((uint64_t)initrd_count());
}








/*
 * sys_framebuffer_vmo is DELETED (Stage 6), not merely undispatched.
 *
 * It wrapped the framebuffer's physical range in a KVMO — the only caller of
 * `kvmo_wrap`, and the last place the kernel fabricated a memory object over
 * MMIO.  The region is a DEVICE Untyped now (ledger D-9) and `fb` retypes its
 * own frame from it, so the code has no callers and no future: leaving it
 * compiled would leave a second way to reach the framebuffer that nothing
 * tests and nothing revokes.  The NUMBER stays reserved (syscall.h).
 */

/*
 * SYS_FRAMEBUFFER_INFO — the geometry, and only the geometry.
 *
 * `SYS_FRAMEBUFFER_VMO` answered this and fabricated a KVMO in the same call,
 * so the only way to learn where the framebuffer was and how wide it is was to
 * accept a kernel-made object over it.  Since the region became a DEVICE
 * Untyped (D-9) a driver retypes its own frame, and the two halves separate
 * cleanly: the geometry is a fact about hardware that boot discovered, and a
 * capability is not.
 *
 * Idempotent, unlike its predecessor — that one cleared `valid` so the region
 * could be claimed once, which was a one-shot GRANT wearing a query's clothes.
 * The grant now lives where grants live: whoever holds the device Untyped.
 */
uint64_t sys_framebuffer_info(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *auth_obj;
    iris_rights_t   auth_rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                             RIGHT_NONE, KOBJ_BOOTSTRAP_CAP,
                                             &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    int ok = kbootcap_is((struct KBootstrapCap *)auth_obj,
                         IRIS_BOOTCAP_FB_CONTROL);
    kobject_release(auth_obj);
    if (!ok) return syscall_err(IRIS_ERR_ACCESS_DENIED);

    if (g_iris_fb_params.size == 0u) return syscall_err(IRIS_ERR_NOT_FOUND);
    if (!copy_to_user_checked(arg1, &g_iris_fb_params,
                              (uint32_t)sizeof(g_iris_fb_params)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    /*
     * The kernel stops painting here.
     *
     * `fbcon` writes the boot log to the screen because most machines have no
     * serial port, and two writers on one screen produce a screen that
     * describes neither.  This is the honest place to hand it over: the call
     * is a query, but the only task that can make it is the one holding
     * FB_CONTROL, and the only reason to ask where the screen is and how wide
     * it is, is to paint on it.
     *
     * It is not a perfect signal and it does not need to be.  Getting it wrong
     * in one direction costs a kernel log that stops early on a screen nobody
     * was going to read anyway; panic reclaims the screen regardless.
     */
    fbcon_yield();
    return syscall_ok_u64(0);
}

/*
 * sys_asid_pool_assign(pool_cptr, vspace_cptr) — ledger A-21.
 *
 * seL4's `seL4_X86_ASIDPool_Assign`.  An address space is a page and a header
 * until somebody who holds a pool gives it a hardware identifier; a thread
 * cannot be configured with one that has none.  Splitting this from retyping
 * the VSpace is the point: holding memory lets you BUILD an address space,
 * holding a pool lets you make one RUNNABLE, and they are different grants.
 */
uint64_t sys_asid_pool_assign(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *pool_obj; iris_rights_t pr;
    iris_error_t err = cspace_resolve_only_obj(t->cspace_root,
                                               (iris_cptr_t)arg0, RIGHT_WRITE,
                                               KOBJ_ASID_POOL, &pool_obj, &pr);
    if (err != IRIS_OK) return syscall_err(err);

    struct KVSpace *vs; iris_rights_t vr;
    err = cspace_resolve_only_vspace(t->cspace_root, (iris_cptr_t)arg1,
                                     RIGHT_WRITE, &vs, &vr);
    if (err != IRIS_OK) { kobject_release(pool_obj); return syscall_err(err); }

    err = kvspace_assign_asid(vs, (struct KAsidPool *)pool_obj);

    kobject_active_release(&vs->base);
    kobject_release(&vs->base);
    kobject_release(pool_obj);
    return (err == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(err);
}
