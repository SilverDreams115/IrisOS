#include "scheduler_priv.h"
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/serial.h>

/*
 * kstack.c — guarded kernel stack allocator.
 *
 * Each task slot i owns a 3-page virtual region in the kstack area:
 *   KSTACK_VIRT_BASE + i * KSTACK_SLOT_SIZE + 0             guard (unmapped)
 *   KSTACK_VIRT_BASE + i * KSTACK_SLOT_SIZE + PAGE_SIZE     kstack page 0
 *   KSTACK_VIRT_BASE + i * KSTACK_SLOT_SIZE + PAGE_SIZE*2   kstack page 1
 *
 * Invariant: t->kstack == 0 iff no kstack is allocated for this slot.
 */

void kstack_panic(const char *msg) {
    serial_write("[IRIS][KSTACK] FATAL: ");
    serial_write(msg);
    serial_write("\n");
    for (;;) __asm__ volatile ("hlt");
}

int kstack_alloc(struct task *t, int idx) {
    uint64_t virt_base = KSTACK_VIRT_BASE + (uint64_t)idx * KSTACK_SLOT_SIZE;
    uint64_t phys = pmm_alloc_pages(2);
    if (phys == 0) return -1;

    uint64_t virt_pg0 = virt_base + KSTACK_PAGE_SIZE;
    uint64_t virt_pg1 = virt_base + KSTACK_PAGE_SIZE * 2;

    if (paging_map_checked_in(kernel_cr3, virt_pg0, phys,
                              PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX) != 0 ||
        paging_map_checked_in(kernel_cr3, virt_pg1, phys + KSTACK_PAGE_SIZE,
                              PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX) != 0) {
        pmm_free_page(phys);
        pmm_free_page(phys + KSTACK_PAGE_SIZE);
        return -1;
    }

    t->kstack      = (uint8_t *)(uintptr_t)virt_pg0;
    t->kstack_phys = phys;
    return 0;
}

void kstack_free(struct task *t, int idx) {
    if (!t->kstack || t->kstack_phys == 0) return;
    uint64_t virt_base = KSTACK_VIRT_BASE + (uint64_t)idx * KSTACK_SLOT_SIZE;
    paging_unmap_in(kernel_cr3, virt_base + KSTACK_PAGE_SIZE);
    paging_unmap_in(kernel_cr3, virt_base + KSTACK_PAGE_SIZE * 2);
    pmm_free_page(t->kstack_phys);
    pmm_free_page(t->kstack_phys + KSTACK_PAGE_SIZE);
    t->kstack      = 0;
    t->kstack_phys = 0;
}
