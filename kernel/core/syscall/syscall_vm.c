#include "syscall_priv.h"
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kvmo.h>
#include <stddef.h>



/*
 * SYS_VSPACE_SELF — hand the caller a capability to its own address space.
 *
 * Fase 19: self-authority only.  A process already fully controls its own
 * address space through the VMO map/unmap syscalls, so a cap to its own VSpace
 * is not new authority — it exists so ring-3 code can mint the cap into a
 * CSpace slot and exercise SYS_FRAME_MAP / SYS_FRAME_UNMAP on itself by CPtr
 * (the resolvers for those syscalls require a VSpace CPtr).  No argument names
 * another VSpace; there is no cross-process reach here.
 */
uint64_t sys_vspace_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process || !t->process->vspace)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KVSpace *vs = t->process->vspace;
    /* handle_entry_init takes the retain + active-retain; the caller's close
     * drops both.  Same object-cap-accessor shape as SYS_TCB_SELF. */
    handle_id_t h = handle_table_insert(&t->process->handle_table, &vs->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}


/*
 * SYS_PROCESS_VSPACE — hand a RIGHT_MANAGE holder a capability to the target
 * process's address space (Fase 25, user-pager groundwork).
 *
 * Authority: RIGHT_MANAGE over the process cap — the same authority that
 * already implies address-space control via SYS_VMO_MAP_INTO.  The returned
 * cap is the delegable/attenuable form of that control: a supervisor mints it
 * (typically down to RIGHT_WRITE) into a pager's CSpace so the pager can
 * drive SYS_FRAME_MAP/SYS_FRAME_UNMAP on the target WITHOUT holding process
 * MANAGE-for-mapping — fault-info (READ) + resume (MANAGE) stay separately
 * scoped on the process cap.  Rights mirror SYS_VSPACE_SELF (no TRANSFER).
 */
uint64_t sys_process_vspace(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KProcess *proc;
    struct KObject  *obj = 0;
    if ((handle_id_t)arg0 == HANDLE_INVALID) {
        /* Self: equivalent to SYS_VSPACE_SELF — no new authority. */
        proc = t->process;
        kobject_retain(&proc->base);
    } else {
        iris_rights_t rights;
        iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                     RIGHT_NONE, KOBJ_PROCESS, &obj, &rights);
        if (r != IRIS_OK) return syscall_err(r);
        if (!rights_check(rights, RIGHT_MANAGE)) {
            kobject_release(obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        proc = (struct KProcess *)obj;
    }

    if (kprocess_teardown_complete(proc)) {
        kobject_release(&proc->base);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }
    struct KVSpace *vs = proc->vspace;
    if (!vs) {
        kobject_release(&proc->base);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    handle_id_t h = handle_table_insert(&t->process->handle_table, &vs->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    kobject_release(&proc->base);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_NO_MEMORY);
    return (uint64_t)h;
}


/* ── VMO syscalls ─────────────────────────────────────────────────── */

/*
 * vmo_create_charged — shared body for SYS_VMO_CREATE / SYS_VMO_CREATE_FOR.
 * Creates a sparse VMO of `size`, charges the VMO OBJECT quota (owned_vmos) and,
 * later, its sparse pages to `payer` (the owner/payer domain), and installs the
 * handle in the CALLER's table (the holder).  Fase 29: owner (payer) and holder
 * are deliberately distinct — a loader creating a child's image VMO charges the
 * CHILD but keeps the handle to map/close it.
 */
static uint64_t vmo_create_charged(struct task *t, uint64_t size,
                                   struct KProcess *payer) {
    uint32_t pages = 0;
    if (kvmo_size_to_pages(size, &pages) != IRIS_OK)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    (void)pages;
    struct KVmo *v = kvmo_create(size);
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);
    iris_error_t r = kvmo_bind_owner(v, payer);
    if (r != IRIS_OK) { kvmo_free(v); return syscall_err(r); }
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &v->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER |
                                        RIGHT_DUPLICATE);
    if (h == HANDLE_INVALID) {
        kvmo_free(v);   /* kvmo_destroy releases the owner charge — no leak */
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&v->base);
    return (uint64_t)h;
}

uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;   /* 1-arg ABI: callers do not set arg1 */
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    return vmo_create_charged(t, arg0, t->process);   /* charge self */
}

/*
 * SYS_VMO_CREATE_FOR(size, charge_target) — explicit, capability-authorized
 * PAYER selection (Fase 29).  The VMO object + its sparse pages are charged to
 * `charge_target` (a KProcess the caller holds RIGHT_MANAGE on), not to the
 * caller.  Same authority SYS_VMO_MAP_INTO requires to map into that process.
 */
uint64_t sys_vmo_create_for(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *payer_obj;
    iris_rights_t   payer_rights;
    iris_error_t pr = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg1,
                                 RIGHT_NONE, KOBJ_PROCESS, &payer_obj, &payer_rights);
    if (pr != IRIS_OK) return syscall_err(pr);
    if (!rights_check(payer_rights, RIGHT_MANAGE)) {
        kobject_release(payer_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    struct KProcess *payer = (struct KProcess *)payer_obj;
    if (kprocess_teardown_complete(payer)) {
        kobject_release(payer_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }
    uint64_t rv = vmo_create_charged(t, arg0, payer);
    kobject_release(payer_obj);
    return rv;
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
    if (!t || !t->process || !t->process->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    /* A1 Increment 1: dual resolver — the VMO may be a CPtr slot or a handle.
     * RIGHT_NONE defers to the READ/WRITE checks below (unchanged). */
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_VMO, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    struct KVSpace *vs = t->process->vspace;
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
            if (paging_virt_to_phys_in(t->process->cr3, arg1 + off) != 0) {
                kobject_release(obj);
                return syscall_err(IRIS_ERR_BUSY);
            }
        }

        /* Fase 29: sparse pages are charged to the VMO's OWNER (payer domain),
         * once, at allocation — not to whoever maps it first.  So a loader that
         * maps a child's segment VMO into its own window to fill it charges the
         * CHILD, and closing/unmapping never strands the charge on the loader
         * (released at kvmo_destroy).  Fallback to the caller if unbound. */
        struct KProcess *vmo_payer = kvmo_owner(v);
        if (!vmo_payer) vmo_payer = t->process;
        uint64_t mapped_until = arg1;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            uint32_t page_idx = (uint32_t)(off >> 12);

            if (v->pages[page_idx] == 0) {
                if (kprocess_quota_acquire_page(vmo_payer) != IRIS_OK) {
                    rollback_vmo_maps(vs, arg1, mapped_until);
                    kobject_release(obj);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint64_t phys = pmm_alloc_page();
                if (!phys) {
                    kprocess_quota_release_page(vmo_payer);
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
        if (paging_virt_to_phys_in(t->process->cr3, arg1 + off) != 0) {
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
    if (!t || !t->process || !t->process->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t vaddr = arg0;
    uint64_t size  = arg1;

    if (!user_vmo_range_valid(vaddr, size)) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KVSpace *vs = t->process->vspace;
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
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
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
    (void)arg2; (void)arg3;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject   *auth_obj;
    iris_rights_t     auth_rights;
    /* Fase 13: dual resolver — spawn cap may be a CPtr slot or a handle. */
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_SPAWN_SERVICE)) {
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
    struct KVmo *v = kvmo_create((uint64_t)elf_size);
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);

    {
        const uint8_t *src = (const uint8_t *)elf_data;
        uint32_t pg;
        for (pg = 0; pg < v->page_capacity; pg++) {
            uint64_t phys = pmm_alloc_page();
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

    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &v->base, RIGHT_READ);
    if (h == HANDLE_INVALID) {
        kvmo_free(v);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&v->base);
    return syscall_ok_u64((uint64_t)h);
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
    /* Fase 13: dual resolver — spawn cap may be a CPtr slot or a handle. */
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_SPAWN_SERVICE)) {
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
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_VMO, &vmo_obj, &vmo_rights);
    if (r != IRIS_OK) return syscall_err(r);

    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    /* A1 Increment 2a: dual resolver on the target process too. */
    r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg1,
                                     RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (r != IRIS_OK) { kobject_release(vmo_obj); return syscall_err(r); }
    if (!rights_check(proc_rights, RIGHT_MANAGE)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KVmo     *v    = (struct KVmo *)vmo_obj;
    struct KProcess *proc = (struct KProcess *)proc_obj;
    uint64_t         vaddr = arg2;
    uint64_t         map_size = 0;

    if (kprocess_teardown_complete(proc)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    struct KVSpace *target_vs = proc->vspace;
    if (!target_vs) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    int writable   = (arg3 & 1) != 0;
    int executable = (arg3 & 2) != 0;
    if (writable && executable) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    uint64_t map_flags = 0;
    if (writable)   map_flags |= 1u;
    if (executable) map_flags |= 2u;

    if (!rights_check(vmo_rights, RIGHT_READ)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (writable && !rights_check(vmo_rights, RIGHT_WRITE)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    if (v->size == 0) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    if (!page_round_up_u64(v->size, &map_size)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }
    if (!user_private_range_valid(vaddr, v->size, USER_STACK_TOP)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    if (v->sparse) {
        /* Pre-check: no VA in the range must already have a PTE. */
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_virt_to_phys_in(proc->cr3, vaddr + off) != 0) {
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(IRIS_ERR_BUSY);
            }
        }

        /* Fase 29: charge the VMO owner (payer domain), not the map target — a
         * shared VMO's pages are paid once by its owner; extra targets that map
         * it do not re-charge (Q6/Q7/Q18). */
        struct KProcess *vmo_payer = kvmo_owner(v);
        if (!vmo_payer) vmo_payer = proc;
        uint64_t mapped_until = vaddr;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            uint32_t page_idx = (uint32_t)(off >> 12);

            if (v->pages[page_idx] == 0) {
                if (kprocess_quota_acquire_page(vmo_payer) != IRIS_OK) {
                    rollback_vmo_maps(target_vs, vaddr, mapped_until);
                    kobject_release(vmo_obj); kobject_release(proc_obj);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint64_t phys = pmm_alloc_page();
                if (!phys) {
                    kprocess_quota_release_page(vmo_payer);
                    rollback_vmo_maps(target_vs, vaddr, mapped_until);
                    kobject_release(vmo_obj); kobject_release(proc_obj);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint8_t *kva = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
                for (int k = 0; k < 4096; k++) kva[k] = 0;
                v->pages[page_idx] = phys;
            }

            struct KFrame *f = kframe_alloc_vmo_page(v->pages[page_idx], v);
            if (!f) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            iris_error_t mr = kframe_map_page(f, target_vs, vaddr + off, map_flags);
            kobject_release(&f->base);
            if (mr != IRIS_OK) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(mr);
            }
            mapped_until = vaddr + off + PAGE_SIZE;
        }

        kobject_release(vmo_obj);
        kobject_release(proc_obj);
        return syscall_ok_u64(0);
    }

    /* Wrap/MMIO VMO path */
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(proc->cr3, vaddr + off) != 0) {
            kobject_release(vmo_obj); kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
    }
    {
        uint64_t mapped_until = vaddr;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            struct KFrame *f = kframe_alloc(v->phys + off, 4096u, NULL);
            if (!f) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            iris_error_t mr = kframe_map_page(f, target_vs, vaddr + off, map_flags);
            kobject_release(&f->base);
            if (mr != IRIS_OK) {
                rollback_vmo_maps(target_vs, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(mr);
            }
            mapped_until = vaddr + off + PAGE_SIZE;
        }
    }
    kobject_release(vmo_obj);
    kobject_release(proc_obj);
    return syscall_ok_u64(0);
}


/*
 * sys_vmo_map_page(vmo_cptr, vspace_cptr, target_va, offset_flags) — Fase 26.
 *
 * Page-granular, offset-addressed map of ONE VMO page into a VSpace named by
 * capability.  The authority is (VMO READ[/WRITE]) + (VSpace WRITE) — NO
 * process MANAGE: the VSpace cap already IS the map-into-target authority
 * (SYS_PROCESS_VSPACE, Fase 25).  This is the VMO-backed analogue of
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
    iris_error_t err = cspace_or_handle_resolve_obj(t->process, vmo_cptr,
                                 RIGHT_NONE, KOBJ_VMO, &vmo_obj, &vmo_rights);
    if (err != IRIS_OK) return syscall_err(err);
    if (!rights_check(vmo_rights, vmo_required)) {
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* VSpace cap: RIGHT_WRITE to install the PTE (dual resolver, Fase 25). */
    struct KVSpace *vs;
    iris_rights_t   vs_rights;
    err = cspace_or_handle_resolve_vspace(t->process, vspace_cptr, RIGHT_WRITE,
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
        /* Fase 29: charge the VMO owner (payer domain), not the mapper.  The
         * pager maps its cache/private VMO pages into targets; those pages are
         * paid by the VMO's owner (the pager / memory-service domain), once. */
        struct KProcess *vmo_payer = kvmo_owner(v);
        if (!vmo_payer) vmo_payer = t->process;
        int charged = 0;
        if (v->pages[page_idx] == 0) {
            if (kprocess_quota_acquire_page(vmo_payer) != IRIS_OK) {
                kobject_active_release(&vs->base); kobject_release(&vs->base);
                kobject_release(vmo_obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
            }
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                kprocess_quota_release_page(vmo_payer);
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


/* ── B4: VMO inter-process share ──────────────────────────────────── */

uint64_t sys_vmo_share(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *vmo_obj;
    iris_rights_t vmo_rights;
    /* A1 Increment 1b: dual resolver on the source VMO only — the destination
     * process stays handle-only until the Process family migrates. */
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_VMO, &vmo_obj, &vmo_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(vmo_rights, RIGHT_READ | RIGHT_DUPLICATE)) {
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KObject *proc_obj;
    iris_rights_t proc_rights;
    /* A1 Increment 2a: dual resolver on the destination process too. */
    r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg1,
                                     RIGHT_NONE, KOBJ_PROCESS, &proc_obj, &proc_rights);
    if (r != IRIS_OK) { kobject_release(vmo_obj); return syscall_err(r); }
    if (!rights_check(proc_rights, RIGHT_MANAGE)) {
        kobject_release(vmo_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (!kprocess_is_alive((struct KProcess *)proc_obj)) {
        kobject_release(vmo_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    iris_rights_t granted = rights_reduce(vmo_rights, (iris_rights_t)arg2);
    if (granted == RIGHT_NONE) {
        kobject_release(vmo_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    struct KProcess *dest = (struct KProcess *)proc_obj;
    handle_id_t new_h = handle_table_insert(&dest->handle_table, vmo_obj, granted);
    kobject_release(vmo_obj);
    kobject_release(proc_obj);
    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)new_h);
}


/*
 * sys_framebuffer_vmo(auth_h, info_uptr) → vmo_handle or iris_error_t
 */
uint64_t sys_framebuffer_vmo(uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3) {
    (void)arg2; (void)arg3;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *auth_obj;
    iris_rights_t   auth_rights;
    iris_error_t r = cspace_or_handle_resolve_obj(t->process, (iris_cptr_t)arg0,
                                 RIGHT_NONE, KOBJ_BOOTSTRAP_CAP, &auth_obj, &auth_rights);
    if (r == IRIS_ERR_WRONG_TYPE) r = IRIS_ERR_ACCESS_DENIED;
    if (r != IRIS_OK) return syscall_err(r);
    if (!kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_FRAMEBUFFER)) {
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

    handle_id_t h = handle_table_insert(&t->process->handle_table, &v->base,
                                        RIGHT_READ | RIGHT_WRITE |
                                        RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        kobject_release(&v->base);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&v->base);
    return syscall_ok_u64((uint64_t)h);
}
