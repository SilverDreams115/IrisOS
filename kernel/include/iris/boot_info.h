#ifndef IRIS_BOOT_INFO_H
#define IRIS_BOOT_INFO_H

#include <stdint.h>

/*
 * Fase 3.4: Bootstrap CSpace slot layout.
 *
 * These CPtr values are reserved in the root CNode of the initial task.
 * The root CNode is created by kprocess_alloc with KCNODE_DEFAULT_SLOTS=256
 * slots (indices 0..255).
 *
 *   Slot 0                       — CPTR_NULL; never populated (kernel invariant).
 *   Slots 1..BOOT_CPTR_RES_END   — reserved for future well-known boot caps
 *                                  (e.g. KBootstrapCap CSpace slot, root CNode
 *                                  self-reference, future VSpace cap, etc.).
 *   Slot 1: BOOT_CPTR_BOOTSTRAP_CAP — the initial KBootstrapCap (Fase 3.5).
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
/* Fase 3.5 / Fase 4: well-known boot capability slots.
 * BOOT_CPTR_BOOTSTRAP_CAP occupies slot 1 in the root CNode of userboot.
 * BOOT_CPTR_VSPACE occupies slot 2 — the initial VSpace capability (Fase 4).
 * The legacy bootstrap_cap_h handle (arg0) remains valid in dual mode. */
#define BOOT_CPTR_BOOTSTRAP_CAP   1u    /* KBootstrapCap well-known CPtr (Fase 3.5) */
#define BOOT_CPTR_VSPACE          2u    /* KVSpace for userboot/root task (Fase 4) */
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
