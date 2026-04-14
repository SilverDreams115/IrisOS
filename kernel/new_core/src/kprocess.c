#include <iris/nc/kprocess.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/irq_routing.h>
#include <iris/syscall.h>
#include <iris/pmm.h>
#include <stdint.h>

static struct KProcess pool[KPROCESS_POOL_SIZE];
static uint8_t         pool_used[KPROCESS_POOL_SIZE];

static void kprocess_clear_exit_watch(struct KProcess *p) {
    if (!p || !p->exit_watch_armed || !p->exit_watch_ch) return;
    kobject_release(&p->exit_watch_ch->base);
    p->exit_watch_ch = 0;
    p->exit_watch_handle = HANDLE_INVALID;
    p->exit_watch_cookie = 0;
    p->exit_watch_armed = 0;
}

static void kprocess_emit_exit_watch(struct KProcess *p) {
    struct KChanMsg msg;

    if (!p || !p->exit_watch_armed || !p->exit_watch_ch) return;

    for (uint32_t i = 0; i < sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
    msg.type = PROC_EVENT_MSG_EXIT;
    msg.data[PROC_EVENT_OFF_HANDLE + 0] = (uint8_t)(p->exit_watch_handle & 0xFFu);
    msg.data[PROC_EVENT_OFF_HANDLE + 1] = (uint8_t)((p->exit_watch_handle >> 8) & 0xFFu);
    msg.data[PROC_EVENT_OFF_HANDLE + 2] = (uint8_t)((p->exit_watch_handle >> 16) & 0xFFu);
    msg.data[PROC_EVENT_OFF_HANDLE + 3] = (uint8_t)((p->exit_watch_handle >> 24) & 0xFFu);
    msg.data[PROC_EVENT_OFF_COOKIE + 0] = (uint8_t)(p->exit_watch_cookie & 0xFFu);
    msg.data[PROC_EVENT_OFF_COOKIE + 1] = (uint8_t)((p->exit_watch_cookie >> 8) & 0xFFu);
    msg.data[PROC_EVENT_OFF_COOKIE + 2] = (uint8_t)((p->exit_watch_cookie >> 16) & 0xFFu);
    msg.data[PROC_EVENT_OFF_COOKIE + 3] = (uint8_t)((p->exit_watch_cookie >> 24) & 0xFFu);
    msg.data_len = PROC_EVENT_MSG_LEN;
    msg.attached_handle = HANDLE_INVALID;
    msg.attached_rights = RIGHT_NONE;
    (void)kchannel_send(p->exit_watch_ch, &msg);
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
            p->brk = USER_HEAP_BASE;
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
    if (p->exit_watch_armed) return IRIS_ERR_BUSY;

    kobject_retain(&ch->base);
    p->exit_watch_ch = ch;
    p->exit_watch_handle = watched_handle;
    p->exit_watch_cookie = cookie;
    p->exit_watch_armed = 1;

    if (!kprocess_is_alive(p)) {
        kprocess_emit_exit_watch(p);
        kprocess_clear_exit_watch(p);
    }
    return IRIS_OK;
}

void kprocess_teardown(struct KProcess *p, struct task *exiting_thread) {
    if (!p || p->teardown_complete) return;

    kprocess_emit_exit_watch(p);
    kprocess_clear_exit_watch(p);
    irq_routing_unregister_owner(p);
    handle_table_close_all(&p->handle_table);

    if (p->main_thread == exiting_thread)
        p->main_thread = 0;
    else if (!exiting_thread)
        p->main_thread = 0;

    p->teardown_complete = 1;
}

void kprocess_reap_address_space(struct KProcess *p) {
    if (!p || p->aspace_reaped) return;

    if (p->brk > USER_HEAP_BASE) {
        uint64_t heap_end = (p->brk - 1ULL) & ~0xFFFULL;
        for (uint64_t virt = USER_HEAP_BASE; virt <= heap_end; virt += 0x1000ULL) {
            uint64_t phys = paging_virt_to_phys_in(p->cr3, virt);
            if (!phys) continue;
            paging_unmap_in(p->cr3, virt);
            pmm_free_page(phys & ~0xFFFULL);
        }
    }

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
    p->brk = USER_HEAP_BASE;
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
