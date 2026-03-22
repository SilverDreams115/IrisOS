#include <iris/syscall.h>
#include <iris/task.h>
#include <iris/pmm.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/knotification.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/nameserver.h>
#include <iris/scheduler.h>

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
#include <iris/vfs.h>

/* user virtual address space bounds */
#define USER_ADDR_MIN 0x1000ULL
#define PAGE_SIZE     0x1000ULL

/* Check that [ptr, ptr+len) is within user range AND mapped in page table.
 * Every page that overlaps the range is checked individually. */
static int user_ptr_valid(uint64_t ptr, uint32_t len) {
    if (ptr == 0) return 0;
    if (ptr < USER_ADDR_MIN) return 0;
    if (len == 0) return 0;
    if (ptr + (uint64_t)len > USER_SPACE_TOP) return 0;
    if (ptr + (uint64_t)len < ptr) return 0; /* overflow check */
    /* verify every page in range is present in the page table */
    uint64_t page = ptr & ~0xFFFULL;
    uint64_t end  = (ptr + len - 1) & ~0xFFFULL;
    for (; page <= end; page += 0x1000ULL) {
        if (paging_virt_to_phys(page) == 0) return 0;
    }
    return 1;
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

static void rollback_user_maps_and_free_phys(uint64_t cr3, uint64_t start, uint64_t end) {
    for (uint64_t virt = start; virt < end; virt += PAGE_SIZE) {
        uint64_t phys = paging_virt_to_phys_in(cr3, virt);
        if (!phys) continue;
        paging_unmap_in(cr3, virt);
        pmm_free_page(phys & ~0xFFFULL);
    }
}

static uint64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = pointer to string in user space */
    if (!user_ptr_valid(arg0, 1)) return (uint64_t)-1;
    const char *s = (const char *)(uintptr_t)arg0;
    /* safe string print — max 256 chars */
    char buf[257];
    uint32_t i = 0;
    while (i < 256 && user_ptr_valid(arg0 + i, 1) && s[i]) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = '\0';
    serial_write("[USER] ");
    serial_write(buf);
    return i;
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

static uint64_t sys_open(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    if (!user_ptr_valid(arg0, 1)) return (uint64_t)-1;
    const char *path = (const char *)(uintptr_t)arg0;
    /* copy path safely */
    char buf[VFS_MAX_NAME];
    uint32_t i = 0;
    while (i < VFS_MAX_NAME - 1 && user_ptr_valid(arg0 + i, 1) && path[i]) {
        buf[i] = path[i]; i++;
    }
    buf[i] = '\0';
    uint32_t flags = (uint32_t)arg1;
    int32_t fd = vfs_open(buf, flags);
    return (uint64_t)(int64_t)fd;
}

static uint64_t sys_read(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    int32_t  fd  = (int32_t)arg0;
    uint64_t buf = arg1;
    uint32_t len = (uint32_t)arg2;
    if (!user_ptr_valid(buf, len)) return (uint64_t)-1;
    int32_t n = vfs_read(fd, (void *)(uintptr_t)buf, len);
    return (uint64_t)(int64_t)n;
}

static uint64_t sys_close(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    int32_t fd = (int32_t)arg0;
    return (uint64_t)(int64_t)vfs_close(fd);
}

static uint64_t sys_brk(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    struct KProcess *proc = t ? t->process : 0;
    if (!proc) return (uint64_t)-1;
    /* arg0 = requested new brk; 0 = query current */
    if (arg0 == 0) return proc->brk;
    if (arg0 < USER_HEAP_BASE) return proc->brk;
    if (arg0 > USER_HEAP_MAX)  return proc->brk;

    uint64_t old_brk = proc->brk;
    uint64_t new_brk = arg0;

    /* map new pages when heap grows */
    if (new_brk > old_brk) {
        uint64_t first = (old_brk + 0xFFFULL) & ~0xFFFULL;  /* round up */
        uint64_t last  = (new_brk - 1ULL)      & ~0xFFFULL;  /* round down */
        uint64_t mapped_until = first;
        for (uint64_t v = first; v <= last; v += 0x1000ULL) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                for (uint64_t rollback = first; rollback < mapped_until; rollback += 0x1000ULL) {
                    uint64_t rollback_phys = paging_virt_to_phys_in(proc->cr3, rollback);
                    if (!rollback_phys) continue;
                    paging_unmap_in(proc->cr3, rollback);
                    pmm_free_page(rollback_phys & ~0xFFFULL);
                }
                return old_brk;  /* OOM — return unchanged brk */
            }
            if (paging_map_checked_in(proc->cr3, v, phys,
                                      PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
                pmm_free_page(phys);
                rollback_user_maps_and_free_phys(proc->cr3, first, mapped_until);
                return old_brk;
            }
            mapped_until = v + 0x1000ULL;
        }
    } else if (new_brk < old_brk) {
        uint64_t first = (new_brk + 0xFFFULL) & ~0xFFFULL;   /* first page no longer needed */
        uint64_t last  = (old_brk - 1ULL)    & ~0xFFFULL;    /* last mapped page in old heap */
        for (uint64_t v = first; v <= last; v += 0x1000ULL) {
            uint64_t phys = paging_virt_to_phys_in(proc->cr3, v);
            if (!phys) continue;
            paging_unmap_in(proc->cr3, v);
            pmm_free_page(phys & ~0xFFFULL);
        }
    }
    proc->brk = new_brk;
    return new_brk;
}

/* ── IPC syscalls — base para servidores de usuario ──────────────── */

static void copy_from_user(void *dst, uint64_t src_uptr, uint32_t len) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)(uintptr_t)src_uptr;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

static void copy_to_user(uint64_t dst_uptr, const void *src, uint32_t len) {
    uint8_t *d = (uint8_t *)(uintptr_t)dst_uptr;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
}

/* ── Capability IPC (KChannel + HandleTable) ─────────────────────── */

static uint64_t sys_chan_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)-1;
    struct KChannel *ch = kchannel_alloc();
    if (!ch) return (uint64_t)-1;
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &ch->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (h == HANDLE_INVALID) { kchannel_free(ch); return (uint64_t)-1; }
    kobject_release(&ch->base); /* table now holds the reference */
    return (uint64_t)h;
}

static uint64_t sys_chan_send(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;
    if (!user_ptr_valid(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return (uint64_t)IRIS_ERR_WRONG_TYPE; }
    if (!rights_check(rights, RIGHT_WRITE)) { kobject_release(obj); return (uint64_t)IRIS_ERR_ACCESS_DENIED; }

    struct KChanMsg msg;
    copy_from_user(&msg, arg1, (uint32_t)sizeof(msg));
    r = kchannel_send((struct KChannel *)obj, &msg);
    kobject_release(obj);
    return (uint64_t)r;
}

static uint64_t sys_chan_recv(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;
    if (!user_ptr_valid(arg1, (uint32_t)sizeof(struct KChanMsg)))
        return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;
    if (obj->type != KOBJ_CHANNEL) { kobject_release(obj); return (uint64_t)IRIS_ERR_WRONG_TYPE; }
    if (!rights_check(rights, RIGHT_READ)) { kobject_release(obj); return (uint64_t)IRIS_ERR_ACCESS_DENIED; }

    struct KChanMsg msg;
    r = kchannel_recv((struct KChannel *)obj, &msg);
    kobject_release(obj);
    if (r == IRIS_OK) copy_to_user(arg1, &msg, (uint32_t)sizeof(msg));
    return (uint64_t)r;
}

static uint64_t sys_handle_close(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;
    return (uint64_t)handle_table_close(&t->process->handle_table, (handle_id_t)arg0);
}

/* ── VMO syscalls ─────────────────────────────────────────────────── */

static uint64_t sys_vmo_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)-1;
    struct KVmo *v = kvmo_create(arg0);
    if (!v) return (uint64_t)-1;
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &v->base,
                                        RIGHT_READ | RIGHT_WRITE);
    if (h == HANDLE_INVALID) { kvmo_free(v); return (uint64_t)-1; }
    kobject_release(&v->base);
    return (uint64_t)h;
}

static uint64_t sys_vmo_map(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process || !t->process->cr3) return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;
    if (obj->type != KOBJ_VMO) { kobject_release(obj); return (uint64_t)IRIS_ERR_WRONG_TYPE; }

    struct KVmo *v = (struct KVmo *)obj;
    uint64_t map_size;
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    int writable = (arg2 & 1) != 0;
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return (uint64_t)IRIS_ERR_ACCESS_DENIED;
    }
    if (writable) {
        if (!rights_check(rights, RIGHT_WRITE)) {
            kobject_release(obj);
            return (uint64_t)IRIS_ERR_ACCESS_DENIED;
        }
        flags |= PAGE_WRITABLE;
    }

    if (v->size == 0 || !user_vmo_range_valid(arg1, v->size)) {
        kobject_release(obj);
        return (uint64_t)IRIS_ERR_INVALID_ARG;
    }

    map_size = (v->size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (paging_virt_to_phys_in(t->process->cr3, arg1 + off) != 0) {
            kobject_release(obj);
            return (uint64_t)IRIS_ERR_BUSY;
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
            return (uint64_t)IRIS_ERR_NO_MEMORY;
        }
        mapped_until = arg1 + off + PAGE_SIZE;
    }
    kobject_release(obj);
    return 0;
}

/* ── Handle duplication ───────────────────────────────────────────── */

/*
 * sys_handle_dup(src_handle, new_rights) → new_handle_id
 *
 * Duplicates src_handle into a new handle in the caller's own table.
 * new_rights must be a subset of the caller's existing rights on src_handle.
 * Pass RIGHT_SAME_RIGHTS to keep the same rights (rights_reduce handles this).
 * Requires RIGHT_DUPLICATE on the source handle.
 */
static uint64_t sys_handle_dup(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;

    if (!rights_check(rights, RIGHT_DUPLICATE)) {
        kobject_release(obj);
        return (uint64_t)IRIS_ERR_ACCESS_DENIED;
    }

    iris_rights_t new_rights = rights_reduce(rights, (iris_rights_t)arg1);
    handle_id_t   new_h      = handle_table_insert(&t->process->handle_table, obj, new_rights);
    kobject_release(obj);

    if (new_h == HANDLE_INVALID) return (uint64_t)IRIS_ERR_TABLE_FULL;
    return (uint64_t)new_h;
}

/* ── Notification syscalls ────────────────────────────────────────── */

static uint64_t sys_notify_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)-1;
    struct KNotification *n = knotification_alloc();
    if (!n) return (uint64_t)-1;
    handle_id_t h = handle_table_insert(&t->process->handle_table,
                                        &n->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE);
    if (h == HANDLE_INVALID) { knotification_free(n); return (uint64_t)-1; }
    kobject_release(&n->base); /* table now holds the reference */
    return (uint64_t)h;
}

static uint64_t sys_notify_signal(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;
    if (obj->type != KOBJ_NOTIFICATION) {
        kobject_release(obj); return (uint64_t)IRIS_ERR_WRONG_TYPE;
    }
    if (!rights_check(rights, RIGHT_WRITE)) {
        kobject_release(obj); return (uint64_t)IRIS_ERR_ACCESS_DENIED;
    }
    knotification_signal((struct KNotification *)obj, arg1);
    kobject_release(obj);
    return IRIS_OK;
}

static uint64_t sys_notify_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;
    if (!user_ptr_valid(arg1, (uint32_t)sizeof(uint64_t)))
        return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;
    if (obj->type != KOBJ_NOTIFICATION) {
        kobject_release(obj); return (uint64_t)IRIS_ERR_WRONG_TYPE;
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj); return (uint64_t)IRIS_ERR_ACCESS_DENIED;
    }
    uint64_t bits = 0;
    r = knotification_wait((struct KNotification *)obj, &bits);
    kobject_release(obj);
    if (r == IRIS_OK)
        copy_to_user(arg1, &bits, (uint32_t)sizeof(bits));
    return (uint64_t)r;
}

/* ── Process spawn ────────────────────────────────────────────────── */

/*
 * sys_spawn(entry_vaddr, out_chan_ptr) → proc_handle
 *
 * Creates a new ring-3 process at entry_vaddr.
 * A bootstrap KChannel is created:
 *   - child receives its bootstrap channel handle via the user-task
 *     bootstrap contract (RBX on first entry, legacy stack mirror)
 *   - parent receives:
 *       return value = KProcess handle (control object)
 *       *out_chan_ptr = KChannel handle (to communicate with child)
 *         (only written if out_chan_ptr != 0 and is a valid user pointer)
 *
 * Rights granted:
 *   parent's KProcess handle: RIGHT_READ | RIGHT_MANAGE
 *   parent's KChannel handle: RIGHT_READ | RIGHT_WRITE
 *   child's KChannel handle:  RIGHT_READ | RIGHT_WRITE
 */
static uint64_t sys_spawn(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    if (arg0 == 0) return (uint64_t)IRIS_ERR_INVALID_ARG;

    struct task *parent = task_current();
    struct task *child = 0;
    struct KChannel *ch = 0;
    struct KProcess *proc = 0;
    handle_id_t proc_h = HANDLE_INVALID;
    handle_id_t parent_ch = HANDLE_INVALID;
    if (!parent || !parent->process) return (uint64_t)IRIS_ERR_INVALID_ARG;

    /* Validate optional out pointer */
    if (arg1 != 0 && !user_ptr_valid(arg1, (uint32_t)sizeof(handle_id_t)))
        return (uint64_t)IRIS_ERR_INVALID_ARG;

    /* 1. Create child task (arg0 on child stack will be patched below) */
    child = task_spawn_user(arg0, 0);
    if (!child) return (uint64_t)IRIS_ERR_NO_MEMORY;

    /* 2. Allocate bootstrap channel */
    ch = kchannel_alloc();
    if (!ch) {
        task_abort_spawned_user(child);
        return (uint64_t)IRIS_ERR_NO_MEMORY;
    }

    /* 3. Insert KChannel into child's handle table */
    handle_id_t child_h = handle_table_insert(&child->process->handle_table,
                                              &ch->base,
                                              RIGHT_READ | RIGHT_WRITE);
    if (child_h == HANDLE_INVALID) {
        kchannel_free(ch);
        task_abort_spawned_user(child);
        return (uint64_t)IRIS_ERR_TABLE_FULL;
    }

    /* 4. Install bootstrap arg0 for the child using the shared helper. */
    task_set_bootstrap_arg0(child, (uint64_t)child_h);

    /* 5. Child becomes externally valid only after parent-facing handles exist. */
    proc = child->process;
    if (!proc) {
        kobject_release(&ch->base);
        task_abort_spawned_user(child);
        return (uint64_t)IRIS_ERR_INTERNAL;
    }

    /* 6. Insert KProcess into parent's handle table */
    proc_h = handle_table_insert(&parent->process->handle_table,
                                 &proc->base,
                                 RIGHT_READ | RIGHT_MANAGE);
    if (proc_h == HANDLE_INVALID) {
        kobject_release(&ch->base);
        task_abort_spawned_user(child);
        return (uint64_t)IRIS_ERR_TABLE_FULL;
    }

    /* 7. Optionally give parent a handle to the bootstrap channel */
    if (arg1 != 0) {
        parent_ch = handle_table_insert(&parent->process->handle_table,
                                        &ch->base,
                                        RIGHT_READ | RIGHT_WRITE);
        if (parent_ch == HANDLE_INVALID) {
            (void)handle_table_close(&parent->process->handle_table, proc_h);
            kobject_release(&ch->base);
            task_abort_spawned_user(child);
            return (uint64_t)IRIS_ERR_TABLE_FULL;
        }
        copy_to_user(arg1, &parent_ch, (uint32_t)sizeof(parent_ch));
    }
    kobject_release(&ch->base); /* table(s) hold the reference(s) */

    return (uint64_t)proc_h;
}

/* ── Handle transfer ──────────────────────────────────────────────── */

/*
 * sys_handle_transfer(src_handle, dest_proc_handle, new_rights) → new_handle_id
 *
 * Moves src_handle from caller's table into dest process's table.
 * Requires RIGHT_TRANSFER on src_handle.
 * Requires RIGHT_MANAGE on dest_proc_handle.
 * new_rights must be a subset of src rights (or RIGHT_SAME_RIGHTS).
 * Consumes (closes) src_handle in caller's table on success.
 * Returns new handle_id in dest's table, or iris_error_t cast to uint64_t on failure.
 */
static uint64_t sys_handle_transfer(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *caller = task_current();
    if (!caller || !caller->process) return (uint64_t)IRIS_ERR_INVALID_ARG;

    /* Get source object — requires RIGHT_TRANSFER */
    struct KObject *src_obj;
    iris_rights_t   src_rights;
    iris_error_t r = handle_table_get_object(&caller->process->handle_table, (handle_id_t)arg0,
                                             &src_obj, &src_rights);
    if (r != IRIS_OK) return (uint64_t)r;
    if (!rights_check(src_rights, RIGHT_TRANSFER)) {
        kobject_release(src_obj);
        return (uint64_t)IRIS_ERR_ACCESS_DENIED;
    }

    /* Get destination KProcess — requires RIGHT_MANAGE */
    struct KObject *dest_obj;
    iris_rights_t   dest_rights;
    r = handle_table_get_object(&caller->process->handle_table, (handle_id_t)arg1,
                                &dest_obj, &dest_rights);
    if (r != IRIS_OK) { kobject_release(src_obj); return (uint64_t)r; }
    if (dest_obj->type != KOBJ_PROCESS) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return (uint64_t)IRIS_ERR_WRONG_TYPE;
    }
    if (!rights_check(dest_rights, RIGHT_MANAGE)) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return (uint64_t)IRIS_ERR_ACCESS_DENIED;
    }

    struct KProcess *dest_proc = (struct KProcess *)dest_obj;
    if (!kprocess_is_alive(dest_proc)) {
        kobject_release(src_obj); kobject_release(dest_obj);
        return (uint64_t)IRIS_ERR_BAD_HANDLE;
    }

    iris_rights_t new_rights = rights_reduce(src_rights, (iris_rights_t)arg2);
    handle_id_t   new_h      = handle_table_insert(&dest_proc->handle_table,
                                                   src_obj, new_rights);
    kobject_release(src_obj);
    kobject_release(dest_obj);

    if (new_h == HANDLE_INVALID) return (uint64_t)IRIS_ERR_TABLE_FULL;

    /* Consume the source handle — transfer is move, not copy */
    handle_table_close(&caller->process->handle_table, (handle_id_t)arg0);
    return (uint64_t)new_h;
}

/* ── Nameserver syscalls ──────────────────────────────────────────── */

/*
 * sys_ns_register(name_uptr, handle, rights) → error_code
 *
 * Registers the KObject behind handle in the nameserver under name.
 * rights is capped to what the caller holds on handle.
 */
static uint64_t sys_ns_register(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return (uint64_t)IRIS_ERR_INVALID_ARG;

    if (!user_ptr_valid(arg0, 1)) return (uint64_t)IRIS_ERR_INVALID_ARG;

    /* Copy name from user space */
    char name[NS_NAME_LEN];
    uint32_t i = 0;
    const char *uname = (const char *)(uintptr_t)arg0;
    while (i < NS_NAME_LEN - 1 && user_ptr_valid(arg0 + i, 1) && uname[i]) {
        name[i] = uname[i]; i++;
    }
    name[i] = '\0';
    if (i == 0) return (uint64_t)IRIS_ERR_INVALID_ARG;

    /* Look up the object to register */
    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg1,
                                             &obj, &rights);
    if (r != IRIS_OK) return (uint64_t)r;

    /* Cap registered rights to what the caller actually holds */
    iris_rights_t reg_rights = rights_reduce(rights, (iris_rights_t)arg2);
    r = ns_register(name, obj, reg_rights, t->process);
    kobject_release(obj);
    return (uint64_t)r;
}

/*
 * sys_ns_lookup(name_uptr, req_rights) → handle_id or error_code
 *
 * Looks up name in the nameserver; inserts a new handle into caller's table.
 * Returns the handle_id on success, or iris_error_t cast to uint64_t on failure.
 */
static uint64_t sys_ns_lookup(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t) return (uint64_t)(int64_t)IRIS_ERR_INVALID_ARG;

    if (!user_ptr_valid(arg0, 1)) return (uint64_t)(int64_t)IRIS_ERR_INVALID_ARG;

    /* Copy name from user space */
    char name[NS_NAME_LEN];
    uint32_t i = 0;
    const char *uname = (const char *)(uintptr_t)arg0;
    while (i < NS_NAME_LEN - 1 && user_ptr_valid(arg0 + i, 1) && uname[i]) {
        name[i] = uname[i]; i++;
    }
    name[i] = '\0';
    if (i == 0) return (uint64_t)(int64_t)IRIS_ERR_INVALID_ARG;

    handle_id_t h = ns_lookup(name, t, (iris_rights_t)arg1);
    if (h == HANDLE_INVALID) return (uint64_t)(int64_t)IRIS_ERR_NOT_FOUND;
    return (uint64_t)h;
}

static uint64_t sys_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    /* arg0 = ticks to sleep (at 100 Hz, 1 tick = 10ms) */
    if (arg0 == 0) return 0;
    scheduler_sleep_current(arg0);
    return 0;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,

                          uint64_t arg1, uint64_t arg2) {
    switch (num) {
        case SYS_WRITE:  return sys_write(arg0, arg1, arg2);
        case SYS_EXIT:   return sys_exit(arg0, arg1, arg2);
        case SYS_GETPID: return sys_getpid(arg0, arg1, arg2);
        case SYS_YIELD:  return sys_yield(arg0, arg1, arg2);
        case SYS_OPEN:   return sys_open(arg0, arg1, arg2);
        case SYS_READ:   return sys_read(arg0, arg1, arg2);
        case SYS_CLOSE:  return sys_close(arg0, arg1, arg2);
        case SYS_BRK:        return sys_brk(arg0, arg1, arg2);
        case SYS_SLEEP:      return sys_sleep(arg0, arg1, arg2);
        case SYS_CHAN_CREATE:  return sys_chan_create(arg0, arg1, arg2);
        case SYS_CHAN_SEND:    return sys_chan_send(arg0, arg1, arg2);
        case SYS_CHAN_RECV:    return sys_chan_recv(arg0, arg1, arg2);
        case SYS_HANDLE_CLOSE: return sys_handle_close(arg0, arg1, arg2);
        case SYS_VMO_CREATE:  return sys_vmo_create(arg0, arg1, arg2);
        case SYS_VMO_MAP:     return sys_vmo_map(arg0, arg1, arg2);
        case SYS_SPAWN:         return sys_spawn(arg0, arg1, arg2);
        case SYS_NOTIFY_CREATE: return sys_notify_create(arg0, arg1, arg2);
        case SYS_NOTIFY_SIGNAL: return sys_notify_signal(arg0, arg1, arg2);
        case SYS_NOTIFY_WAIT:   return sys_notify_wait(arg0, arg1, arg2);
        case SYS_HANDLE_DUP:    return sys_handle_dup(arg0, arg1, arg2);
        case SYS_HANDLE_TRANSFER: return sys_handle_transfer(arg0, arg1, arg2);
        case SYS_NS_REGISTER:     return sys_ns_register(arg0, arg1, arg2);
        case SYS_NS_LOOKUP:       return sys_ns_lookup(arg0, arg1, arg2);
        default:
            serial_write("[SYSCALL] unknown syscall=");
            serial_write_dec(num);
            serial_write("\n");
            return (uint64_t)-1;
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
