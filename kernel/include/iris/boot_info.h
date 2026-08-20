#ifndef IRIS_BOOT_INFO_H
#define IRIS_BOOT_INFO_H

#include <stdint.h>

/*
 * Phase 3.4: Bootstrap CSpace slot layout.
 *
 * These CPtr values are reserved in the root CNode of the initial task.
 * The root CNode is created by kprocess_alloc with KCNODE_DEFAULT_SLOTS=256
 * slots (indices 0..255).
 *
 *   Slot 0                       — CPTR_NULL; never populated (kernel invariant).
 *   Slots 1..BOOT_CPTR_RES_END   — reserved for future well-known boot caps
 *                                  (e.g. KBootstrapCap CSpace slot, root CNode
 *                                  self-reference, future VSpace cap, etc.).
 *   Slot 1: BOOT_CPTR_BOOTSTRAP_CAP — the initial KBootstrapCap (Phase 3.5).
 *   Slots 2..BOOT_CPTR_RES_END     — reserved for future well-known boot caps.
 *   Slots BOOT_CPTR_UNTYPED_START..BOOT_CPTR_UNTYPED_END
 *                                — boot KUntyped blocks, in drain order.
 *
 * kernel_main inserts each boot KUntyped into slot
 *   BOOT_CPTR_UNTYPED_START + drain_index
 * so CPtr 16 is the first block, 17 the second, and so on.
 *
 * The initial task discovers how many blocks it received by scanning slots
 * BOOT_CPTR_UNTYPED_START..BOOT_CPTR_UNTYPED_END with SYS_UNTYPED_INFO
 * until it gets a non-zero error (NOT_FOUND).
 */
/* Phase 3.5 / Phase 4: well-known boot capability slots.
 * BOOT_CPTR_BOOTSTRAP_CAP occupies slot 1 in the root CNode of userboot.
 * BOOT_CPTR_VSPACE occupies slot 2 — the initial VSpace capability (Phase 4).
 * The legacy bootstrap_cap_h handle (arg0) remains valid in dual mode. */
/* Slot 1 held the MONOLITHIC KBootstrapCap until Stage 5 Step 2 split it into
 * one capability per authority.  It is permanently reserved and permanently
 * EMPTY: there is no capability with more than one authority left to put
 * there, and the root task reads what it holds from BootInfo. */
#define BOOT_CPTR_BOOTSTRAP_CAP   1u    /* retired — reserved, never populated */
#define BOOT_CPTR_VSPACE          2u    /* KVSpace for userboot/root task (Phase 4) */
/* Stage 5 Step 2: one capability per authority.  Each of these is a boot
 * capability carrying EXACTLY one authority, published into its own slot; the
 * root task delegates the one it means instead of narrowing a mask.  The root
 * task reads them out of BootInfo — these constants exist for the kernel that
 * writes them and for the diagnostic path that runs when BootInfo is
 * unreadable. */
#define BOOT_CPTR_IRQ_CONTROL     3u    /* SYS_CAP_CREATE_IRQCAP authority */
#define BOOT_CPTR_IOPORT_CONTROL  4u    /* SYS_CAP_CREATE_IOPORT authority */
#define BOOT_CPTR_DEBUG_CONTROL   5u    /* klog drain / sched info / poweroff */
#define BOOT_CPTR_PROC_CONTROL    6u    /* SYS_PROCESS_CREATE authority */
#define BOOT_CPTR_INITRD_CONTROL  7u    /* SYS_INITRD_COUNT / SYS_INITRD_VMO */
#define BOOT_CPTR_FB_CONTROL      8u    /* SYS_FRAMEBUFFER_VMO (one-shot) */
/* Stage 5 Step 3: the root task's OWN objects, as capabilities.  Its root
 * CNode was reachable only through the "arg0 == 0 means my own root"
 * convention and its TCB only by asking SYS_TCB_SELF; seL4's root task simply
 * finds seL4_CapInitThreadCNode and seL4_CapInitThreadTCB in its CSpace. */
#define BOOT_CPTR_CNODE           9u    /* the root task's own root CNode */
#define BOOT_CPTR_TCB            10u    /* the root task's initial thread */
#define BOOT_CPTR_RES_END         15u   /* inclusive; slots 1-15 are reserved */
#define BOOT_CPTR_UNTYPED_START   16u   /* first boot KUntyped CPtr */
#define BOOT_CPTR_UNTYPED_END    255u   /* last boot KUntyped CPtr (root CNode has 256 slots) */

#define IRIS_BOOTINFO_MAGIC   0x49524953424F4F54ULL
#define IRIS_BOOTINFO_VERSION 2ULL

#define IRIS_MMAP_MAX_ENTRIES 256

#define IRIS_MEM_USABLE          1
#define IRIS_MEM_RESERVED        2
#define IRIS_MEM_ACPI_RECLAIMABLE 3
#define IRIS_MEM_ACPI_NVS        4
#define IRIS_MEM_BAD             5
#define IRIS_MEM_BOOTLOADER      6
#define IRIS_MEM_KERNEL          7
#define IRIS_MEM_FRAMEBUFFER     8

struct iris_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct iris_framebuffer_info {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t reserved;
};

struct iris_boot_info {
    uint64_t magic;
    uint64_t version;
    struct iris_framebuffer_info framebuffer;
    uint64_t mmap_entry_count;
    struct iris_mmap_entry mmap[IRIS_MMAP_MAX_ENTRIES];
};

#endif
