#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
#include <iris/irq_routing.h>
#include <iris/nameserver.h>
#include <iris/pmm.h>
#include <stdint.h>

#define POOL_SIZE 16
static struct KProcess pool[POOL_SIZE];
static uint8_t         pool_used[POOL_SIZE];

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

    for (int i = 0; i < POOL_SIZE; i++) {
        if (&pool[i] == p) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kprocess_ops = {
    .destroy = kprocess_destroy
};

struct KProcess *kprocess_alloc(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
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

void kprocess_teardown(struct KProcess *p, struct task *exiting_thread) {
    if (!p || p->teardown_complete) return;

    ns_unregister_owner(p);
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

    paging_destroy_user_space(p->cr3);
    p->cr3 = 0;
    p->brk = USER_HEAP_BASE;
    p->aspace_reaped = 1;
}
