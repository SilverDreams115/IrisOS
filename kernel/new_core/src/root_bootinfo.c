/*
 * root_bootinfo.c — Stage 5, Etapa 1: building the root task's BootInfo page.
 *
 * Pure buffer arithmetic on purpose: no PMM, no CSpace, no task.  The kernel's
 * boot path decides WHAT the root task holds; this file only writes it down in
 * a form the root task can read, which is why it is unit-testable on the host
 * (tests/kernel/test_root_bootinfo.c) instead of only observable by booting.
 *
 * Every writer here is bounds-checked against the caller-declared buffer size.
 * The buffer is a page the root task will have mapped: an overrun is a kernel
 * heap overflow, and a short write is a lie about the CSpace.
 */

#include <iris/root_bootinfo.h>
#include <iris/boot_info.h>
#include <iris/nc/error.h>
#include <iris/nc/kcnode.h>
#include <stdint.h>

#define RBI_HEADER_BYTES ((uint32_t)sizeof(struct iris_root_bootinfo))
#define RBI_ENTRY_BYTES  ((uint32_t)sizeof(struct iris_bootinfo_untyped))

/* The description must be able to cover every untyped slot a root CNode has
 * room for; otherwise the page, not the memory map, decides how much of the
 * machine reaches userland. */
_Static_assert((IRIS_ROOT_BOOTINFO_BYTES - RBI_HEADER_BYTES) / RBI_ENTRY_BYTES >=
                   (KCNODE_DEFAULT_SLOTS - BOOT_CPTR_UNTYPED_START),
               "BootInfo must describe every boot-untyped slot of a root CNode");

uint32_t root_bootinfo_capacity(uint32_t bytes) {
    if (bytes < RBI_HEADER_BYTES) return 0u;
    return (bytes - RBI_HEADER_BYTES) / RBI_ENTRY_BYTES;
}

iris_error_t root_bootinfo_init(void *buf, uint32_t bytes,
                                uint64_t cap_bootstrap, uint64_t cap_vspace,
                                uint32_t cnode_slots) {
    struct iris_root_bootinfo *bi = (struct iris_root_bootinfo *)buf;

    if (!bi) return IRIS_ERR_INVALID_ARG;
    if (bytes < RBI_HEADER_BYTES) return IRIS_ERR_INVALID_ARG;

    bi->magic            = IRIS_ROOT_BOOTINFO_MAGIC;
    bi->version          = IRIS_ROOT_BOOTINFO_VERSION;
    bi->header_bytes     = RBI_HEADER_BYTES;
    bi->total_bytes      = RBI_HEADER_BYTES;
    bi->cap_bootstrap    = cap_bootstrap;
    bi->cap_vspace       = cap_vspace;
    bi->cnode_slots      = cnode_slots;
    /* No free slots claimed yet — the boot path knows which slots it left
     * empty only after it has stopped filling them. */
    bi->empty_slot_first = cnode_slots;
    bi->empty_slot_end   = cnode_slots;
    bi->untyped_count    = 0u;
    return IRIS_OK;
}

iris_error_t root_bootinfo_set_empty_range(void *buf, uint32_t bytes,
                                           uint32_t first, uint32_t end) {
    struct iris_root_bootinfo *bi = (struct iris_root_bootinfo *)buf;

    if (!bi) return IRIS_ERR_INVALID_ARG;
    if (bytes < RBI_HEADER_BYTES)                  return IRIS_ERR_INVALID_ARG;
    if (bi->magic != IRIS_ROOT_BOOTINFO_MAGIC)     return IRIS_ERR_INVALID_ARG;
    if (bi->version != IRIS_ROOT_BOOTINFO_VERSION) return IRIS_ERR_INVALID_ARG;
    /* An inverted range describes something impossible, and one past the end
     * of the CNode describes slots that do not exist.  Both are refused here
     * rather than shipped to a root task that would trust them. */
    if (first > end)             return IRIS_ERR_INVALID_ARG;
    if (end > bi->cnode_slots)   return IRIS_ERR_INVALID_ARG;

    bi->empty_slot_first = first;
    bi->empty_slot_end   = end;
    return IRIS_OK;
}

iris_error_t root_bootinfo_add_untyped(void *buf, uint32_t bytes,
                                       uint64_t cptr, uint64_t paddr,
                                       uint64_t size_bytes, int is_device) {
    struct iris_root_bootinfo *bi = (struct iris_root_bootinfo *)buf;

    if (!bi) return IRIS_ERR_INVALID_ARG;
    if (bytes < RBI_HEADER_BYTES)               return IRIS_ERR_INVALID_ARG;
    if (bi->magic != IRIS_ROOT_BOOTINFO_MAGIC)  return IRIS_ERR_INVALID_ARG;
    if (bi->version != IRIS_ROOT_BOOTINFO_VERSION) return IRIS_ERR_INVALID_ARG;
    /* A descriptor naming slot 0 or a zero-sized region describes a capability
     * that cannot be invoked: refuse to write it rather than let the root task
     * discover it by failing. */
    if (cptr == 0u || size_bytes == 0u)         return IRIS_ERR_INVALID_ARG;

    if (bi->untyped_count >= root_bootinfo_capacity(bytes))
        return IRIS_ERR_NO_MEMORY;

    struct iris_bootinfo_untyped *e = &bi->untyped[bi->untyped_count];
    e->cptr       = cptr;
    e->paddr      = paddr;
    e->size_bytes = size_bytes;
    e->is_device  = is_device ? 1u : 0u;
    e->reserved   = 0u;

    bi->untyped_count++;
    bi->total_bytes = (uint64_t)RBI_HEADER_BYTES +
                      (uint64_t)bi->untyped_count * (uint64_t)RBI_ENTRY_BYTES;
    return IRIS_OK;
}
