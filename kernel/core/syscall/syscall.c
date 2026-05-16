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
#include <iris/nc/kinitrdentry.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/scheduler.h>
#include <iris/usercopy.h>
#include <iris/futex.h>
#include <iris/diag.h>
#include <iris/initrd.h>
#include <iris/fb_info.h>

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

/* All syscall handlers use iris_error_t for capability/object paths. */

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

/*
 * task_has_kdebug_cap — check whether task t holds a KBootstrapCap with
 * IRIS_BOOTCAP_KDEBUG.  Scans the handle table under its spinlock so that
 * no entry can be closed while the check runs.  The table's own reference
 * prevents any live slot's object from being freed while used[i] is true.
 * Only init (which receives KDEBUG at boot) and handles it delegates can
 * pass this check; ordinary services without a bootstrap cap cannot.
 */
static int task_has_kdebug_cap(struct task *t) {
    if (!t || !t->process) return 0;
    HandleTable *ht = &t->process->handle_table;
    int found = 0;
    spinlock_lock(&ht->lock);
    for (uint32_t i = 0; i < HANDLE_TABLE_MAX && !found; i++) {
        if (!ht->used[i]) continue;
        struct KObject *obj = ht->slots[i].object;
        if (!obj || obj->type != KOBJ_BOOTSTRAP_CAP) continue;
        if (kbootcap_allows((struct KBootstrapCap *)obj, IRIS_BOOTCAP_KDEBUG))
            found = 1;
    }
    spinlock_unlock(&ht->lock);
    return found;
}

/* SYS_WRITE retired in Phase 30: serial output moved to ring-3 console service. */
static uint64_t sys_write(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    return syscall_err(IRIS_ERR_NOT_SUPPORTED);
}

static uint64_t sys_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
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
    iris_error_t r = kchannel_bind_owner(ch, t->process);
    if (r != IRIS_OK) {
        kchannel_free(ch);
        return syscall_err(r);
    }
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
    msg.sender_id = t->id; /* kernel stamps verified sender; ring-3 cannot forge */

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

    map_size = (v->size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);

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

    kprocess_unregister_vmo_map(t->process, vaddr);
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
    iris_error_t r = knotification_bind_owner(n, t->process);
    if (r != IRIS_OK) {
        knotification_free(n);
        return syscall_err(r);
    }
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

    task_kill_process(target);
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
 * caller-supplied user buffer.  Requires IRIS_BOOTCAP_KDEBUG.
 *
 * arg0: user pointer to a buffer of at least IRIS_DIAG_SNAPSHOT_SIZE (64) bytes.
 *       Must be writable user-space memory; validated before any kernel reads.
 *
 * The snapshot is built in a local kernel-stack struct and copied to user
 * space atomically via copy_to_user_checked.  Callers must verify
 * snapshot.magic == IRIS_DIAG_MAGIC and snapshot.version == IRIS_DIAG_VERSION
 * before reading any other field.
 *
 * Returns IRIS_OK (0) on success, IRIS_ERR_ACCESS_DENIED if caller lacks
 * KDEBUG cap, IRIS_ERR_INVALID_ARG if the buffer pointer is invalid.
 */
static uint64_t sys_diag_snapshot(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);

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
    msg.sender_id = t->id; /* kernel stamps verified sender; ring-3 cannot forge */
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

/* ── Hardware capability creation (C2: policy moved to svcmgr) ──────── */

static uint64_t sys_cap_create_irqcap(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t    cap_h   = (handle_id_t)arg0;
    uint8_t        irq_num = (uint8_t)(arg1 & 0xFFu);
    struct task   *t       = task_current();
    struct KObject *auth   = 0;
    iris_rights_t   auth_r;
    struct KIrqCap *irqcap;
    handle_id_t     out_h;
    (void)arg2;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (irq_num > 15u)     return syscall_err(IRIS_ERR_INVALID_ARG);

    if (handle_table_get_object(&t->process->handle_table, cap_h, &auth, &auth_r) != IRIS_OK)
        return syscall_err(IRIS_ERR_NOT_FOUND);
    if (auth->type != KOBJ_BOOTSTRAP_CAP ||
        !kbootcap_allows((struct KBootstrapCap *)auth, IRIS_BOOTCAP_HW_ACCESS)) {
        kobject_release(auth);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth);

    irqcap = kirqcap_alloc(irq_num);
    if (!irqcap) return syscall_err(IRIS_ERR_NO_MEMORY);

    out_h = handle_table_insert(&t->process->handle_table, &irqcap->base, RIGHT_ROUTE);
    if (out_h == HANDLE_INVALID) {
        kirqcap_free(irqcap);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&irqcap->base);
    return syscall_ok_u64((uint64_t)out_h);
}

/*
 * Whitelist of port ranges ring-3 services may claim via SYS_CAP_CREATE_IOPORT.
 * A request must fall entirely within one entry ([base, base+count)).
 * The kernel itself owns PIC/PIT; those are not listed here.
 */
static const struct { uint16_t base; uint16_t count; } kioport_whitelist[] = {
    { 0x0060u, 5u },   /* PS/2: data(0x60) + status/cmd(0x64) */
    { 0x02F8u, 8u },   /* COM2 serial */
    { 0x03F8u, 8u },   /* COM1 serial */
    { 0x0600u, 8u },   /* QEMU ACPI power management (0x604 poweroff) */
};

static int kioport_in_whitelist(uint16_t base, uint16_t count) {
    uint32_t req_end = (uint32_t)base + (uint32_t)count;
    for (uint32_t i = 0; i < sizeof(kioport_whitelist)/sizeof(kioport_whitelist[0]); i++) {
        uint32_t wl_end = (uint32_t)kioport_whitelist[i].base + (uint32_t)kioport_whitelist[i].count;
        if ((uint32_t)base >= (uint32_t)kioport_whitelist[i].base && req_end <= wl_end)
            return 1;
    }
    return 0;
}

static uint64_t sys_cap_create_ioport(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    handle_id_t    cap_h = (handle_id_t)arg0;
    uint16_t       base  = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t       count = (uint16_t)(arg2 & 0xFFFFu);
    struct task   *t     = task_current();
    struct KObject *auth = 0;
    iris_rights_t   auth_r;
    struct KIoPort *ioport;
    handle_id_t     out_h;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (count == 0u || (uint32_t)base + count > 0x10000u)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!kioport_in_whitelist(base, count))
        return syscall_err(IRIS_ERR_ACCESS_DENIED);

    if (handle_table_get_object(&t->process->handle_table, cap_h, &auth, &auth_r) != IRIS_OK)
        return syscall_err(IRIS_ERR_NOT_FOUND);
    if (auth->type != KOBJ_BOOTSTRAP_CAP ||
        !kbootcap_allows((struct KBootstrapCap *)auth, IRIS_BOOTCAP_HW_ACCESS)) {
        kobject_release(auth);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    kobject_release(auth);

    ioport = kioport_alloc(base, count);
    if (!ioport) return syscall_err(IRIS_ERR_NO_MEMORY);

    out_h = handle_table_insert(&t->process->handle_table, &ioport->base,
                                RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (out_h == HANDLE_INVALID) {
        kioport_free(ioport);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&ioport->base);
    return syscall_ok_u64((uint64_t)out_h);
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
static uint64_t sys_initrd_vmo(uint64_t arg0, uint64_t arg1,
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
static uint64_t sys_initrd_count(uint64_t arg0, uint64_t arg1,
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
 * sys_clock_get() → uint64_t nanoseconds since boot or iris_error_t
 *
 * Returns a monotonically increasing timestamp derived from the scheduler tick
 * counter (100 Hz; 10 ms resolution).  No capability required — any task may
 * query.  Does not block.
 */
static uint64_t sys_clock_get(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    /* wall_ticks is incremented only by the real PIT ISR; it is never
     * fast-forwarded by the idle-loop clock workaround in task_yield. */
    return syscall_ok_u64(sched_wall_ticks() * 10000000ULL);
}

/*
 * sys_chan_recv_timeout(chan_h, msg_uptr, timeout_ns) → 0 or iris_error_t
 *
 * Identical to SYS_CHAN_RECV but returns IRIS_ERR_TIMED_OUT (-15) if no
 * message arrives within timeout_ns nanoseconds.  Resolution: 10 ms (one
 * scheduler tick at 100 Hz).
 */
static uint64_t sys_chan_recv_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
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

    /* Convert timeout_ns to an absolute tick deadline.  Round up by +1 tick so
     * a caller requesting 10 ms gets at least one full tick of wait time. */
    uint64_t timeout_ns = arg2;
    uint64_t deadline_ticks = sched_current_ticks() + timeout_ns / 10000000ULL + 1u;

    struct KChanMsg msg;
    r = kchannel_recv_timeout_into_process((struct KChannel *)obj, t->process, &msg, deadline_ticks);
    kobject_release(obj);
    if (r == IRIS_OK && !copy_to_user_checked(arg1, &msg, (uint32_t)sizeof(msg)))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    return syscall_err(r);
}

/*
 * sys_notify_wait_timeout(notify_h, bits_uptr, timeout_ns) → 0 or iris_error_t
 *
 * Identical to SYS_NOTIFY_WAIT but returns IRIS_ERR_TIMED_OUT (-15) if no
 * signal arrives within timeout_ns nanoseconds.
 */
static uint64_t sys_notify_wait_timeout(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg1, (uint32_t)sizeof(uint64_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject  *obj;
    iris_rights_t    rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0,
                                             &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_NOTIFICATION) { kobject_release(obj); return syscall_err(IRIS_ERR_WRONG_TYPE); }
    if (!rights_check(rights, RIGHT_WAIT)) { kobject_release(obj); return syscall_err(IRIS_ERR_ACCESS_DENIED); }

    uint64_t timeout_ns = arg2;
    uint64_t deadline_ticks = sched_current_ticks() + timeout_ns / 10000000ULL + 1u;

    struct KNotification *notif = (struct KNotification *)obj;
    uint64_t bits = 0;
    r = knotification_wait_timeout(notif, &bits, deadline_ticks);
    kobject_release(obj);
    if (r == IRIS_OK) {
        if (!copy_to_user_checked(arg1, &bits, (uint32_t)sizeof(bits)))
            return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    return syscall_err(r);
}

/*
 * sys_process_create() → proc_handle or iris_error_t
 *
 * Allocates a new empty KProcess with a fresh user address space (new CR3).
 * No threads are created.  The caller uses sys_vmo_map_into to populate the
 * address space and sys_thread_start to launch the first thread.
 */
static uint64_t sys_process_create(uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KProcess *proc = kprocess_alloc();
    if (!proc) return syscall_err(IRIS_ERR_NO_MEMORY);

    proc->cr3 = paging_create_user_space();
    if (!proc->cr3) {
        kprocess_free(proc);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    handle_id_t h = handle_table_insert(&t->process->handle_table, &proc->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE |
                                        RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_ROUTE);
    /* Do NOT kobject_release: initial ref is the thread-lifecycle reference,
     * released by reap_dead_task_off_cpu → kprocess_free when last thread exits. */
    if (h == HANDLE_INVALID) {
        kprocess_free(proc);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    return syscall_ok_u64((uint64_t)h);
}

/*
 * sys_vmo_map_into(vmo_h, proc_h, vaddr, flags) → 0 or iris_error_t
 *
 * Maps VMO pages into a target process's address space.  Requires RIGHT_READ
 * (+ RIGHT_WRITE if writable) on vmo_h and RIGHT_MANAGE on proc_h.
 * W^X enforced: WRITABLE + EXEC simultaneously → ERR_INVALID_ARG.
 * vaddr must be page-aligned within [USER_PRIVATE_BASE, USER_STACK_TOP).
 */
static uint64_t sys_vmo_map_into(uint64_t arg0, uint64_t arg1,
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

    uint64_t vaddr = arg2;
    if (v->size == 0 || (vaddr & (PAGE_SIZE - 1ULL)) != 0 ||
        vaddr < USER_PRIVATE_BASE) {
        kobject_release(vmo_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }
    uint64_t map_size = (v->size + (PAGE_SIZE - 1ULL)) & ~(PAGE_SIZE - 1ULL);
    if (vaddr + map_size < vaddr || vaddr + map_size > USER_STACK_TOP) {
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

/*
 * sys_thread_start(proc_h, entry_vaddr, stack_top, boot_arg) → 0 or iris_error_t
 *
 * Creates a new ring-3 thread in the process identified by proc_h.
 * entry_vaddr and stack_top must be within user space and stack_top 8-byte aligned.
 * boot_arg is delivered in RBX on first execution.
 * Requires RIGHT_MANAGE on proc_h.
 */
static uint64_t sys_thread_start(uint64_t arg0, uint64_t arg1,
                                 uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint64_t entry_vaddr = arg1;
    uint64_t user_rsp    = arg2;

    if (entry_vaddr < USER_SPACE_BASE || entry_vaddr >= USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp < USER_SPACE_BASE || user_rsp > USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp & 0x7ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_MANAGE)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *proc = (struct KProcess *)proc_obj;
    /* Accept fresh (thread_count=0) processes that haven't been torn down. */
    if (kprocess_teardown_complete(proc)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    struct task *nt = task_thread_create(proc, entry_vaddr, user_rsp, arg3);
    kobject_release(proc_obj);
    if (!nt) return syscall_err(IRIS_ERR_NO_MEMORY);
    return syscall_ok_u64(0);
}

/*
 * sys_handle_insert(proc_h, obj_h, rights, _) → new_handle_id or iris_error_t
 *
 * Copies obj_h into the target process's handle table with the specified rights
 * (a subset of obj_h's current rights).  Requires RIGHT_MANAGE on proc_h and
 * RIGHT_DUPLICATE on obj_h.  The source handle is NOT consumed.
 * Returns the new handle_id assigned in the target process.
 */
static uint64_t sys_handle_insert(uint64_t arg0, uint64_t arg1,
                                  uint64_t arg2, uint64_t arg3) {
    (void)arg3;
    struct task *caller = task_current();
    if (!caller || !caller->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *proc_obj;
    iris_rights_t   proc_rights;
    iris_error_t r = handle_table_get_object(&caller->process->handle_table,
                                             (handle_id_t)arg0, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_MANAGE)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KProcess *proc = (struct KProcess *)proc_obj;
    /* Accept fresh (thread_count=0) processes that haven't been torn down. */
    if (kprocess_teardown_complete(proc)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_BAD_HANDLE);
    }

    struct KObject *src_obj;
    iris_rights_t   src_rights;
    r = handle_table_get_object(&caller->process->handle_table,
                                (handle_id_t)arg1, &src_obj, &src_rights);
    if (r != IRIS_OK) { kobject_release(proc_obj); return syscall_err(r); }
    if (!rights_check(src_rights, RIGHT_DUPLICATE)) {
        kobject_release(src_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    iris_rights_t new_rights = rights_reduce(src_rights, (iris_rights_t)arg2);
    if (new_rights == RIGHT_NONE) {
        kobject_release(src_obj); kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    handle_id_t new_h = handle_table_insert(&proc->handle_table, src_obj, new_rights);
    kobject_release(src_obj);
    kobject_release(proc_obj);

    if (new_h == HANDLE_INVALID) return syscall_err(IRIS_ERR_TABLE_FULL);
    return syscall_ok_u64((uint64_t)new_h);
}

/* ── I/O port sub-delegation (A4) ───────────────────────────────────── */

/*
 * sys_ioport_restrict(ioport_h, offset, count) → new_handle or iris_error_t
 *
 * Creates a narrower KIoPort from an existing one.  Requires RIGHT_READ |
 * RIGHT_DUPLICATE on ioport_h.  offset + count must fit within the parent range.
 * The derived cap is granted READ|WRITE|DUPLICATE|TRANSFER so it can do both IN and OUT.
 */
static uint64_t sys_ioport_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t   rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_IOPORT) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ | RIGHT_DUPLICATE)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KIoPort *parent = (struct KIoPort *)obj;
    uint16_t offset = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t count  = (uint16_t)(arg2 & 0xFFFFu);

    if (count == 0 || (uint32_t)offset + count > (uint32_t)parent->count) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_INVALID_ARG);
    }

    uint16_t new_base = (uint16_t)(parent->base_port + offset);
    kobject_release(obj);

    struct KIoPort *sub = kioport_alloc(new_base, count);
    if (!sub) return syscall_err(IRIS_ERR_NO_MEMORY);

    handle_id_t h = handle_table_insert(&t->process->handle_table, &sub->base,
                                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        kioport_free(sub);
        return syscall_err(IRIS_ERR_TABLE_FULL);
    }
    kobject_release(&sub->base);
    return syscall_ok_u64((uint64_t)h);
}

/* ── Multi-channel readable wait (A5) ───────────────────────────────── */

#define WAIT_ANY_MAX_CHANNELS 64u

/*
 * sys_wait_any(handles_uptr, count, out_index_uptr) → 0 or iris_error_t
 *
 * Blocks until at least one of the watched KChannels has a pending message,
 * then writes its 0-based index to *out_index_uptr and returns 0.
 * Does NOT consume the message — caller follows up with CHAN_RECV / CHAN_RECV_NB.
 */
static uint64_t sys_wait_any(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint32_t count = (uint32_t)(arg1 & 0xFFu);
    if (count == 0 || count > WAIT_ANY_MAX_CHANNELS)
        return syscall_err(IRIS_ERR_INVALID_ARG);

    if (!user_range_readable(arg0, (uint32_t)(count * sizeof(handle_id_t))))
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (!user_range_writable(arg2, (uint32_t)sizeof(uint32_t)))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    handle_id_t handles[WAIT_ANY_MAX_CHANNELS];
    if (!copy_from_user_checked(handles, arg0, (uint32_t)(count * sizeof(handle_id_t))))
        return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KChannel *chans[WAIT_ANY_MAX_CHANNELS];
    for (uint32_t i = 0; i < count; i++) chans[i] = 0;

    for (uint32_t i = 0; i < count; i++) {
        struct KObject *obj;
        iris_rights_t   rights;
        iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                                 handles[i], &obj, &rights);
        if (r != IRIS_OK) {
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(r);
        }
        if (obj->type != KOBJ_CHANNEL) {
            kobject_release(obj);
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_WRONG_TYPE);
        }
        if (!rights_check(rights, RIGHT_READ)) {
            kobject_release(obj);
            for (uint32_t j = 0; j < i; j++) kobject_release(&chans[j]->base);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
        chans[i] = (struct KChannel *)obj;
    }

    /* Phase 1: non-blocking scan */
    for (uint32_t i = 0; i < count; i++) {
        if (kchannel_is_readable(chans[i])) {
            uint32_t idx = i;
            for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
            if (!copy_to_user_checked(arg2, &idx, (uint32_t)sizeof(idx)))
                return syscall_err(IRIS_ERR_INVALID_ARG);
            return syscall_ok_u64(0);
        }
    }

    /* Phase 2: enqueue on all, yield, retry */
    for (;;) {
        t->state = TASK_BLOCKED_IPC;
        for (uint32_t i = 0; i < count; i++) {
            iris_error_t r = kchannel_waiters_add_or_closed(chans[i], t);
            if (r == IRIS_ERR_CLOSED) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j <= i; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
            if (r != IRIS_OK) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j <= i; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(r);
            }
        }

        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_readable(chans[i])) {
                uint32_t idx = i;
                t->state = TASK_READY;
                for (uint32_t j = 0; j < count; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                if (!copy_to_user_checked(arg2, &idx, (uint32_t)sizeof(idx)))
                    return syscall_err(IRIS_ERR_INVALID_ARG);
                return syscall_ok_u64(0);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (chans[i]->closed) {
                t->state = TASK_READY;
                for (uint32_t j = 0; j < count; j++)
                    kchannel_waiters_remove_task(chans[j], t);
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }
        task_yield();

        for (uint32_t i = 0; i < count; i++)
            kchannel_waiters_remove_task(chans[i], t);

        for (uint32_t i = 0; i < count; i++) {
            if (kchannel_is_readable(chans[i])) {
                uint32_t idx = i;
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                if (!copy_to_user_checked(arg2, &idx, (uint32_t)sizeof(idx)))
                    return syscall_err(IRIS_ERR_INVALID_ARG);
                return syscall_ok_u64(0);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            if (chans[i]->closed) {
                for (uint32_t j = 0; j < count; j++) kobject_release(&chans[j]->base);
                return syscall_err(IRIS_ERR_CLOSED);
            }
        }
        /* spurious wakeup — retry */
    }
}

/* ── B3: bootstrap cap permission restriction ─────────────────────── */

static uint64_t sys_bootcap_restrict(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *obj;
    iris_rights_t rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (obj->type != KOBJ_BOOTSTRAP_CAP) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(rights, RIGHT_READ)) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }
    struct KBootstrapCap *restricted =
        kbootcap_clone_restricted((struct KBootstrapCap *)obj, (uint32_t)arg1);
    if (!restricted) {
        kobject_release(obj);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    r = handle_table_replace(&t->process->handle_table, (handle_id_t)arg0, &restricted->base);
    kobject_release(&restricted->base);
    kobject_release(obj);
    if (r != IRIS_OK) return syscall_err(r);
    return syscall_ok_u64(0);
}

/* ── B4: VMO inter-process share ──────────────────────────────────── */

static uint64_t sys_vmo_share(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
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

/* ── B5: exception handler registration ───────────────────────────── */

static uint64_t sys_exception_handler(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KObject *proc_obj;
    iris_rights_t proc_rights;
    iris_error_t r = handle_table_get_object(&t->process->handle_table,
                                             (handle_id_t)arg0, &proc_obj, &proc_rights);
    if (r != IRIS_OK) return syscall_err(r);
    if (proc_obj->type != KOBJ_PROCESS) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_WRONG_TYPE);
    }
    if (!rights_check(proc_rights, RIGHT_MANAGE)) {
        kobject_release(proc_obj);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    struct KObject *chan_obj;
    iris_rights_t chan_rights;
    r = handle_table_get_object(&t->process->handle_table,
                                (handle_id_t)arg1, &chan_obj, &chan_rights);
    if (r != IRIS_OK) { kobject_release(proc_obj); return syscall_err(r); }
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

    r = kprocess_set_exception_handler((struct KProcess *)proc_obj,
                                       (struct KChannel *)chan_obj);
    kobject_release(proc_obj);
    kobject_release(chan_obj);
    return syscall_err(r);
}

/* ── Threading (D2) ──────────────────────────────────────────────── */

static uint64_t sys_thread_create(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    struct task *t = task_current();
    uint64_t entry_vaddr = arg0;
    uint64_t user_rsp    = arg1;
    uint64_t arg         = arg2;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (entry_vaddr < USER_SPACE_BASE || entry_vaddr >= USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp < USER_SPACE_BASE || user_rsp > USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (user_rsp & 0x7ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);  /* must be 8-byte aligned */

    struct task *nt = task_thread_create(t->process, entry_vaddr, user_rsp, arg);
    if (!nt) return syscall_err(IRIS_ERR_NO_MEMORY);
    return syscall_ok_u64((uint64_t)nt->id);
}

static uint64_t sys_thread_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    task_exit_current();
    return 0;  /* unreachable */
}

static uint64_t sys_handle_type(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg1; (void)arg2;
    struct task *t = task_current();
    struct KObject *obj;
    iris_rights_t rights;
    iris_error_t r;
    uint64_t type;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);
    r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0, &obj, &rights);
    if (r != IRIS_OK) return syscall_err(r);
    (void)rights;
    type = (uint64_t)obj->type;
    kobject_release(obj);
    return syscall_ok_u64(type);
}

static uint64_t sys_handle_same_object(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    struct task *t = task_current();
    struct KObject *obj_a;
    struct KObject *obj_b;
    iris_rights_t rights_a;
    iris_rights_t rights_b;
    iris_error_t r;
    uint64_t same;

    if (!t || !t->process) return syscall_err(IRIS_ERR_INVALID_ARG);

    r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg0, &obj_a, &rights_a);
    if (r != IRIS_OK) return syscall_err(r);
    r = handle_table_get_object(&t->process->handle_table, (handle_id_t)arg1, &obj_b, &rights_b);
    if (r != IRIS_OK) {
        kobject_release(obj_a);
        return syscall_err(r);
    }
    (void)rights_a;
    (void)rights_b;
    same = (obj_a == obj_b) ? 1u : 0u;
    kobject_release(obj_b);
    kobject_release(obj_a);
    return syscall_ok_u64(same);
}

/* ── Futex (D3) ──────────────────────────────────────────────────── */

static uint64_t sys_futex_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    uint64_t uaddr    = arg0;
    uint32_t expected = (uint32_t)arg1;

    if (uaddr < USER_SPACE_BASE || uaddr >= USER_SPACE_TOP)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (uaddr & 0x3ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);  /* must be 4-byte aligned */

    return syscall_err(futex_wait(uaddr, expected));
}

static uint64_t sys_futex_wake(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg2;
    uint64_t uaddr = arg0;
    uint32_t count = (uint32_t)arg1;

    if (uaddr & 0x3ULL)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if (count == 0) return syscall_ok_u64(0);

    return syscall_ok_u64((uint64_t)futex_wake(uaddr, count));
}

/*
 * sys_framebuffer_vmo(auth_h, info_uptr) → vmo_handle or iris_error_t
 *
 * Claims the physical framebuffer as an MMIO VMO (one-shot).  Requires a
 * KBootstrapCap with IRIS_BOOTCAP_FRAMEBUFFER.  Writes struct iris_fb_params to
 * info_uptr in user space, then returns a non-owning KVmo handle.
 * Clears g_iris_fb_params_valid so the framebuffer can only be claimed once.
 */
static uint64_t sys_framebuffer_vmo(uint64_t arg0, uint64_t arg1,
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

    /* One-shot: clear immediately so no second caller can claim it. */
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

/*
 * sys_poweroff — privileged machine halt.  Requires KDEBUG cap.
 *
 * Hardware power-off (ACPI S5, ISA debug exit) is intentionally NOT performed
 * here.  Ring-3 processes that need ACPI shutdown create a KIoPort cap for
 * 0x0604 via SYS_CAP_CREATE_IOPORT and issue the write themselves via
 * SYS_IOPORT_OUT.  The kernel owns no ACPI state after early boot.
 *
 * arg0 is reserved for future use (ignored).
 */
static uint64_t sys_poweroff(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    (void)arg0; (void)arg1; (void)arg2;
    struct task *t = task_current();
    if (!t || !task_has_kdebug_cap(t)) return syscall_err(IRIS_ERR_ACCESS_DENIED);
    for (;;) __asm__ volatile ("hlt");
    return syscall_ok_u64(0); /* unreachable */
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2, uint64_t arg3) {
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
        case SYS_SPAWN_SERVICE: return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 22 */
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
        case SYS_CHAN_CALL:            return sys_chan_call(arg0, arg1, arg2);
        case SYS_CAP_CREATE_IRQCAP:   return sys_cap_create_irqcap(arg0, arg1, arg2);
        case SYS_CAP_CREATE_IOPORT:   return sys_cap_create_ioport(arg0, arg1, arg2);
        case SYS_INITRD_LOOKUP: return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 29 */
        case SYS_SPAWN_ELF:     return syscall_err(IRIS_ERR_NOT_SUPPORTED); /* retired Phase 29 */
        case SYS_IOPORT_RESTRICT:      return sys_ioport_restrict(arg0, arg1, arg2);
        case SYS_WAIT_ANY:             return sys_wait_any(arg0, arg1, arg2);
        case SYS_BOOTCAP_RESTRICT:     return sys_bootcap_restrict(arg0, arg1, arg2);
        case SYS_VMO_SHARE:            return sys_vmo_share(arg0, arg1, arg2);
        case SYS_EXCEPTION_HANDLER:    return sys_exception_handler(arg0, arg1, arg2);
        case SYS_THREAD_CREATE:        return sys_thread_create(arg0, arg1, arg2);
        case SYS_THREAD_EXIT:          return sys_thread_exit(arg0, arg1, arg2);
        case SYS_FUTEX_WAIT:           return sys_futex_wait(arg0, arg1, arg2);
        case SYS_FUTEX_WAKE:           return sys_futex_wake(arg0, arg1, arg2);
        case SYS_HANDLE_TYPE:          return sys_handle_type(arg0, arg1, arg2);
        case SYS_HANDLE_SAME_OBJECT:   return sys_handle_same_object(arg0, arg1, arg2);
        case SYS_POWEROFF:             return sys_poweroff(arg0, arg1, arg2);
        case SYS_INITRD_VMO:    return sys_initrd_vmo(arg0, arg1, arg2, arg3);
        case SYS_INITRD_COUNT:  return sys_initrd_count(arg0, arg1, arg2, arg3);
        case SYS_PROCESS_CREATE: return sys_process_create(arg0, arg1, arg2, arg3);
        case SYS_VMO_MAP_INTO:  return sys_vmo_map_into(arg0, arg1, arg2, arg3);
        case SYS_THREAD_START:  return sys_thread_start(arg0, arg1, arg2, arg3);
        case SYS_HANDLE_INSERT: return sys_handle_insert(arg0, arg1, arg2, arg3);
        case SYS_FRAMEBUFFER_VMO: return sys_framebuffer_vmo(arg0, arg1, arg2, arg3);
        case SYS_CLOCK_GET:           return sys_clock_get(arg0, arg1, arg2);
        case SYS_CHAN_RECV_TIMEOUT:   return sys_chan_recv_timeout(arg0, arg1, arg2);
        case SYS_NOTIFY_WAIT_TIMEOUT: return sys_notify_wait_timeout(arg0, arg1, arg2);
        default:
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
