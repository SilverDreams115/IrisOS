#ifndef IRIS_NC_KIOPAGETABLE_H
#define IRIS_NC_KIOPAGETABLE_H

/*
 * kiopagetable.h — one level of a device's translation tables (Stage 10-dma).
 *
 * The same object as `KPageTable` one address space over: 4 KiB of storage
 * that IS the table, plus the record of where it is installed.  It is a
 * separate type rather than a reused one because the two are walked by
 * different hardware with different entry formats — a CPU page-table entry and
 * a remapping unit's differ in every bit that matters — and a capability that
 * could be installed in either would be a capability whose meaning depends on
 * where it lands.
 *
 * The holder pays for it out of its own Untyped, which is the whole reason the
 * kernel needs no allocator on this path.
 */

#include <iris/nc/kobject.h>
#include <stdint.h>

/* Levels, numbered by walk depth so a level can be compared and decremented,
 * exactly as KPT_LEVEL_* is.  A 39-bit space uses 3, 2, 1; the leaf level
 * holds page entries. */
#define KIOPT_LEVEL_UNMAPPED 0
#define KIOPT_LEVEL_LEAF     1   /* contains page entries */
#define KIOPT_LEVEL_MID      2
#define KIOPT_LEVEL_TOP      3

struct KIOSpace;

struct KIOPageTable {
    struct KObject  base;        /* must be first */
    uint64_t        paddr;       /* the 4 KiB region — this IS the table */
    struct KIOSpace *mapped_io;  /* where it is installed, or nothing */
    uint64_t        mapped_dma;  /* the DMA address whose walk it serves */
    uint32_t        level;       /* KIOPT_LEVEL_*; UNMAPPED until installed */
    struct KIOPageTable *next;   /* the IOSpace's list of installed levels */
};

#ifdef __KERNEL__

struct KIOPageTable *kiopagetable_alloc_at(void *mem, uint64_t paddr);

/* Zero the 4 KiB region.  A table installed over stale bytes is a device
 * walking into whatever the region held before it was retyped. */
void kiopagetable_zero(struct KIOPageTable *pt);

uint32_t kiopagetable_live_count(void);

#endif /* __KERNEL__ */

#endif /* IRIS_NC_KIOPAGETABLE_H */
