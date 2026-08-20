#ifndef IRIS_NC_KPAGETABLE_H
#define IRIS_NC_KPAGETABLE_H

/*
 * kpagetable.h — a page table as a capability (Stage 6-pure, Etapa 1).
 *
 * WHAT CHANGES, AND WHY IT IS NOT COSMETIC
 *
 * Stage 6 made every intermediate page table come out of an Untyped the
 * address space named, which answered "who pays".  It did not answer the
 * question seL4 answers, and ledger D-5 records the difference: the KERNEL
 * still decided WHEN a table came into existence and WHERE it went, carving
 * one silently on whichever map first needed it.  The holder paid for an
 * object it could not name, could not count, could not hand to anyone else and
 * could not reclaim on its own.
 *
 * Here a page table is retyped from an Untyped like every other object, lands
 * in a CSpace slot like every other object, and is INSTALLED into an address
 * space by an explicit invocation — seL4's seL4_X86_PageTable_Map.  A map that
 * needs a level which is not there fails and says so; it does not quietly
 * spend the holder's budget.
 *
 * THE STORAGE IS THE PAGE.  A KPageTable's 4 KiB region IS the hardware table
 * — the same identity a Frame has with its page.  The KObject header is a
 * top-carved child block of the same Untyped, exactly as KFrame does it, and
 * for the same reason: the header must not live inside the page, because that
 * page is walked by the MMU.  That IRIS needs a header at all is the residue
 * of D-5 that this etapa does not close (seL4 has no object behind a frame or
 * a page table); what it closes is the part that mattered — the user decides
 * that the table exists, and holds the capability to it.
 */

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <stdatomic.h>
#include <stdint.h>

struct KVSpace;
struct KUntyped;

/* Paging levels, named by what the table CONTAINS, which is how the mapping
 * invocation reports what is missing.  The numbers are the x86-64 walk depth
 * so that a level can be compared and decremented. */
#define KPT_LEVEL_UNMAPPED 0    /* retyped, not installed anywhere */
#define KPT_LEVEL_PT       1    /* contains page entries      (PD  -> PT)   */
#define KPT_LEVEL_PD       2    /* contains PT entries        (PDPT-> PD)   */
#define KPT_LEVEL_PDPT     3    /* contains PD entries        (PML4-> PDPT) */

struct KPageTable {
    struct KObject   base;       /* must be first */
    uint64_t         paddr;      /* the 4 KiB region — this IS the table */
    /* Where it is installed, or nothing.  A table is mapped at most once:
     * installing the same table at two addresses would make one region two
     * different parts of a walk, and unmapping either would strand the other. */
    struct KVSpace  *mapped_vs;
    uint64_t         mapped_va;  /* the VA whose walk this table serves */
    uint32_t         level;      /* KPT_LEVEL_*; UNMAPPED until installed */
    struct KPageTable *next;     /* VSpace's list of installed tables */
};

#ifdef __KERNEL__

/* Placement-init over a top-carved child block; `paddr` is the page carved
 * from the same Untyped.  The block arrives zeroed. */
struct KPageTable *kpagetable_alloc_at(void *mem, uint64_t paddr);

/* Zero the table's 4 KiB region.  Called at retype: a table installed over
 * stale bytes is a walk into whatever the region held before. */
void kpagetable_zero(struct KPageTable *pt);

uint32_t kpagetable_live_count(void);

#endif /* __KERNEL__ */

#endif /* IRIS_NC_KPAGETABLE_H */
