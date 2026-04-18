#include <iris/nc/kprocess.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/kvmo.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/syscall.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/fault_proto.h>
#include <stdint.h>

static struct KProcess pool[KPROCESS_POOL_SIZE];
static uint8_t         pool_used[KPROCESS_POOL_SIZE];

static void kprocess_clear_exception_chan(struct KProcess *p) {
    struct KChannel *old = 0;
    if (!p) return;

    spinlock_lock(&p->base.lock);
    old = p->exception_chan;
    p->exception_chan = 0;
    spinlock_unlock(&p->base.lock);

    if (!old) return;
    kobject_active_release(&old->base);
    kobject_release(&old->base);
}

static void kprocess_clear_exit_watch(struct KProcess *p) {
    if (!p) return;
    for (uint32_t i = 0; i < KPROCESS_EXIT_WATCH_MAX; i++) {
        struct KExitWatch *w = &p->exit_watches[i];
        if (!w->armed) continue;
        kobject_release(&w->ch->base);
        w->ch = 0;
        w->armed = 0;
    }
}

static void kprocess_emit_exit_watch(struct KProcess *p) {
    if (!p) return;
    for (uint32_t i = 0; i < KPROCESS_EXIT_WATCH_MAX; i++) {
        struct KExitWatch *w = &p->exit_watches[i];
        struct KChanMsg msg;
        if (!w->armed || !w->ch) continue;
        for (uint32_t j = 0; j < sizeof(msg); j++) ((uint8_t *)&msg)[j] = 0;
        msg.type = PROC_EVENT_MSG_EXIT;
        msg.data[PROC_EVENT_OFF_HANDLE + 0] = (uint8_t)(w->watched_handle & 0xFFu);
        msg.data[PROC_EVENT_OFF_HANDLE + 1] = (uint8_t)((w->watched_handle >> 8) & 0xFFu);
        msg.data[PROC_EVENT_OFF_HANDLE + 2] = (uint8_t)((w->watched_handle >> 16) & 0xFFu);
        msg.data[PROC_EVENT_OFF_HANDLE + 3] = (uint8_t)((w->watched_handle >> 24) & 0xFFu);
        msg.data[PROC_EVENT_OFF_COOKIE + 0] = (uint8_t)(w->cookie & 0xFFu);
        msg.data[PROC_EVENT_OFF_COOKIE + 1] = (uint8_t)((w->cookie >> 8) & 0xFFu);
        msg.data[PROC_EVENT_OFF_COOKIE + 2] = (uint8_t)((w->cookie >> 16) & 0xFFu);
        msg.data[PROC_EVENT_OFF_COOKIE + 3] = (uint8_t)((w->cookie >> 24) & 0xFFu);
        msg.data_len = PROC_EVENT_MSG_LEN;
        msg.attached_handle = HANDLE_INVALID;
        msg.attached_rights = RIGHT_NONE;
        (void)kchannel_send(w->ch, &msg);
    }
}

static void kprocess_destroy(struct KObject *obj) {
    struct KProcess *p = (struct KProcess *)obj;

    /* Final refcount drop: finish only idempotent process-owned cleanup.
     * Task-local resources must already be gone before this point. */
    if (!p->teardown_complete) {
        kprocess_teardown(p, 0);
    }
    if (!p->aspace_reaped) {
        kprocess_reap_address_space(p);
    }

    for (int i = 0; i < KPROCESS_POOL_SIZE; i++) {
        if (&pool[i] == p) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kprocess_ops = {
    .destroy = kprocess_destroy
};

struct KProcess *kprocess_alloc(void) {
    for (int i = 0; i < KPROCESS_POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KProcess *p = &pool[i];
            uint8_t *raw = (uint8_t *)p;
            for (uint32_t j = 0; j < sizeof(*p); j++) raw[j] = 0;
            kobject_init(&p->base, KOBJ_PROCESS, &kprocess_ops);
            handle_table_init(&p->handle_table);
            return p;
        }
    }
    return 0;
}

void kprocess_free(struct KProcess *p) {
    kobject_release(&p->base);
}

iris_error_t kprocess_watch_exit(struct KProcess *p, struct KChannel *ch,
                                 handle_id_t watched_handle, uint32_t cookie) {
    if (!p || !ch || watched_handle == HANDLE_INVALID) return IRIS_ERR_INVALID_ARG;

    uint32_t slot = KPROCESS_EXIT_WATCH_MAX;
    for (uint32_t i = 0; i < KPROCESS_EXIT_WATCH_MAX; i++) {
        if (!p->exit_watches[i].armed) { slot = i; break; }
    }
    if (slot == KPROCESS_EXIT_WATCH_MAX) return IRIS_ERR_TABLE_FULL;

    kobject_retain(&ch->base);
    p->exit_watches[slot].ch = ch;
    p->exit_watches[slot].watched_handle = watched_handle;
    p->exit_watches[slot].cookie = cookie;
    p->exit_watches[slot].armed = 1;

    if (!kprocess_is_alive(p)) {
        kprocess_emit_exit_watch(p);
        kprocess_clear_exit_watch(p);
    }
    return IRIS_OK;
}

iris_error_t kprocess_set_exception_handler(struct KProcess *p, struct KChannel *ch) {
    struct KChannel *old;
    if (!p || !ch) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&p->base.lock);
    if (p->exception_chan == ch) {
        spinlock_unlock(&p->base.lock);
        return IRIS_OK;
    }
    spinlock_unlock(&p->base.lock);

    kobject_retain(&ch->base);
    kobject_active_retain(&ch->base);

    spinlock_lock(&p->base.lock);
    if (p->exception_chan == ch) {
        spinlock_unlock(&p->base.lock);
        kobject_active_release(&ch->base);
        kobject_release(&ch->base);
        return IRIS_OK;
    }
    old = p->exception_chan;
    p->exception_chan = ch;
    spinlock_unlock(&p->base.lock);

    if (old) {
        kobject_active_release(&old->base);
        kobject_release(&old->base);
    }
    return IRIS_OK;
}

void kprocess_notify_fault(struct task *t, uint64_t vector,
                            uint64_t error_code, uint64_t rip, uint64_t cr2) {
    struct KChanMsg msg;
    struct KProcess *p;
    struct KChannel *ch;
    int i;

    if (!t || !t->process) return;
    p = t->process;

    spinlock_lock(&p->base.lock);
    ch = p->exception_chan;
    if (ch) kobject_retain(&ch->base);
    spinlock_unlock(&p->base.lock);
    if (!ch) return;

    for (i = 0; i < (int)sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
    msg.type = FAULT_MSG_NOTIFY;
    msg.data[FAULT_OFF_VECTOR + 0] = (uint8_t)(vector & 0xFFu);
    msg.data[FAULT_OFF_VECTOR + 1] = (uint8_t)((vector >> 8) & 0xFFu);
    msg.data[FAULT_OFF_VECTOR + 2] = (uint8_t)((vector >> 16) & 0xFFu);
    msg.data[FAULT_OFF_VECTOR + 3] = (uint8_t)((vector >> 24) & 0xFFu);
    msg.data[FAULT_OFF_TASK_ID + 0] = (uint8_t)(t->id & 0xFFu);
    msg.data[FAULT_OFF_TASK_ID + 1] = (uint8_t)((t->id >> 8) & 0xFFu);
    msg.data[FAULT_OFF_TASK_ID + 2] = (uint8_t)((t->id >> 16) & 0xFFu);
    msg.data[FAULT_OFF_TASK_ID + 3] = (uint8_t)((t->id >> 24) & 0xFFu);
    for (i = 0; i < 8; i++) msg.data[FAULT_OFF_RIP + i] = (uint8_t)((rip >> (i * 8)) & 0xFFu);
    msg.data[FAULT_OFF_ERROR + 0] = (uint8_t)(error_code & 0xFFu);
    msg.data[FAULT_OFF_ERROR + 1] = (uint8_t)((error_code >> 8) & 0xFFu);
    msg.data[FAULT_OFF_ERROR + 2] = (uint8_t)((error_code >> 16) & 0xFFu);
    msg.data[FAULT_OFF_ERROR + 3] = (uint8_t)((error_code >> 24) & 0xFFu);
    for (i = 0; i < 8; i++) msg.data[FAULT_OFF_CR2 + i] = (uint8_t)((cr2 >> (i * 8)) & 0xFFu);
    msg.data_len = FAULT_MSG_LEN;
    msg.attached_handle = HANDLE_INVALID;
    msg.attached_rights = RIGHT_NONE;
    (void)kchannel_send(ch, &msg);
    kobject_release(&ch->base);
}

iris_error_t kprocess_register_vmo_map(struct KProcess *p, uint64_t virt_base,
                                        uint64_t size, struct KVmo *vmo,
                                        uint64_t page_flags) {
    uint32_t i;
    if (!p || !vmo || size == 0) return IRIS_ERR_INVALID_ARG;
    for (i = 0; i < KPROCESS_VMO_MAP_MAX; i++) {
        if (p->vmo_mappings[i].vmo) continue;
        p->vmo_mappings[i].virt_base   = virt_base;
        p->vmo_mappings[i].size        = size;
        p->vmo_mappings[i].vmo         = vmo;
        p->vmo_mappings[i].page_flags  = page_flags;
        kobject_retain(&vmo->base);
        return IRIS_OK;
    }
    return IRIS_ERR_TABLE_FULL;
}

void kprocess_unregister_vmo_map(struct KProcess *p, uint64_t virt_base) {
    uint32_t i;
    if (!p) return;
    for (i = 0; i < KPROCESS_VMO_MAP_MAX; i++) {
        if (!p->vmo_mappings[i].vmo) continue;
        if (p->vmo_mappings[i].virt_base != virt_base) continue;
        kobject_release(&p->vmo_mappings[i].vmo->base);
        p->vmo_mappings[i].vmo = 0;
        p->vmo_mappings[i].virt_base = 0;
        p->vmo_mappings[i].size = 0;
        p->vmo_mappings[i].page_flags = 0;
        return;
    }
}

iris_error_t kprocess_resolve_demand_fault(struct task *t, uint64_t fault_addr) {
    struct KProcess *p;
    struct KVmoMapping *m;
    uint32_t i;
    uint64_t page_base, page_idx, phys;
    uint8_t published;

    if (!t || !t->process || !t->process->cr3) return IRIS_ERR_INVALID_ARG;
    p = t->process;
    page_base = fault_addr & ~0xFFFULL;

    for (i = 0; i < KPROCESS_VMO_MAP_MAX; i++) {
        m = &p->vmo_mappings[i];
        if (!m->vmo) continue;
        if (page_base < m->virt_base) continue;
        if (page_base >= m->virt_base + m->size) continue;

        page_idx = (page_base - m->virt_base) >> 12;
        if (page_idx >= KVMO_MAX_PAGES) return IRIS_ERR_INVALID_ARG;

        published = 0;
        spinlock_lock(&m->vmo->base.lock);
        phys = m->vmo->pages[page_idx];
        if (phys == 0) {
            uint8_t *kva;
            int k;
            phys = pmm_alloc_page();
            if (!phys) {
                spinlock_unlock(&m->vmo->base.lock);
                return IRIS_ERR_NO_MEMORY;
            }
            kva = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
            for (k = 0; k < 4096; k++) kva[k] = 0;
            m->vmo->pages[page_idx] = phys;
            published = 1;
        }

        if (paging_map_checked_in(p->cr3, page_base, phys, m->page_flags) != 0) {
            if (published && m->vmo->pages[page_idx] == phys) {
                pmm_free_page(phys);
                m->vmo->pages[page_idx] = 0;
            }
            spinlock_unlock(&m->vmo->base.lock);
            return IRIS_ERR_NO_MEMORY;
        }
        spinlock_unlock(&m->vmo->base.lock);
        return IRIS_OK;
    }
    return IRIS_ERR_NOT_FOUND;
}

void kprocess_teardown(struct KProcess *p, struct task *exiting_thread) {
    if (!p || p->teardown_complete) return;

    kprocess_emit_exit_watch(p);
    kprocess_clear_exit_watch(p);
    kprocess_clear_exception_chan(p);
    {
        uint32_t i;
        for (i = 0; i < KPROCESS_VMO_MAP_MAX; i++) {
            if (p->vmo_mappings[i].vmo) {
                kobject_release(&p->vmo_mappings[i].vmo->base);
                p->vmo_mappings[i].vmo = 0;
            }
        }
    }
    irq_routing_unregister_owner(p);
    handle_table_close_all(&p->handle_table);

    (void)exiting_thread; /* thread_count tracks liveness; no per-thread ref needed */
    p->teardown_complete = 1;
}

void kprocess_reap_address_space(struct KProcess *p) {
    if (!p || p->aspace_reaped) return;

    /* Free ELF segment backing pages for ELF-loaded processes.
     * paging_destroy_user_space only frees page table structure pages (PML4/
     * PDPT/PD/PT), not leaf physical pages.  For kernel-linked user tasks the
     * leaf pages are the shared kernel text (never freed) and the user stack
     * (freed by task_exit_current before we reach here).  For ELF tasks the
     * segment pages are exclusively owned by this process and must be freed
     * before the page tables are torn down. */
    for (uint32_t i = 0; i < p->elf_seg_count; i++) {
        for (uint32_t pg = 0; pg < p->elf_segs[i].page_count; pg++) {
            pmm_free_page(p->elf_segs[i].phys_base + (uint64_t)pg * 0x1000ULL);
        }
    }
    p->elf_seg_count = 0;

    paging_destroy_user_space(p->cr3);
    p->cr3 = 0;
    p->aspace_reaped = 1;
}

/*
 * kprocess_live_count: count pool slots currently allocated.
 *
 * Iterates the pool_used[] bitmap.  A slot is live when pool_used[i] == 1,
 * regardless of whether the process has begun teardown.  This gives a
 * conservative upper bound on live processes useful for pressure monitoring.
 * Called from sys_diag_snapshot; no locks held (byte reads are atomic).
 */
uint32_t kprocess_live_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < KPROCESS_POOL_SIZE; i++) {
        if (pool_used[i])
            count++;
    }
    return count;
}
