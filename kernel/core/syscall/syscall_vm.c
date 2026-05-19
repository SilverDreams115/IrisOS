#include "syscall_priv.h"



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


uint64_t sys_vmo_map(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process || !t->process->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_VMO) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }

    struct KVmo *v = (struct KVmo *)obj;
    uint64_t map_size;
    int writable   = (arg2 & 1) != 0;
    int executable = (arg2 & 2) != 0;
    if (writable && executable) { kobject_release(obj); return syscall_err(IRIS_ERR_INVALID_ARG); }
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (!executable) flags |= PAGE_NX;
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (writable) {
        if (!rights_check(rights, RIGHT_WRITE)) {
            kobject_release(obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        flags |= PAGE_WRITABLE;
    }

    if (v->size == 0 || !user_vmo_range_valid(arg1, v->size)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    if (!page_round_up_u64(v->size, &map_size)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_OVERFLOW);
    }

    if (v->demand) {
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_virt_to_phys_in(t->process->cr3, arg1 + off) != 0) {
                kobject_release(obj);
                return syscall_err(IRIS_ERR_BUSY);
            }
        }
        for (struct KVmoMapping *m = t->process->vmo_mappings; m; m = m->next) {
            if (arg1 + map_size <= m->virt_base) continue;
            if (arg1 >= m->virt_base + m->size) continue;
            kobject_release(obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
        r = kprocess_register_vmo_map(t->process, arg1, map_size,
                                      (struct KVmo *)obj, flags);
        kobject_release(obj);
        return syscall_err(r);
    }

    /* Eager path (wrap/MMIO VMOs) */
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(t->process->cr3, arg1 + off) != 0) {
            kobject_release(obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
    }

    {
        uint64_t mapped_until = arg1;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_map_checked_in(t->process->cr3,
                                      arg1 + off,
                                      v->phys + off,
                                      flags) != 0) {
                rollback_user_maps(t->process->cr3, arg1, mapped_until);
                kobject_release(obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
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
 * Removes [vaddr, vaddr+size) PTEs from the caller's page table.
 * Physical pages are NOT freed — the KVmo still owns them; they are released
 * when the last handle to the VMO is closed.
 *
 * The range is validated to lie entirely within [USER_VMO_BASE, USER_VMO_TOP).
 * This prevents accidentally unmapping kernel-shared regions or the user stack.
 * Unmapped pages within the range are silently skipped.
 */
uint64_t sys_vmo_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process || !t->process->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t vaddr = arg0;
    uint64_t size  = arg1;

    if (!user_vmo_range_valid(vaddr, size)) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t map_size = 0;
    if (!page_round_up_u64(size, &map_size)) return syscall_err(IRIS_ERR_OVERFLOW);
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE)
        paging_unmap_in(t->process->cr3, vaddr + off);

    kprocess_unregister_vmo_map(t->process, vaddr);
    return syscall_ok_u64(0);
}


/*
 * sys_vmo_size(vmo_h) → uint64_t byte size or iris_error_t
 *
 * Returns the byte size of the VMO as it was created.
 * Requires RIGHT_READ on vmo_h.
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
 *
 * Authenticates via KOBJ_BOOTSTRAP_CAP (IRIS_BOOTCAP_SPAWN_SERVICE), retrieves
 * the initrd image at the given integer index, and returns a read-only eager
 * KVmo handle wrapping the raw ELF bytes.
 * Name→index mapping is a ring-3 concern (services/common/svc_loader.c).
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

    /* Create a demand VMO and copy ELF bytes into pre-populated pages.
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
 *
 * Authenticates via KOBJ_BOOTSTRAP_CAP (IRIS_BOOTCAP_SPAWN_SERVICE).
 * Returns the number of entries in the kernel's initrd catalog so that
 * ring-3 can verify its local name→index table is consistent at startup.
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
 * Maps VMO pages into a target process's address space.  Requires RIGHT_READ
 * (+ RIGHT_WRITE if writable) on vmo_h and RIGHT_MANAGE on proc_h.
 * W^X enforced: WRITABLE + EXEC simultaneously → ERR_INVALID_ARG.
 * vaddr must be page-aligned within [USER_PRIVATE_BASE, USER_STACK_TOP).
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

    /* Accept fresh (thread_count=0) processes that haven't been torn down. */
    if (kprocess_teardown_complete(proc)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    int writable   = (arg3 & 1) != 0;
    int executable = (arg3 & 2) != 0;
    if (writable && executable) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (!executable) flags |= PAGE_NX;
    if (!rights_check(vmo_rights, RIGHT_READ)) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    if (writable) {
        if (!rights_check(vmo_rights, RIGHT_WRITE)) {
            kobject_release(vmo_obj); kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        flags |= PAGE_WRITABLE;
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

    if (v->demand) {
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_virt_to_phys_in(proc->cr3, vaddr + off) != 0) {
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(IRIS_ERR_BUSY);
            }
        }
        for (struct KVmoMapping *m = proc->vmo_mappings; m; m = m->next) {
            if (vaddr + map_size <= m->virt_base) continue;
            if (vaddr >= m->virt_base + m->size) continue;
            kobject_release(vmo_obj); kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
        r = kprocess_register_vmo_map(proc, vaddr, map_size, v, flags);
        kobject_release(vmo_obj);
        kobject_release(proc_obj);
        return syscall_err(r);
    }

    /* Eager path (wrap/MMIO VMOs) */
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(proc->cr3, vaddr + off) != 0) {
            kobject_release(vmo_obj); kobject_release(proc_obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
    }
    {
        uint64_t mapped_until = vaddr;
        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (paging_map_checked_in(proc->cr3, vaddr + off,
                                      v->phys + off, flags) != 0) {
                rollback_user_maps(proc->cr3, vaddr, mapped_until);
                kobject_release(vmo_obj); kobject_release(proc_obj);
                return syscall_err(IRIS_ERR_NO_MEMORY);
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
 *
 * Claims the physical framebuffer as an MMIO VMO (one-shot).  Requires a
 * KBootstrapCap with IRIS_BOOTCAP_FRAMEBUFFER.  Writes struct iris_fb_params to
 * info_uptr in user space, then returns a non-owning KVmo handle.
 * Clears g_iris_fb_params_valid so the framebuffer can only be claimed once.
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
    /* Commit the one-shot claim before the copy so a concurrent second caller
     * sees NOT_FOUND even if this call is still in progress. */
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
