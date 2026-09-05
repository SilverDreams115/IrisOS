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

/*
 * kstack_alloc / kstack_free are DELETED (Stage 9-evt step 3).
 *
 * Every thread used to own two pages of kernel stack plus a guard page,
 * allocated at creation from the PMM reserve — including a TCB retyped from an
 * Untyped, whose payload the user paid for and whose kernel stack the kernel
 * supplied anyway.  Charter M3 says the kernel does not implicitly allocate
 * memory on somebody's behalf, and 8 KiB per thread was the largest standing
 * exception to it.
 *
 * They are gone because nothing needs them: a thread's ring-3 context and its
 * first entry both live in its TCB, a parked syscall holds nothing, and every
 * kernel entry lands on the core's stack.  Kernel memory stops scaling with
 * thread count, which is the thing step 3 was for.
 *
 * `kstack_panic` stays: the boot path still has one fatal condition to report
 * and no console to report it through.
 */
