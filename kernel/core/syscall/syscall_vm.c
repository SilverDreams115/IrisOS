#include "syscall_priv.h"
#include <iris/nc/kframe.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kvmo.h>
#include <stddef.h>



/* ── VMO syscalls ─────────────────────────────────────────────────── */

uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    uint32_t pages = 0;
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (kvmo_size_to_pages(arg0, &pages) != IRIS_OK)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    (void)pages;
    struct KVmo *v = kvmo_create(arg0);
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);
    iris_error_t r = kvmo_bind_owner(v, t->process);
    if (r != IRIS_OK) {
        kvmo_free(v);
        return syscall_err(r);
    }
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &v->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER |
                                        RIGHT_DUPLICATE);
    if (h == HANDLE_INVALID) {
        kvmo_free(v);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&v->base);
    return (uint64_t)h;
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
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_VMO) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }

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

        uint64_t mapped_until = arg1;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            uint32_t page_idx = (uint32_t)(off >> 12);

            if (v->pages[page_idx] == 0) {
                if (kprocess_quota_acquire_page(t->process) != IRIS_OK) {
                    rollback_vmo_maps(vs, arg1, mapped_until);
                    kobject_release(obj);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint64_t phys = pmm_alloc_page();
                if (!phys) {
                    kprocess_quota_release_page(t->process);
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
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_VMO) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
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
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (auth_obj->type != KOBJ_BOOTSTRAP_CAP ||
        !kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_SPAWN_SERVICE)) {
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
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (auth_obj->type != KOBJ_BOOTSTRAP_CAP ||
        !kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_SPAWN_SERVICE)) {
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
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &vmo_obj, &vmo_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (vmo_obj->type != KOBJ_VMO) {
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }

    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &proc_obj, &proc_rights);
    if (r != IRIS_OK) { kobject_release(vmo_obj); return syscall_err(r); }
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
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

        uint64_t mapped_until = vaddr;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            uint32_t page_idx = (uint32_t)(off >> 12);

            if (v->pages[page_idx] == 0) {
                if (kprocess_quota_acquire_page(proc) != IRIS_OK) {
                    rollback_vmo_maps(target_vs, vaddr, mapped_until);
                    kobject_release(vmo_obj); kobject_release(proc_obj);
                    return syscall_err(IRIS_ERR_NO_MEMORY);
                }
                uint64_t phys = pmm_alloc_page();
                if (!phys) {
                    kprocess_quota_release_page(proc);
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


/* ── B4: VMO inter-process share ──────────────────────────────────── */

uint64_t sys_vmo_share(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *vmo_obj;
    iris_rights_t vmo_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &vmo_obj, &vmo_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (vmo_obj->type != KOBJ_VMO) {
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(vmo_rights, RIGHT_READ | RIGHT_DUPLICATE)) {
        kobject_release(vmo_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KObject *proc_obj;
    iris_rights_t proc_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &proc_obj, &proc_rights);
    if (r != IRIS_OK) { kobject_release(vmo_obj); return syscall_err(r); }
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(vmo_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
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
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &auth_obj, &auth_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (auth_obj->type != KOBJ_BOOTSTRAP_CAP ||
        !kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_FRAMEBUFFER)) {
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
