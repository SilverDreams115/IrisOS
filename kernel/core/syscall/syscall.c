#include <iris/syscall.h>
#include <iris/task.h>
#include <iris/pmm.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kirqcap.h>
#include <iris/nc/kioport.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <iris/diag.h>
#include <iris/initrd.h>
#include <iris/elf_loader.h>

/* MSR addresses */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void syscall_entry(void);

#include <iris/serial.h>
#include <iris/paging.h>

#define PAGE_SIZE     0x1000ULL

/*
 * Syscall ABI status in this file:
 *   - Capability/object syscalls use iris_error_t on failure.
 *   - Legacy VFS/stdio-style syscalls (SYS_WRITE) still expose transitional
 *     ad hoc returns and are kept as-is for compatibility.
 * Keep new kernel/object paths on the iris_error_t contract; do not add more
 * generic -1 returns outside the legacy compatibility surface.
 */

static inline uint64_t syscall_err(iris_error_t err) {
    return (uint64_t)(int64_t)err;
}

static inline uint64_t syscall_ok_u64(uint64_t value) {
    return value;
}

static int user_vmo_range_valid(uint64_t virt, uint64_t size) {
    uint64_t end;

    if (size == 0) return 0;
    if ((virt & (PAGE_SIZE - 1ULL)) != 0) return 0;
    if (virt < USER_VMO_BASE || virt >= USER_VMO_TOP) return 0;
    if (size > UINT64_MAX - (PAGE_SIZE - 1ULL)) return 0;

    size = (size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    end = virt + size;
    if (end < virt) return 0;
    if (end > USER_VMO_TOP) return 0;
    return 1;
}

static void rollback_user_maps(uint64_t cr3, uint64_t start, uint64_t end) {
    for (uint64_t virt = start; virt < end; virt += PAGE_SIZE)
        paging_unmap_in(cr3, virt);
}

/* ── Transitional stdio / retired VFS syscall island ─────────────── */

static uint64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = pointer to string in user space */
    if (!user_range_readable(arg0, 1)) return syscall_err(IRIS_ERR_INVALID_ARG);
    /* safe string print — max 256 chars */
    char buf[257];
    uint32_t i = copy_user_cstr_bounded(arg0, buf, (uint32_t)sizeof(buf));
    serial_write("[USER] ");
    serial_write(buf);
    return syscall_ok_u64(i);
}

static uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    serial_write("[SYSCALL] exit code=");
    serial_write_dec(arg0);
    serial_write("\n");
    task_exit_current();
    return 0; /* unreachable */
}

static uint64_t sys_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_yield();
    return 0;
}

static uint64_t sys_getpid(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    return t ? t->id : 0;
}

static uint64_t sys_process_self(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    handle_id_t h;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    h = handle_table_insert(&t->process->handle_table,
                            &t->process->base,
                            RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)h);
}

/* ── Capability IPC (KChannel + HandleTable) ─────────────────────── */

static uint64_t sys_chan_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KChannel *ch = kchannel_alloc();
    if (!ch) return syscall_err(IRIS_ERR_NO_MEMORY);
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &ch->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        kchannel_free(ch);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&ch->base); /* table now holds the reference */
    return (uint64_t)h;
}

static uint64_t sys_chan_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_readable(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KChanMsg msg;
    if (!copy_from_user_checked(&msg, arg1, (uint32_t)sizeof(msg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    if (msg.attached_handle != HANDLE_INVALID) {
        struct KObject *xfer_obj;
        iris_rights_t   xfer_rights;
        iris_error_t xr = handle_table_get_object(&t->process->handle_table,
                                                  msg.attached_handle,
                                                  &xfer_obj, &xfer_rights);
        if (xr != IRIS_OK) { kobject_release(obj); return syscall_err(xr); }
        if (!rights_check(xfer_rights, RIGHT_TRANSFER)) {
            kobject_release(xfer_obj);
            kobject_release(obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }

        iris_rights_t moved_rights = rights_reduce(xfer_rights, msg.attached_rights);
        if (moved_rights == RIGHT_NONE) {
            kobject_release(xfer_obj);
            kobject_release(obj);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }

        kobject_active_retain(xfer_obj);
        r = kchannel_send_attached((struct KChannel *)obj, &msg, xfer_obj, moved_rights);
        if (r != IRIS_OK) {
            kobject_active_release(xfer_obj);
            kobject_release(xfer_obj);
            kobject_release(obj);
            return syscall_err(r);
        }

        r = handle_table_close(&t->process->handle_table, msg.attached_handle);
        if (r != IRIS_OK) { kobject_release(obj); return syscall_err(r); }
    } else {
        r = kchannel_send((struct KChannel *)obj, &msg);
    }
    kobject_release(obj);
    return syscall_err(r);
}

static uint64_t sys_chan_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KChanMsg msg;
    r = kchannel_recv_into_process((struct KChannel *)obj, t->process, &msg);
    kobject_release(obj);
    if (r == IRIS_OK && !copy_to_user_checked(arg1, &msg, (uint32_t)sizeof(msg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}

/*
 * sys_chan_recv_nb — non-blocking channel receive.
 *
 * Identical to sys_chan_recv except it calls kchannel_try_recv_into_process
 * (no blocking).  Returns IRIS_ERR_WOULD_BLOCK immediately if the channel is
 * empty.  Useful for polling loops and supervisor-side drain passes.
 */
static uint64_t sys_chan_recv_nb(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    struct KChanMsg msg;
    r = kchannel_try_recv_into_process((struct KChannel *)obj, t->process, &msg);
    kobject_release(obj);
    if (r == IRIS_OK && !copy_to_user_checked(arg1, &msg, (uint32_t)sizeof(msg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}

static uint64_t sys_handle_close(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(handle_table_close(&t->process->handle_table, (handle_id_t)arg0));
}

/* ── VMO syscalls ─────────────────────────────────────────────────── */

static uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    uint32_t pages = 0;
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (kvmo_size_to_pages(arg0, &pages) != IRIS_OK)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    (void)pages;
    struct KVmo *v = kvmo_create(arg0);
    if (!v) return syscall_err(IRIS_ERR_NO_MEMORY);
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &v->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        kvmo_free(v);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&v->base);
    return (uint64_t)h;
}

static uint64_t sys_vmo_map(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
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
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    int writable = (arg2 & 1) != 0;
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

    map_size = (v->size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(t->process->cr3, arg1 + off) != 0) {
            kobject_release(obj);
            return syscall_err(IRIS_ERR_BUSY);
        }
    }

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
static uint64_t sys_vmo_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process || !t->process->cr3) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t vaddr = arg0;
    uint64_t size  = arg1;

    if (!user_vmo_range_valid(vaddr, size)) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t map_size = (size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE)
        paging_unmap_in(t->process->cr3, vaddr + off);

    return syscall_ok_u64(0);
}

/* ── Handle duplication ───────────────────────────────────────────── */

/*
 * sys_handle_dup(src_handle, new_rights) → new_handle_id
 *
 * Duplicates src_handle into a new handle in the caller's own table.
 * new_rights must be a subset of the caller's existing rights on src_handle.
 * Pass RIGHT_SAME_RIGHTS to keep the same rights (rights_reduce handles this).
 * The reduced rights set must not collapse to RIGHT_NONE.
 * Requires RIGHT_DUPLICATE on the source handle.
 */
static uint64_t sys_handle_dup(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (!rights_check(rights, RIGHT_DUPLICATE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t new_rights = rights_reduce(rights, (iris_rights_t)arg1);
    if (new_rights == RIGHT_NONE) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    handle_id_t   new_h      = handle_table_insert(&t->process->handle_table, obj, new_rights);
    kobject_release(obj);

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)new_h);
}

/* ── Notification syscalls ────────────────────────────────────────── */

static uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KNotification *n = knotification_alloc();
    if (!n) return syscall_err(IRIS_ERR_NO_MEMORY);
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &n->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT |
                                        RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        knotification_free(n);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&n->base); /* table now holds the reference */
    return (uint64_t)h;
}

static uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_NOTIFICATION) {
        kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    knotification_signal((struct KNotification *)obj, arg1);
    kobject_release(obj);
    return syscall_ok_u64(IRIS_OK);
}

static uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_NOTIFICATION) {
        kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WAIT)) {
        kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    uint64_t bits = 0;
    r = knotification_wait((struct KNotification *)obj, &bits);
    kobject_release(obj);
    if (r == IRIS_OK && !copy_to_user_checked(arg1, &bits, (uint32_t)sizeof(bits)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}

/* ── ELF service spawning ─────────────────────────────────────────── */

/*
 * sys_spawn_service(name_uptr, out_chan_ptr) → proc_handle or iris_error_t
 *
 * Spawns a named service from the kernel initrd. Restricted by an explicit
 * bootstrap capability handle delivered during svcmgr bootstrap.
 *
 * Semantics match sys_spawn except that the child process is loaded from a
 * named ELF image instead of executing a kernel-text function pointer.
 *
 * Steps:
 *   1. Authority check — caller must present a bootstrap spawn capability.
 *   2. Copy service name from user space (max INITRD_NAME_MAX chars).
 *   3. Look up the named ELF image in the initrd.
 *   4. Load the ELF into a new isolated address space via elf_loader_load().
 *   5. Create a bootstrap KChannel pair.
 *   6. Spawn a ring-3 task from the loaded image via task_spawn_elf().
 *   7. Install handles into parent and child tables.
 *   8. Return proc_handle to the parent; write chan_handle to *out_chan_ptr.
 *
 * On any failure all allocated resources are rolled back.
 */

#define SPAWN_SVC_NAME_MAX 32u  /* max service name length including NUL */

static uint64_t sys_spawn_service(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task   *parent = task_current();
    struct KChannel *ch   = 0;
    handle_id_t    proc_h      = HANDLE_INVALID;
    handle_id_t    parent_ch   = HANDLE_INVALID;
    handle_id_t    child_h     = HANDLE_INVALID;
    iris_elf_image_t img;
    struct task    *child = 0;

    if (!parent || !parent->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    {
        struct KObject *auth_obj;
        iris_rights_t auth_rights;
        iris_error_t auth_r = handle_table_get_object(&parent->process->handle_table,
                                                      (handle_id_t)arg2,
                                                      &auth_obj, &auth_rights);
        if (auth_r != IRIS_OK) return syscall_err(auth_r);
        if (auth_obj->type != KOBJ_BOOTSTRAP_CAP) {
            kobject_release(auth_obj);
            return syscall_err(IRIS_ERR_WRONG_TYPE);
        }
        if (!rights_check(auth_rights, RIGHT_READ) ||
            !kbootcap_allows((struct KBootstrapCap *)auth_obj, IRIS_BOOTCAP_SPAWN_SERVICE)) {
            kobject_release(auth_obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        kobject_release(auth_obj);
    }

    /* Validate optional out pointer */
    if (arg1 != 0 && !user_range_writable(arg1, (uint32_t)sizeof(handle_id_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Read service name from user space */
    char name[SPAWN_SVC_NAME_MAX];
    uint32_t name_len = copy_user_cstr_bounded(arg0, name, SPAWN_SVC_NAME_MAX);
    if (name_len == 0) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Find the ELF image in the initrd */
    const void *elf_data = 0;
    uint32_t    elf_size = 0;
    if (!initrd_find(name, &elf_data, &elf_size))
        return syscall_err(IRIS_ERR_NOT_FOUND);

    /* Load the ELF into a new isolated address space */
    iris_error_t lerr = elf_loader_load(elf_data, elf_size, &img);
    if (lerr != IRIS_OK) return syscall_err(lerr);

    /* Allocate bootstrap channel */
    ch = kchannel_alloc();
    if (!ch) { elf_loader_free_image(&img); return syscall_err(IRIS_ERR_NO_MEMORY); }

    /* Spawn the child task from the ELF image (arg0 patched below) */
    child = task_spawn_elf(&img, 0);
    if (!child) {
        kchannel_free(ch);
        elf_loader_free_image(&img);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }
    /* img.cr3_phys and img.segs are now owned by child->process */

    /* Insert bootstrap channel handle into child's table */
    child_h = handle_table_insert(&child->process->handle_table,
                                  &ch->base, RIGHT_READ | RIGHT_WRITE);
    if (child_h == HANDLE_INVALID) {
        kchannel_free(ch);
        task_abort_spawned_user(child);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }

    /* Deliver bootstrap handle as child's arg0 */
    task_set_bootstrap_arg0(child, (uint64_t)child_h);

    /* Insert KProcess into parent's handle table */
    proc_h = handle_table_insert(&parent->process->handle_table,
                                 &child->process->base,
                                 RIGHT_READ | RIGHT_ROUTE | RIGHT_MANAGE | RIGHT_DUPLICATE);
    if (proc_h == HANDLE_INVALID) {
        kobject_release(&ch->base);
        task_abort_spawned_user(child);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }

    /* Optionally install parent-side channel handle */
    if (arg1 != 0) {
        parent_ch = handle_table_insert(&parent->process->handle_table,
                                        &ch->base, RIGHT_READ | RIGHT_WRITE);
        if (parent_ch == HANDLE_INVALID) {
            (void)handle_table_close(&parent->process->handle_table, proc_h);
            kobject_release(&ch->base);
            task_abort_spawned_user(child);
            return syscall_err(IRIS_ERR_TABLE_FULL);
        }
        if (!copy_to_user_checked(arg1, &parent_ch, (uint32_t)sizeof(parent_ch))) {
            (void)handle_table_close(&parent->process->handle_table, parent_ch);
            (void)handle_table_close(&parent->process->handle_table, proc_h);
            kobject_release(&ch->base);
            task_abort_spawned_user(child);
            return syscall_err(IRIS_ERR_INVALID_ARG);
        }
    }
    kobject_release(&ch->base);

    return syscall_ok_u64((uint64_t)proc_h);
}

/* ── Handle transfer ──────────────────────────────────────────────── */

/*
 * sys_handle_transfer(src_handle, dest_proc_handle, new_rights) → new_handle_id
 *
 * Moves src_handle from caller's table into dest process's table.
 * Requires RIGHT_TRANSFER on src_handle.
 * Requires RIGHT_MANAGE on dest_proc_handle.
 * new_rights must be a subset of src rights (or RIGHT_SAME_RIGHTS).
 * The reduced rights set must not collapse to RIGHT_NONE.
 * Consumes (closes) src_handle in caller's table on success.
 * Returns new handle_id in dest's table, or iris_error_t cast to uint64_t on failure.
 */
static uint64_t sys_handle_transfer(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Get source object — requires RIGHT_TRANSFER */
    struct KObject *src_obj;
    iris_rights_t   src_rights;
    iris_error_t r = handle_table_get_object(&caller->process->handle_table, (handle_id_t)arg0,
                                             &src_obj, &src_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (!rights_check(src_rights, RIGHT_TRANSFER)) {
        kobject_release(src_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Get destination KProcess — requires RIGHT_MANAGE */
    struct KObject *dest_obj;
    iris_rights_t   dest_rights;
    r = handle_table_get_object(&caller->process->handle_table, (handle_id_t)arg1,
                                &dest_obj, &dest_rights);
    if (r != IRIS_OK) { kobject_release(src_obj); return syscall_err(r); }
    if (dest_obj->type != KOBJ_PROCESS) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(dest_rights, RIGHT_MANAGE)) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *dest_proc = (struct KProcess *)dest_obj;
    if (!kprocess_is_alive(dest_proc)) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    iris_rights_t new_rights = rights_reduce(src_rights, (iris_rights_t)arg2);
    if (new_rights == RIGHT_NONE) {
        kobject_release(src_obj);
        kobject_release(dest_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    handle_id_t   new_h      = handle_table_insert(&dest_proc->handle_table,
                                                   src_obj, new_rights);
    kobject_release(src_obj);
    kobject_release(dest_obj);

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);

    /* Consume the source handle — transfer is move, not copy */
    handle_table_close(&caller->process->handle_table, (handle_id_t)arg0);
    return syscall_ok_u64((uint64_t)new_h);
}

/* ── Process lifecycle query ──────────────────────────────────────── */

/*
 * sys_process_status(proc_handle) → 1 (alive), 0 (dead), or iris_error_t
 *
 * Non-blocking.  Returns immediately regardless of the target state.
 * Requires RIGHT_READ on proc_handle.
 *
 * Lifecycle contract:
 *   - Returns 1 while the process is running or blocked (main_thread alive).
 *   - Returns 0 once the process has called SYS_EXIT or been reaped;
 *     kprocess_teardown has run and TASK_DEAD has been set.
 *   - The handle remains valid after death until the caller closes it;
 *     this allows the caller to detect and then clean up in one pass.
 *   - Closing the handle (SYS_HANDLE_CLOSE) is the caller's responsibility
 *     after observing death; the KProcess is released when refcount hits zero.
 */
static uint64_t sys_process_status(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (obj->type != KOBJ_PROCESS) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    int alive = kprocess_is_alive((struct KProcess *)obj);
    kobject_release(obj);
    return syscall_ok_u64(alive ? 1 : 0);
}

/*
 * sys_process_watch(proc_handle, chan_handle, cookie) → 0 or iris_error_t
 *
 * Registers a single process-exit watch for proc_handle. When the target
 * process tears down, the kernel sends PROC_EVENT_MSG_EXIT into chan_handle.
 *
 * This is the current event-driven lifecycle path for service supervision.
 * SYS_PROCESS_STATUS remains available as a fallback/non-blocking query.
 */
static uint64_t sys_process_watch(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    struct KObject *proc_obj;
    struct KObject *chan_obj;
    iris_rights_t proc_rights;
    iris_rights_t chan_rights;
    iris_error_t r;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg0, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_READ)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &chan_obj, &chan_rights);
    if (r != IRIS_OK) {
        kobject_release(proc_obj);
        return syscall_err(r);
    }
    if (chan_obj->type != KOBJ_CHANNEL) {
        kobject_release(proc_obj);
        kobject_release(chan_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(chan_rights, RIGHT_WRITE)) {
        kobject_release(proc_obj);
        kobject_release(chan_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    r = kprocess_watch_exit((struct KProcess *)proc_obj,
                            (struct KChannel *)chan_obj,
                            (handle_id_t)arg0,
                            (uint32_t)arg2);
    kobject_release(proc_obj);
    kobject_release(chan_obj);
    return syscall_err(r);
}

static uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = ticks to sleep (at 100 Hz, 1 tick = 10ms) */
    if (arg0 == 0) return 0;
    scheduler_sleep_current(arg0);
    return 0;
}

/* ── Process termination ──────────────────────────────────────────── */

/*
 * sys_process_kill(proc_handle) → 0 or iris_error_t
 *
 * Requires RIGHT_MANAGE on proc_handle.
 * Cannot be used for self-termination — use SYS_EXIT for that (IRIS_ERR_INVALID_ARG).
 * Idempotent: if the target is already dead, returns 0 immediately.
 *
 * Internally calls task_kill_external which: runs kprocess_teardown (fires exit
 * watches, closes the process's own handle table, unregisters IRQ routes),
 * frees user stack pages, reaps the address space (safe since the caller's CR3
 * is different from the target's), and releases the kernel's creation reference.
 *
 * The caller's handle to the proc remains valid until the caller closes it;
 * the KProcess object is freed when all handles to it are closed.
 */
static uint64_t sys_process_kill(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);

    if (obj->type != KOBJ_PROCESS) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_MANAGE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *target = (struct KProcess *)obj;

    /* Prevent suicide — caller must use SYS_EXIT for self-termination. */
    if (target == t->process) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    /* Idempotent: already dead. */
    if (!kprocess_is_alive(target)) {
        kobject_release(obj);
        return syscall_ok_u64(0);
    }

    task_kill_external(target->main_thread);
    kobject_release(obj);
    return syscall_ok_u64(0);
}

/* ── IRQ route management ─────────────────────────────────────────── */

/*
 * sys_irq_route_register(irqcap_handle, chan_handle, proc_handle) → 0 or iris_error_t
 *
 * Routes the hardware IRQ line embedded in irqcap_handle into the KChannel
 * behind chan_handle.  The IRQ number is extracted from the KIrqCap object;
 * callers cannot supply an arbitrary vector — they are bound to whatever the
 * kernel encoded into the capability at boot.
 *
 * The route is owned by the KProcess behind proc_handle: when that process
 * exits, kprocess_teardown → irq_routing_unregister_owner clears the route
 * automatically.
 *
 * The irqcap_handle is not consumed — svcmgr may reuse it across service
 * restarts without requesting a new capability.
 *
 * Authority:
 *   irqcap_handle — KOBJ_IRQ_CAP with RIGHT_ROUTE.
 *   chan_handle   — KOBJ_CHANNEL with RIGHT_READ|RIGHT_WRITE.
 *   proc_handle   — KOBJ_PROCESS with RIGHT_READ|RIGHT_ROUTE.
 */
static uint64_t sys_irq_route_register(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Resolve the IRQ capability — it carries the authorized IRQ number */
    struct KObject  *irqcap_obj;
    iris_rights_t    irqcap_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &irqcap_obj, &irqcap_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (irqcap_obj->type != KOBJ_IRQ_CAP) {
        kobject_release(irqcap_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(irqcap_rights, RIGHT_ROUTE)) {
        kobject_release(irqcap_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    uint8_t irq_num = ((struct KIrqCap *)irqcap_obj)->irq_num;
    kobject_release(irqcap_obj);

    if (irq_num >= IRQ_ROUTE_MAX) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Resolve and validate the channel handle */
    struct KObject  *ch_obj;
    iris_rights_t    ch_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &ch_obj, &ch_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (ch_obj->type != KOBJ_CHANNEL) {
        kobject_release(ch_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(ch_rights, RIGHT_READ | RIGHT_WRITE)) {
        kobject_release(ch_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Resolve and validate the process handle (will own the route) */
    struct KObject  *proc_obj;
    iris_rights_t    proc_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg2, &proc_obj, &proc_rights);
    if (r != IRIS_OK) {
        kobject_release(ch_obj);
        return syscall_err(r);
    }
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(ch_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_READ | RIGHT_ROUTE)) {
        kobject_release(ch_obj);
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* irq_routing_register retains ch on its own; proc is an unretained owner
     * pointer — safe because kprocess_teardown calls unregister_owner before
     * the process object is freed. */
    irq_routing_register(irq_num, (struct KChannel *)ch_obj,
                         (struct KProcess *)proc_obj);
    kobject_release(ch_obj);
    kobject_release(proc_obj);
    return syscall_err(IRIS_OK);
}

/* ── I/O port access via KIoPort capability ──────────────────────── */

/*
 * sys_ioport_in(ioport_handle, port_offset) → uint8_t value or iris_error_t
 *
 * Executes the x86 IN instruction for one byte from:
 *   cap->base_port + port_offset
 * Returns the byte in bits [7:0] of the result on success.
 * Requires RIGHT_READ on ioport_handle.
 */
static uint64_t sys_ioport_in(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_IOPORT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KIoPort *port = (struct KIoPort *)obj;
    uint32_t offset = (uint32_t)(arg1 & 0xFFFFu);
    if (offset >= (uint32_t)port->count) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint16_t io_port = (uint16_t)(port->base_port + (uint16_t)offset);
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(io_port));
    kobject_release(obj);
    return syscall_ok_u64((uint64_t)value);
}

/*
 * sys_ioport_out(ioport_handle, port_offset, value) → 0 or iris_error_t
 *
 * Executes the x86 OUT instruction, writing the low byte of value to:
 *   cap->base_port + port_offset
 * Requires RIGHT_WRITE on ioport_handle.
 */
static uint64_t sys_ioport_out(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_IOPORT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KIoPort *port = (struct KIoPort *)obj;
    uint32_t offset = (uint32_t)(arg1 & 0xFFFFu);
    if (offset >= (uint32_t)port->count) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint16_t io_port = (uint16_t)(port->base_port + (uint16_t)offset);
    uint8_t  value   = (uint8_t)(arg2 & 0xFFu);
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(io_port));
    kobject_release(obj);
    return syscall_ok_u64(0);
}

/*
 * sys_diag_snapshot — SYS_DIAG_SNAPSHOT implementation.
 *
 * Captures a compact snapshot of kernel-side diagnostics state into the
 * caller-supplied user buffer.  Unrestricted: any task may query.
 *
 * arg0: user pointer to a buffer of at least IRIS_DIAG_SNAPSHOT_SIZE (64) bytes.
 *       Must be writable user-space memory; validated before any kernel reads.
 *
 * The snapshot is built in a local kernel-stack struct and copied to user
 * space atomically via copy_to_user_checked.  Callers must verify
 * snapshot.magic == IRIS_DIAG_MAGIC and snapshot.version == IRIS_DIAG_VERSION
 * before reading any other field.
 *
 * Returns IRIS_OK (0) on success, IRIS_ERR_INVALID_ARG (-1) if the buffer
 * pointer is invalid or not writable.
 */
static uint64_t sys_diag_snapshot(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;

    if (!user_range_writable(arg0, (uint32_t)IRIS_DIAG_SNAPSHOT_SIZE))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct iris_diag_snapshot snap;
    uint8_t *raw = (uint8_t *)&snap;
    for (uint32_t i = 0; i < (uint32_t)sizeof(snap); i++) raw[i] = 0;

    snap.magic              = IRIS_DIAG_MAGIC;
    snap.version            = IRIS_DIAG_VERSION;
    snap.tasks_live         = sched_live_task_count();
    snap.tasks_max          = (uint32_t)TASK_MAX;
    snap.kproc_live         = kprocess_live_count();
    snap.kproc_max          = (uint32_t)KPROCESS_POOL_SIZE;
    snap.irq_routes_active  = irq_routing_active_count();
    snap.irq_routes_max     = (uint32_t)IRQ_ROUTE_MAX;
    uint64_t ticks          = sched_current_ticks();
    snap.ticks_lo           = (uint32_t)(ticks & 0xFFFFFFFFu);
    snap.ticks_hi           = (uint32_t)(ticks >> 32);
    snap.kchan_live         = kchannel_live_count();
    snap.kchan_max          = (uint32_t)KCHANNEL_POOL_SIZE;
    snap.knotif_live        = knotification_live_count();
    snap.knotif_max         = (uint32_t)KNOTIF_POOL_SIZE;
    snap.kvmo_live          = kvmo_live_count();
    snap.kvmo_max           = (uint32_t)KVMO_POOL_SIZE;

    copy_to_user_checked(arg0, &snap, (uint32_t)sizeof(snap));
    return syscall_err(IRIS_OK);
}

/* ── Channel seal ─────────────────────────────────────────────────── */

/*
 * sys_chan_seal(chan_handle) → 0 or iris_error_t
 *
 * Requires RIGHT_WRITE on chan_handle.
 * Explicitly closes the channel (marks it sealed) and wakes all blocked
 * receivers, which return IRIS_ERR_CLOSED.  Future sends also return
 * IRIS_ERR_CLOSED.  Already-buffered messages can still be drained.
 *
 * The handle itself is NOT consumed; close it with SYS_HANDLE_CLOSE afterwards.
 * Idempotent: sealing an already-sealed channel returns 0.
 *
 * Primary use: svcmgr poisons old service channels before a restart so stale
 * client handles fail fast instead of silently queuing to a dead endpoint.
 */
static uint64_t sys_chan_seal(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_CHANNEL) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    kchannel_seal((struct KChannel *)obj);
    kobject_release(obj);
    return syscall_ok_u64(0);
}

/* ── Synchronous channel call ─────────────────────────────────────── */

/*
 * sys_chan_call(req_chan, msg_uptr, reply_chan) → 0 or iris_error_t
 *
 * Requires RIGHT_WRITE on req_chan and RIGHT_READ on reply_chan.
 * Sends *msg_uptr on req_chan, then blocks on reply_chan until a reply
 * arrives; the reply overwrites *msg_uptr in place.
 *
 * The outbound request may NOT carry an attached handle (msg->attached_handle
 * is forced to HANDLE_INVALID before sending).  The inbound reply MAY carry
 * an attached handle, which is installed in the caller's handle table and
 * written into msg->attached_handle on return.
 *
 * req_chan and reply_chan are NOT consumed — both handles remain valid after
 * the call.  This is a convenience wrapper equivalent to:
 *   SYS_CHAN_SEND(req_chan, msg) + SYS_CHAN_RECV(reply_chan, msg)
 * but in a single syscall to minimize ring transitions.
 */
static uint64_t sys_chan_call(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Validate the user message buffer — read for send, write for recv */
    if (!user_range_readable(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Resolve req_chan — requires RIGHT_WRITE */
    struct KObject  *req_obj;
    iris_rights_t    req_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &req_obj, &req_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (req_obj->type != KOBJ_CHANNEL) {
        kobject_release(req_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(req_rights, RIGHT_WRITE)) {
        kobject_release(req_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Resolve reply_chan — requires RIGHT_READ */
    struct KObject  *rep_obj;
    iris_rights_t    rep_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg2, &rep_obj, &rep_rights);
    if (r != IRIS_OK) {
        kobject_release(req_obj);
        return syscall_err(r);
    }
    if (rep_obj->type != KOBJ_CHANNEL) {
        kobject_release(req_obj);
        kobject_release(rep_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rep_rights, RIGHT_READ)) {
        kobject_release(req_obj);
        kobject_release(rep_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* Copy request from user, clear attached handle (no transfer on request path) */
    struct KChanMsg msg;
    if (!copy_from_user_checked(&msg, arg1, (uint32_t)sizeof(msg))) {
        kobject_release(req_obj);
        kobject_release(rep_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    msg.attached_handle = HANDLE_INVALID;
    msg.attached_rights = RIGHT_NONE;

    /* Send the request — no handle transfer */
    r = kchannel_send((struct KChannel *)req_obj, &msg);
    kobject_release(req_obj);
    if (r != IRIS_OK) {
        kobject_release(rep_obj);
        return syscall_err(r);
    }

    /* Block until a reply arrives, installing any attached handle into our table */
    r = kchannel_recv_into_process((struct KChannel *)rep_obj, t->process, &msg);
    kobject_release(rep_obj);
    if (r != IRIS_OK) return syscall_err(r);

    /* Write the reply back to the user buffer in place */
    if (!copy_to_user_checked(arg1, &msg, (uint32_t)sizeof(msg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    return syscall_ok_u64(0);
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,

                          uint64_t arg1, uint64_t arg2) {
    switch (num) {
        case SYS_WRITE:  return sys_write(arg0, arg1, arg2);
        case SYS_EXIT:   return sys_exit(arg0, arg1, arg2);
        case SYS_GETPID: return sys_getpid(arg0, arg1, arg2);
        case SYS_YIELD:  return sys_yield(arg0, arg1, arg2);
        case SYS_BRK:        return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 20 */
        case SYS_SLEEP:      return sys_sleep(arg0, arg1, arg2);
        case SYS_CHAN_CREATE:  return sys_chan_create(arg0, arg1, arg2);
        case SYS_CHAN_SEND:    return sys_chan_send(arg0, arg1, arg2);
        case SYS_CHAN_RECV:    return sys_chan_recv(arg0, arg1, arg2);
        case SYS_HANDLE_CLOSE: return sys_handle_close(arg0, arg1, arg2);
        case SYS_VMO_CREATE:  return sys_vmo_create(arg0, arg1, arg2);
        case SYS_VMO_MAP:     return sys_vmo_map(arg0, arg1, arg2);
        case SYS_VMO_UNMAP:   return sys_vmo_unmap(arg0, arg1, arg2);
        case SYS_SPAWN:         return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 19 */
        case SYS_SPAWN_SERVICE: return sys_spawn_service(arg0, arg1, arg2);
        case SYS_NOTIFY_CREATE: return sys_notify_create(arg0, arg1, arg2);
        case SYS_NOTIFY_SIGNAL: return sys_notify_signal(arg0, arg1, arg2);
        case SYS_NOTIFY_WAIT:   return sys_notify_wait(arg0, arg1, arg2);
        case SYS_HANDLE_DUP:    return sys_handle_dup(arg0, arg1, arg2);
        case SYS_HANDLE_TRANSFER: return sys_handle_transfer(arg0, arg1, arg2);
        case SYS_PROCESS_SELF:    return sys_process_self(arg0, arg1, arg2);
        case SYS_PROCESS_STATUS:  return sys_process_status(arg0, arg1, arg2);
        case SYS_PROCESS_WATCH:   return sys_process_watch(arg0, arg1, arg2);
        case SYS_IRQ_ROUTE_REGISTER: return sys_irq_route_register(arg0, arg1, arg2);
        case SYS_IOPORT_IN:          return sys_ioport_in(arg0, arg1, arg2);
        case SYS_IOPORT_OUT:         return sys_ioport_out(arg0, arg1, arg2);
        case SYS_CHAN_RECV_NB:        return sys_chan_recv_nb(arg0, arg1, arg2);
        case SYS_PROCESS_KILL:        return sys_process_kill(arg0, arg1, arg2);
        case SYS_DIAG_SNAPSHOT:  return sys_diag_snapshot(arg0, arg1, arg2);
        case SYS_CHAN_SEAL:       return sys_chan_seal(arg0, arg1, arg2);
        case SYS_CHAN_CALL:       return sys_chan_call(arg0, arg1, arg2);
        default:
            serial_write("[SYSCALL] unknown syscall=");
            serial_write_dec(num);
            serial_write("\n");
            return syscall_err(IRIS_ERR_NOT_SUPPORTED);
    }
}

/* syscall_kstack_ptr lives in syscall_entry.S .data section */
extern uint64_t syscall_kstack_ptr;

void syscall_set_kstack(uint64_t kstack_top) {
    syscall_kstack_ptr = kstack_top;
}

void syscall_init(void) {
    /* enable SCE bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= (1ULL << 0); /* SCE = syscall enable */
    wrmsr(MSR_EFER, efer);

    /* STAR: bits 63:48 = user CS-8 (sysret CS = this+16, SS = this+8)
     *       bits 47:32 = kernel CS (syscall CS, SS = this+8)
     * kernel CS = 0x08, kernel SS = 0x10
     * user   CS = 0x1B (0x18|3), user SS = 0x23 (0x20|3)
     * STAR[47:32] = 0x0008  (kernel: CS=0x08, SS=0x10)
     * STAR[63:48] = 0x0013  (sysret: CS=0x1B=0x13|3, SS=0x23=0x1B+8)
     */
    uint64_t star = 0;
    star |= ((uint64_t)0x0008 << 32); /* kernel CS selector */
    star |= ((uint64_t)0x0013 << 48); /* user CS-8 for sysret */
    wrmsr(MSR_STAR, star);

    /* LSTAR: syscall handler entry point */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* SFMASK: clear IF on syscall entry (disable interrupts) */
    wrmsr(MSR_SFMASK, (1ULL << 9)); /* IF = bit 9 */
}
