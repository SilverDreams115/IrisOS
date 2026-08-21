#include "syscall_priv.h"
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kvmo.h>
#include <stddef.h>



/*
 * SYS_VSPACE_SELF — hand the caller a capability to its own address space.
 *
 * Phase 19: self-authority only.  A process already fully controls its own
 * address space through the VMO map/unmap syscalls, so a cap to its own VSpace
 * is not new authority — it exists so ring-3 code can mint the cap into a
 * CSpace slot and exercise SYS_FRAME_MAP / SYS_FRAME_UNMAP on itself by CPtr
 * (the resolvers for those syscalls require a VSpace CPtr).  No argument names
 * another VSpace; there is no cross-process reach here.
 */
uint64_t sys_vspace_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->vspace)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KVSpace *vs = t->vspace;
    const iris_rights_t rights = RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE;

    /* Stage 4: arg0 is a destination slot (RETYPE2 packing).  The VSpace is
     * borrowed from the process, so publish_slot's consuming release needs a
     * reference of its own.  A LEGACY_ROOT: the caller's own address space is
     * an attribute of being a process, not something another slot granted. */
    /* Stage 4: a destination slot is REQUIRED.  The handle result is retired,
     * so arg0 == 0 names no destination and is an error rather than a silent
     * fall back to the other namespace. */
    if (arg0 == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    kobject_retain(&vs->base);
    iris_error_t pe = syscall_publish_slot(t, &vs->base, rights, arg0, 0, 0);
    if (pe != IRIS_OK) return syscall_err(pe);
    return syscall_ok_u64(0);
}


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
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *pt_obj;  iris_rights_t pt_rights;
    iris_error_t err = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                            RIGHT_NONE, KOBJ_PAGE_TABLE, &pt_obj, &pt_rights);
    if (err != IRIS_OK)
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
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
        return syscall_err(err == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : err);
    }

    err = kvspace_map_table(vs, (struct KPageTable *)pt_obj, arg2);
    kobject_active_release(&vs->base);
    kobject_release(&vs->base);
    kobject_release(pt_obj);
    if (err != IRIS_OK) return syscall_err(err);
    return syscall_ok_u64(0);
}

/*
 * SYS_PROCESS_VSPACE — RETIRED (Stage 7 Step 15).
 *
 * It answered "give me that process's address space", and the kernel answered
 * by reading `child->vspace` out of a KProcess — a supervisor reaching an
 * object it did not hold by naming a different one.  The same shape Step 9
 * removed for CSpaces, one object over.
 *
 * The spawner already has it.  Since Stage 6-pure Step 4 the loader RETYPES
 * the child's VSpace and holds it through the entire spawn; it just threw the
 * capability away at the end, which is what made this syscall necessary.  It
 * hands it over now (`svc_load_minted_ws`'s `keep_vspace_dest`), opt-in
 * because keeping one has a cost: a VSpace capability keeps that address space
 * and every page table in it alive past the child's death, blocking the RESET
 * of the budget they are charged to.
 *
 * The self case was documented as "equivalent to SYS_VSPACE_SELF" for as long
 * as it existed.  That is the syscall to call.
 */
uint64_t sys_process_vspace(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/* ── VMO syscalls ─────────────────────────────────────────────────── */

/*
 * vmo_create_charged — shared body for SYS_VMO_CREATE / SYS_VMO_CREATE_FOR.
 * Creates a sparse VMO of `size`, charges the VMO OBJECT quota (owned_vmos) and,
 * later, its sparse pages to `payer` (the owner/payer domain), and installs the
 * handle in the CALLER's table (the holder).  Phase 29: owner (payer) and holder
 * are deliberately distinct — a loader creating a child's image VMO charges the
 * CHILD but keeps the handle to map/close it.
 */
static uint64_t vmo_create_charged(struct task *t, uint64_t size,
                                    uint64_t dest, struct KUntyped *named_pool) {
    uint32_t pages = 0;
    if (kvmo_size_to_pages(size, &pages) != IRIS_OK)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    (void)pages;
    /* Stage 6 Step 5: a VMO's memory comes from the budget its PAYER was
     * given.  Anonymous user memory was the last thing the kernel handed out
     * for free — bounded only by a per-process quota the kernel invented,
     * rather than by a capability someone delegated. */
    /* Stage 7 Step 14: named_pool is never NULL — the caller requires it.
     * Stage 7-mem: and there is no owner to bind.  A VMO used to be charged to
     * a PROCESS, against a ceiling of 32 the kernel invented; the budget it
     * was carved from is the accounting, and it is one somebody delegated. */
    struct KVmo *v = kvmo_create_from(size, named_pool);
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);
    /* Step 4: a destination slot publishes the VMO into CSpace instead of
     * producing a handle.  No MDB parent: a KVMO is fabricated from kernel
     * memory, not retyped from an Untyped, so it has no capability ancestor to
     * name — that is KVMO's own debt (ledger: FROZEN, memory-server), not this
     * step's.  It is an explicit LEGACY root, counted, exactly as the handle
     * form was untracked. */
    /* Stage 4: a destination slot is REQUIRED — the handle result is retired. */
    if (dest == 0u) {
        kvmo_free(v);   /* kvmo_destroy releases the owner charge — no leak */
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    iris_error_t pe = syscall_publish_slot(t, &v->base,
                                           RIGHT_READ | RIGHT_WRITE |
                                           RIGHT_TRANSFER | RIGHT_DUPLICATE,
                                           dest, 0, 0);
    if (pe != IRIS_OK) return syscall_err(pe);
    return syscall_ok_u64(0);
}

uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /*
     * Stage 6 Step 5: arg1 names WHICH Untyped pays for this VMO's pages.
     *
     * It was a dead argument.  A process asking for anonymous memory got PMM
     * pages bounded by a per-process quota the kernel invented; now the memory
     * comes from a budget, and which budget is the caller's to say — a process
     * holds several (the one it was spawned with, the pools its supervisor
     * delegated) and they are not interchangeable.
     *
     * Stage 7 Step 14: REQUIRED.  Zero used to mean "the budget my address
     * space was built from", read off the KProcess — the kernel picking whose
     * memory pays for a caller that did not say.
     */
    if (arg1 == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KUntyped *named = 0;
    {
        iris_rights_t nr;
        iris_error_t ne = cspace_resolve_only_untyped(t->cspace_root,
                              (iris_cptr_t)arg1, RIGHT_WRITE, &named, &nr);
        if (ne != IRIS_OK)
            return syscall_err(ne == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : ne);
    }
    uint64_t rc = vmo_create_charged(t, arg0, arg2, named);
    kobject_active_release(&named->base); kobject_release(&named->base);
    return rc;
}

/*
 * SYS_VMO_CREATE_FOR — RETIRED (Stage 7-mem).
 *
 * It named a PAYER: a process the caller held RIGHT_MANAGE on, which the VMO
 * object and its pages were charged to.  Phase 29 introduced it to fix real
 * caller-charged accounting — a loader's own quota grew with every child it
 * launched — and that was the right fix for a model where a process is a
 * resource domain with a ceiling.
 *
 * There is no ceiling any more.  Stage 7 Step 14 made the MEMORY come from a
 * budget the caller names and holds, and Stage 7-mem removed the per-process
 * VMO count that was the only thing the payer argument still selected.  What
 * is left of "who pays" is the Untyped, which SYS_VMO_CREATE already takes —
 * so the two syscalls had become the same call, one of them carrying an
 * argument that no longer meant anything.
 *
 * A loader that wants a child's image charged to the child carves it from the
 * child's budget, which it holds, and says so.
 */
uint64_t sys_vmo_create_for(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                            uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/*
 * rollback_vmo_maps — unmap pages in [start, end) from vs.
 *
 * Called on error paths in sys_vmo_map / sys_vmo_map_into to remove
 * KFrame-backed pages that were successfully installed before the failure.
 * kvspace_unmap_page handles PTE removal, mapped_count decrement, frame
 * release, and KFrameMapping node deallocation atomically.
 * Pages not found (e.g. never mapped) are silently skipped.
 */
static void rollback_vmo_maps(struct KVSpace *vs, uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE_SIZE)
        (void)kvspace_unmap_page(vs, va);
}


uint64_t sys_vmo_map(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->vspace || !t->vspace->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    /* A1 Increment 1: dual resolver — the VMO may be a CPtr slot or a handle.
     * RIGHT_NONE defers to the READ/WRITE checks below (unchanged). */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_VMO, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    struct KVSpace *vs = t->vspace;
    if (!vs) { kobject_release(obj); return syscall_err(IRIS_ERR_INVALID_ARG); }

    struct KVmo *v = (struct KVmo *)obj;
    uint64_t map_size;
    int writable   = (arg2 & 1) != 0;
    int executable = (arg2 & 2) != 0;
    if (writable && executable) { kobject_release(obj); return syscall_err(IRIS_ERR_INVALID_ARG); }

    /* map_flags for kframe_map_page: bit 0 = WRITABLE, bit 1 = EXEC */
    uint64_t map_flags = 0;
    if (writable)   map_flags |= 1u;
    if (executable) map_flags |= 2u;

    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (writable && !rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    if (v->size == 0 || !user_vmo_range_valid(arg1, v->size)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    if (!page_round_up_u64(v->size, &map_size)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }

    if (v->sparse) {
        /* Sparse VMO: allocate physical pages eagerly; map via KFrame. */

        /* Pre-check: no VA in the range must already have a PTE. */
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_virt_to_phys_in(t->vspace->cr3, arg1 + off) != 0) {
                kobject_release(obj);
                return syscall_err(IRIS_ERR_BUSY);
            }
        }

        /* Phase 29: sparse pages are charged to the VMO's OWNER (payer domain),
         * once, at allocation — not to whoever maps it first.  So a loader that
         * maps a child's segment VMO into its own window to fill it charges the
         * CHILD, and closing/unmapping never strands the charge on the loader
         * (released at kvmo_destroy).  Fallback to the caller if unbound. */
        uint64_t mapped_until = arg1;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            uint32_t page_idx = (uint32_t)(off >> 12);

            if (v->pages[page_idx] == 0) {
                uint64_t phys = kvmo_alloc_page(v);
                if (!phys) {
                    rollback_vmo_maps(vs, arg1, mapped_until);
                    kobject_release(obj);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint8_t *kva = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
                for (int k = 0; k < 4096; k++) kva[k] = 0;
                v->pages[page_idx] = phys;
            }

            /* Create a KFrame backed by this VMO page.  The KFrame retains
             * the VMO so that kvmo_destroy (and thus pmm_free_page) is deferred
             * until after all KFrames for this VMO's pages are released. */
            struct KFrame *f = kframe_alloc_vmo_page(v->pages[page_idx], v);
            if (!f) {
                rollback_vmo_maps(vs, arg1, mapped_until);
                kobject_release(obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            iris_error_t mr = kframe_map_page(f, vs, arg1 + off, map_flags);
            kobject_release(&f->base); /* drop alloc retain; mapping retain held by vs->mappings */
            if (mr != IRIS_OK) {
                rollback_vmo_maps(vs, arg1, mapped_until);
                kobject_release(obj);
                return syscall_err(mr);
            }
            mapped_until = arg1 + off + PAGE_SIZE;
        }

        kobject_release(obj);
        return syscall_ok_u64(0);
    }

    /* Wrap/MMIO VMO: map each physical page via KFrame (no PMM ownership). */
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(t->vspace->cr3, arg1 + off) != 0) {
            kobject_release(obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
    }

    {
        uint64_t mapped_until = arg1;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            /* MMIO pages are not PMM-owned; create a KFrame with no vmo_owner.
             * kframe_obj_destroy will call only kslab_free — no physical free. */
            struct KFrame *f = kframe_alloc(v->phys + off, 4096u, NULL);
            if (!f) {
                rollback_vmo_maps(vs, arg1, mapped_until);
                kobject_release(obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            iris_error_t mr = kframe_map_page(f, vs, arg1 + off, map_flags);
            kobject_release(&f->base);
            if (mr != IRIS_OK) {
                rollback_vmo_maps(vs, arg1, mapped_until);
                kobject_release(obj);
                return syscall_err(mr);
            }
            mapped_until = arg1 + off + PAGE_SIZE;
        }
    }
    kobject_release(obj);
    return syscall_ok_u64(0);
}


/*
 * sys_vmo_unmap(vaddr, size) → 0 or iris_error_t
 *
 * Removes [vaddr, vaddr+size) KFrame mappings from the caller's VSpace.
 * Each kvspace_unmap_page call removes the PTE, decrements mapped_count, and
 * releases the frame retain (which may trigger kframe_obj_destroy → release
 * of the VMO retain → kvmo_destroy if this was the last reference).
 *
 * Physical pages are NOT freed here — the KVmo still owns them; they are
 * released when the last handle to the VMO is closed.
 *
 * Pages not mapped in the VSpace (VA absent) are silently skipped.
 */
uint64_t sys_vmo_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->vspace || !t->vspace->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t vaddr = arg0;
    uint64_t size  = arg1;

    if (!user_vmo_range_valid(vaddr, size)) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KVSpace *vs = t->vspace;
    if (!vs) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t map_size = 0;
    if (!page_round_up_u64(size, &map_size)) return syscall_err(IRIS_ERR_OVERFLOW);

    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE)
        (void)kvspace_unmap_page(vs, vaddr + off);

    return syscall_ok_u64(0);
}


/*
 * sys_vmo_size(vmo_h) → uint64_t byte size or iris_error_t
 */
uint64_t sys_vmo_size(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    /* A1 Increment 1b: dual resolver — the VMO may be a CPtr slot or a handle. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_VMO, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    uint64_t size = ((struct KVmo *)obj)->size;
    kobject_release(obj);
    return syscall_ok_u64(size);
}


/* ── Initrd/spawn syscalls: retired Phase 29 ─────────────────────── */

/* SYS_INITRD_LOOKUP(41) and SYS_SPAWN_ELF(42) are permanently retired.
 * Ring-3 loaders use SYS_INITRD_VMO(55) + SYS_PROCESS_CREATE(56) +
 * SYS_VMO_MAP_INTO(57) + SYS_THREAD_START(58) + SYS_HANDLE_INSERT(59). */

/* ── Phase 29 composable spawn primitives ────────────────────────── */

/*
 * sys_initrd_vmo(auth_h, index) → vmo_handle or iris_error_t
 */
uint64_t sys_initrd_vmo(uint64_t arg0, uint64_t arg1,
                               uint64_t arg2, uint64_t arg3) {
    uint64_t dest      = arg2;   /* Step 4: cnode | slot<<32 */
    uint64_t pool_cptr = arg3;   /* Stage 6 Step 5: which budget pays */
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject   *auth_obj;
    iris_rights_t     auth_rights;
    /* Stage 5 Step 2: the authority is the INITRD capability — reading boot
     * images, and nothing else.  vfs holds it and no longer carries the
     * authority to create processes as a side effect. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_is((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_INITRD_CONTROL)) {
        kobject_release(auth_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth_obj);

    const void *elf_data = 0;
    uint32_t    elf_size = 0;
    if (!initrd_get((uint32_t)arg1, &elf_data, &elf_size))
        return syscall_err(IRIS_ERR_NOT_FOUND);

    /* Create a sparse VMO and copy ELF bytes into pre-populated pages.
     * initrd_get returns a kernel virtual address (identity-mapped) that is
     * NOT guaranteed to be page-aligned.  Rather than wrapping the raw
     * physical address (which paging_map_checked_in would align down, causing
     * a read offset bug), we copy into freshly-allocated page-aligned pages. */
    /*
     * Stage 6 Step 5: the boot image copy is charged to a budget.  Reading an
     * initrd entry allocates as many kernel pages as the image is long, and
     * that used to be free.
     *
     * `pool_cptr` says WHICH of the caller's budgets pays, because this
     * allocation is transient for a loader — it parses the image and drops it —
     * and a caller that points it at a scratch Untyped can RESET that region
     * between spawns instead of consuming its whole pool one image at a time.
     *
     * Stage 7 Step 14: REQUIRED.  Zero used to mean "charge my own budget",
     * read off the KProcess, which is the kernel picking whose memory pays for
     * a caller that did not say — the last of three such sites.  A caller that
     * wants its own budget names its own budget.
     */
    if (pool_cptr == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KUntyped *pool = 0;
    {
        iris_rights_t nr;
        iris_error_t ne = cspace_resolve_only_untyped(t->cspace_root,
                              (iris_cptr_t)pool_cptr, RIGHT_WRITE, &pool, &nr);
        if (ne != IRIS_OK)
            return syscall_err(ne == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : ne);
    }
    struct KVmo *v = kvmo_create_from((uint64_t)elf_size, pool);
    { kobject_active_release(&pool->base); kobject_release(&pool->base); }
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);

    {
        const uint8_t *src = (const uint8_t *)elf_data;
        uint32_t pg;
        for (pg = 0; pg < v->page_capacity; pg++) {
            uint64_t phys = kvmo_alloc_page(v);
            if (!phys) { kvmo_free(v); return syscall_err(IRIS_ERR_NO_MEMORY); }
            uint8_t *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
            uint64_t off = (uint64_t)pg * PAGE_SIZE;
            uint64_t cp  = elf_size - (uint32_t)off;
            if (cp > PAGE_SIZE) cp = PAGE_SIZE;
            for (uint64_t j = 0; j < cp; j++)  dst[j] = src[off + j];
            for (uint64_t j = cp; j < PAGE_SIZE; j++) dst[j] = 0;
            v->pages[pg] = phys;
        }
    }

    /* Step 4: arg2 names a destination CSpace slot.  With one, the image VMO
     * is published there as an MDB child of the spawn-cap slot that authorised
     * the read, so the loader never holds a handle and the grant is revocable
     * by whoever granted the spawn authority.  arg2 == 0 keeps the legacy
     * handle for callers not yet migrated. */
    /* Stage 4: a destination slot is REQUIRED — the handle result is retired. */
    if (dest == 0u) { kvmo_free(v); return syscall_err(IRIS_ERR_INVALID_ARG); }
    {
        struct KCNode *auth_cn = 0; uint32_t auth_idx = 0;
        if (cspace_value_is_cptr((iris_cptr_t)arg0) &&
            cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                                &auth_cn, &auth_idx) != IRIS_OK)
            auth_cn = 0;
        iris_error_t pe = syscall_publish_slot(t, &v->base, RIGHT_READ,
                                               dest, auth_cn, auth_idx);
        if (auth_cn) {
            kobject_active_release(&auth_cn->base);
            kobject_release(&auth_cn->base);
        }
        if (pe != IRIS_OK) return syscall_err(pe);
        return syscall_ok_u64(0);
    }
}


/*
 * sys_initrd_count(auth_h) → uint32_t count or iris_error_t
 */
uint64_t sys_initrd_count(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    (void)arg1; (void)arg2; (void)arg3;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject   *auth_obj;
    iris_rights_t     auth_rights;
    /* Stage 5 Step 2: the authority is the INITRD capability — reading boot
     * images, and nothing else.  vfs holds it and no longer carries the
     * authority to create processes as a side effect. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_is((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_INITRD_CONTROL)) {
        kobject_release(auth_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth_obj);
    return syscall_ok_u64((uint64_t)initrd_count());
}


/*
 * sys_vmo_map_into(vmo_h, proc_h, vaddr, flags) → 0 or iris_error_t
 *
 * Maps VMO pages into a target process's address space via KFrame capabilities.
 * Requires RIGHT_READ (+ RIGHT_WRITE if writable) on vmo_h and RIGHT_MANAGE
 * on proc_h.  W^X enforced.
 */
uint64_t sys_vmo_map_into(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *vmo_obj;
    iris_rights_t   vmo_rights;
    /* A1 Increment 1b: dual resolver on the VMO argument only — the target
     * process stays handle-only until the Process family migrates. */
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_VMO, &vmo_obj, &vmo_rights);
    if (r != IRIS_OK) return syscall_err(r);

    /*
     * Stage 7 Step 9: the target is the ADDRESS SPACE, named directly.
     *
     * It was a PROCESS with RIGHT_MANAGE, and the kernel then read
     * `proc->vspace` out of it — so a caller that already held the address
     * space it wanted to map into had to hold authority over the whole process
     * as well, and the process capability was doing nothing but carrying a
     * pointer to the thing actually being used.  A spawner HAS the VSpace: it
     * retyped it (Stage 6-pure Step 4) and handed it to SYS_PROCESS_CREATE.
     *
     * This is the shape SYS_VMO_MAP_PAGE and SYS_FRAME_MAP have had since
     * Phase 25/26; the whole-VMO form was the last one still asking for a
     * process.  RIGHT_WRITE on the address space is the authority to install a
     * PTE in it, which is exactly what this does.
     */
    struct KVSpace *target_vs;
    iris_rights_t   vs_rights;
    r = cspace_resolve_only_vspace(t->cspace_root, (iris_cptr_t)arg1,
                                   RIGHT_WRITE, &target_vs, &vs_rights);
    if (r != IRIS_OK) {
        kobject_release(vmo_obj);
        return syscall_err(r == IRIS_ERR_WRONG_TYPE ? IRIS_ERR_INVALID_ARG : r);
    }

    struct KVmo *v        = (struct KVmo *)vmo_obj;
    uint64_t     vaddr    = arg2;
    uint64_t     map_size = 0;

    int writable   = (arg3 & 1) != 0;
    int executable = (arg3 & 2) != 0;
    if (writable && executable) {
        kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    uint64_t map_flags = 0;
    if (writable)   map_flags |= 1u;
    if (executable) map_flags |= 2u;

    if (!rights_check(vmo_rights, RIGHT_READ)) {
        kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (writable && !rights_check(vmo_rights, RIGHT_WRITE)) {
        kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    if (v->size == 0) {
        kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    if (!page_round_up_u64(v->size, &map_size)) {
        kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }
    if (!user_private_range_valid(vaddr, v->size, USER_STACK_TOP)) {
        kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    if (v->sparse) {
        /* Pre-check: no VA in the range must already have a PTE. */
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_virt_to_phys_in(target_vs->cr3, vaddr + off) != 0) {
                kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
                return syscall_err(IRIS_ERR_BUSY);
            }
        }

        /* Phase 29: charge the VMO owner (payer domain), not the map target — a
         * shared VMO's pages are paid once by its owner; extra targets that map
         * it do not re-charge (Q6/Q7/Q18). */
        uint64_t mapped_until = vaddr;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            uint32_t page_idx = (uint32_t)(off >> 12);

            if (v->pages[page_idx] == 0) {
                uint64_t phys = kvmo_alloc_page(v);
                if (!phys) {
                    rollback_vmo_maps(target_vs, vaddr, mapped_until);
                    kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint8_t *kva = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
                for (int k = 0; k < 4096; k++) kva[k] = 0;
                v->pages[page_idx] = phys;
            }

            struct KFrame *f = kframe_alloc_vmo_page(v->pages[page_idx], v);
            if (!f) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            iris_error_t mr = kframe_map_page(f, target_vs, vaddr + off, map_flags);
            kobject_release(&f->base);
            if (mr != IRIS_OK) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
                return syscall_err(mr);
            }
            mapped_until = vaddr + off + PAGE_SIZE;
        }

        kobject_release(vmo_obj);
        kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
        return syscall_ok_u64(0);
    }

    /* Wrap/MMIO VMO path */
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(target_vs->cr3, vaddr + off) != 0) {
            kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
            return syscall_err(IRIS_ERR_BUSY);
        }
    }
    {
        uint64_t mapped_until = vaddr;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            struct KFrame *f = kframe_alloc(v->phys + off, 4096u, NULL);
            if (!f) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            iris_error_t mr = kframe_map_page(f, target_vs, vaddr + off, map_flags);
            kobject_release(&f->base);
            if (mr != IRIS_OK) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
                return syscall_err(mr);
            }
            mapped_until = vaddr + off + PAGE_SIZE;
        }
    }
    kobject_release(vmo_obj);
    kobject_active_release(&target_vs->base); kobject_release(&target_vs->base);
    return syscall_ok_u64(0);
}


/*
 * sys_vmo_map_page(vmo_cptr, vspace_cptr, target_va, offset_flags) — Phase 26.
 *
 * Page-granular, offset-addressed map of ONE VMO page into a VSpace named by
 * capability.  The authority is (VMO READ[/WRITE]) + (VSpace WRITE) — NO
 * process MANAGE: the VSpace cap already IS the map-into-target authority
 * (SYS_PROCESS_VSPACE, Phase 25).  This is the VMO-backed analogue of
 * SYS_FRAME_MAP; it does not touch the whole-VMO contiguous map path.
 */
uint64_t sys_vmo_map_page(uint64_t arg0, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3) {
    iris_cptr_t vmo_cptr    = (iris_cptr_t)arg0;
    iris_cptr_t vspace_cptr = (iris_cptr_t)arg1;
    uint64_t    target_va   = arg2;
    uint64_t    offset_flags = arg3;

    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Fast-fail decode before any cap resolution. */
    uint64_t map_flags = offset_flags & 0x3ULL;
    if (offset_flags & 0xFFCULL) return syscall_err(IRIS_ERR_INVALID_ARG);  /* reserved [11:2] */
    if ((map_flags & 1u) && (map_flags & 2u)) return syscall_err(IRIS_ERR_INVALID_ARG); /* W^X */
    uint64_t offset = offset_flags & ~0xFFFULL;
    if (!kframe_va_valid(target_va)) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* VMO cap: RIGHT_READ always, RIGHT_WRITE if a writable PTE is requested. */
    iris_rights_t vmo_required = RIGHT_READ;
    if (map_flags & 1u) vmo_required |= RIGHT_WRITE;

    struct KObject *vmo_obj;
    iris_rights_t   vmo_rights;
    iris_error_t err = cspace_resolve_only_obj(t->cspace_root, vmo_cptr,
                                 RIGHT_NONE, KOBJ_VMO, &vmo_obj, &vmo_rights);
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(vmo_rights, vmo_required)) {
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* VSpace cap: RIGHT_WRITE to install the PTE (dual resolver, Phase 25). */
    struct KVSpace *vs;
    iris_rights_t   vs_rights;
    err = cspace_resolve_only_vspace(t->cspace_root, vspace_cptr, RIGHT_WRITE,
                                          &vs, &vs_rights);
    if (err != IRIS_OK) { kobject_release(vmo_obj); return syscall_err(err); }

    struct KVmo *v = (struct KVmo *)vmo_obj;

    /* Offset must address a page fully within the VMO. */
    uint64_t map_size = 0;
    if (v->size == 0 || !page_round_up_u64(v->size, &map_size)) {
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    if (offset >= map_size) {
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    if (v->sparse) {
        uint32_t page_idx = (uint32_t)(offset >> 12);
        if (page_idx >= v->page_capacity) {
            kobject_active_release(&vs->base); kobject_release(&vs->base);
            kobject_release(vmo_obj);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
        /* Phase 29: charge the VMO owner (payer domain), not the mapper.  The
         * pager maps its cache/private VMO pages into targets; those pages are
         * paid by the VMO's owner (the pager / memory-service domain), once. */
        int charged = 0;
        if (v->pages[page_idx] == 0) {
            uint64_t phys = kvmo_alloc_page(v);
            if (!phys) {
                kobject_active_release(&vs->base); kobject_release(&vs->base);
                kobject_release(vmo_obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            uint8_t *kva = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
            for (int k = 0; k < 4096; k++) kva[k] = 0;
            v->pages[page_idx] = phys;
            charged = 1;
        }

        struct KFrame *f = kframe_alloc_vmo_page(v->pages[page_idx], v);
        if (!f) {
            /* The page stays owned by the VMO (freed at kvmo_destroy); the
             * quota charge stays with it, exactly as the whole-VMO map path. */
            (void)charged;
            kobject_active_release(&vs->base); kobject_release(&vs->base);
            kobject_release(vmo_obj);
            return syscall_err(IRIS_ERR_NO_MEMORY);
        }
        iris_error_t mr = kframe_map_page(f, vs, target_va, map_flags);
        kobject_release(&f->base);  /* drop alloc retain; vs->mappings holds one */
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        kobject_release(vmo_obj);
        return (mr == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(mr);
    }

    /* Wrap/MMIO VMO: no PMM ownership, no vmo_owner retain on the frame. */
    struct KFrame *f = kframe_alloc(v->phys + offset, 4096u, NULL);
    if (!f) {
        kobject_active_release(&vs->base); kobject_release(&vs->base);
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }
    iris_error_t mr = kframe_map_page(f, vs, target_va, map_flags);
    kobject_release(&f->base);
    kobject_active_release(&vs->base); kobject_release(&vs->base);
    kobject_release(vmo_obj);
    return (mr == IRIS_OK) ? syscall_ok_u64(0) : syscall_err(mr);
}


/* ── B4: VMO inter-process share — RETIRED (Stage 4) ──────────────────
 *
 * SYS_VMO_SHARE (46) placed a VMO capability in ANOTHER process's handle
 * table.  That is a cross-process handle producer: the receiver got authority
 * it could not name in its CSpace, with no MDB edge to the sender's cap, so
 * the grant could not be revoked by the grantor.
 *
 * SYS_PROC_CSPACE_MINT / SYS_CSPACE_MINT_INTO are the canonical form and have
 * been since Phase 8 — they install into the target's root CNode as an MDB
 * child of the caller's source slot, which makes the delegation revocable.
 * The number stays permanently reserved and answers NOT_SUPPORTED.
 */
uint64_t sys_vmo_share(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}


/*
 * sys_framebuffer_vmo(auth_h, info_uptr) → vmo_handle or iris_error_t
 */
uint64_t sys_framebuffer_vmo(uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3) {
    (void)arg3;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *auth_obj;
    iris_rights_t   auth_rights;
    iris_error_t r = cspace_resolve_only_obj(t->cspace_root, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_is((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_FB_CONTROL)) {
        kobject_release(auth_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth_obj);

    if (!g_iris_fb_params_valid) return syscall_err(IRIS_ERR_NOT_FOUND);
    g_iris_fb_params_valid = 0;
    if (!copy_to_user_checked(arg1, &g_iris_fb_params, (uint32_t)sizeof(g_iris_fb_params)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KVmo *v = kvmo_wrap(g_iris_fb_params.phys, g_iris_fb_params.size);
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);
    const iris_rights_t rights = RIGHT_READ | RIGHT_WRITE |
                                 RIGHT_DUPLICATE | RIGHT_TRANSFER;

    /* Stage 4: arg2 is a destination slot (RETYPE2 packing).  The framebuffer
     * is authorised by the bootstrap cap in arg0, but that cap was resolved
     * and released above, so the grant is published as a root rather than a
     * child of it — the ancestry the device caps get is Stage 5 work. */
    if (arg2 == 0u) {
        kobject_release(&v->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    iris_error_t pe = syscall_publish_slot(t, &v->base, rights, arg2, 0, 0);
    if (pe != IRIS_OK) return syscall_err(pe);
    return syscall_ok_u64(0);
}
