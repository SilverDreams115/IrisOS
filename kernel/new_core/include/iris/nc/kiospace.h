/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_NC_KIOSPACE_H
#define IRIS_NC_KIOSPACE_H

/*
 * kiospace.h — a DEVICE's address space (Stage 10-dma, §10.2 step 4).
 *
 * Every other address space in IRIS bounds what a THREAD may reach.  This one
 * bounds what a DEVICE may reach, and it is the same idea with the same
 * machinery: a set of translations, built out of page-table levels the holder
 * paid for, containing exactly the frames somebody mapped.
 *
 * ── What names a device ─────────────────────────────────────────────────────
 *
 * Its SOURCE-ID: the PCI bus:device:function that rides on every DMA request
 * it makes.  The kernel does not discover that — a holder of
 * IOSPACE_CONTROL passes it, which is why there is no PCI enumeration in the
 * kernel.  seL4 works the same way.
 *
 * ── Why the page table is three levels and not four ─────────────────────────
 *
 * Because the hardware said so.  The remapping unit reports which address
 * widths it supports (SAGAW), and the one IRIS is tested against offers 39
 * bits — three levels — only.  A walker written for four and run on that
 * builds a table the unit reads as garbage, which is why the depth is read
 * from the unit rather than chosen.
 *
 * ── The one thing that is NOT like a VSpace ─────────────────────────────────
 *
 * A CPU page table is walked by a core that also runs the code that wrote it,
 * so a write and a walk are ordered by the same cache.  A remapping unit is a
 * separate device with its own view of memory, and on the hardware IRIS is
 * tested against its walk is NOT cache-coherent.  Every line written into one
 * of these tables has to be pushed out of the CPU's cache before the unit can
 * see it — and a kernel that forgets installs translations the hardware never
 * reads, which presents as a device that still reaches everything.
 */

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <stdint.h>

struct KIOPageTable;
struct KFrame;

/*
 * How many frames one device may have mapped at once.
 *
 * A BOUND with a refusal rather than a list with an allocator, for the reason
 * the context-table pool is one: this runs on a syscall path, and the kernel
 * reaching an allocator from a capability invocation is what charter M1 and
 * the purity gate exist to prevent.  The bound is declared so "a device may
 * hold sixteen mappings" is a fact somebody can read rather than a limit
 * somebody discovers.
 */
#define KIOSPACE_MAX_MAPPINGS 16u

/*
 * What a device reaches, recorded so it can be taken back.
 *
 * The page-table entry holds a physical address and nothing else, so the table
 * alone cannot say WHICH frame capability a mapping came from — and a mapping
 * is a reference to that frame: while it stands the frame must not be
 * destroyed, or a bus master would be pointed at memory that has been handed
 * back.  This is where that reference is held.
 */
struct KIOMapping {
    uint64_t       dma;
    struct KFrame *frame;
};

struct KIOSpace {
    struct KObject base;          /* must be first */
    uint16_t source_id;           /* bus:device:function, as the device reports it */
    uint16_t domain_id;           /* the unit's translation domain for this device */
    uint8_t  unit;                /* which remapping unit claims that source-id */
    uint8_t  installed;           /* a context entry names this space */
    uint8_t  levels;              /* page-table depth the unit requires */
    uint8_t  _pad;
    /*
     * The top level, and the list of every level installed under it.
     *
     * The list exists for teardown: when the last capability to this space
     * goes, every level has to be released, and walking the tables to find
     * them would mean walking translations that are being torn down.
     */
    struct KIOPageTable *root;
    struct KIOPageTable *tables;

    struct KIOMapping maps[KIOSPACE_MAX_MAPPINGS];
    uint32_t          map_count;
};

#ifdef __KERNEL__

/*
 * Placement-init over a top-carved child block.  The space arrives UNBOUND: it
 * names no device yet.
 *
 * Two steps rather than one, and the split is the authority.  Retyping is
 * paying for the object out of your own memory, which anyone holding an
 * Untyped may do.  Naming a DEVICE is saying which piece of hardware may reach
 * what, and that takes IOSPACE_CONTROL — the same shape as retyping a TCB and
 * then configuring it, or a scheduling context and then configuring it against
 * SchedControl.
 */
struct KIOSpace *kiospace_alloc_at(void *mem);

/*
 * Bind the space to a device.  Resolves which remapping unit claims that
 * source-id and refuses if none does — an IOSpace nothing enforces would be a
 * capability promising containment it cannot deliver, which is worse than not
 * having one because the holder would believe the device is bounded.
 *
 * ALREADY_EXISTS on a space that is already bound: rebinding would move a live
 * set of translations to a different device.
 */
iris_error_t kiospace_bind(struct KIOSpace *io, uint16_t source_id);

uint32_t kiospace_live_count(void);

/*
 * Install a page-table level at `dma`.  The FIRST call installs the top level
 * and is what makes the device stop being blocked: it claims a translation
 * domain and writes the context entry.  Later calls install whatever level the
 * walk is missing.
 *
 * ALREADY_EXISTS when the level is there or the table is installed elsewhere;
 * NO_MEMORY when the unit has no domain left or no context table for the
 * device's bus — both bounded pools with declared limits, refused by name
 * rather than allocated around.
 */
iris_error_t kiospace_map_table(struct KIOSpace *io, struct KIOPageTable *pt,
                                uint64_t dma);

/*
 * Map a frame at `dma` with `rights`.  This is the operation the whole stage
 * exists for: after it, the device may reach that frame and nothing else that
 * was not mapped the same way.
 *
 * MISSING_TABLE when the walk is incomplete — the holder retypes a
 * KIOPageTable and installs it, exactly as it would for a CPU page table, and
 * the kernel allocates nothing.
 */
iris_error_t kiospace_map_frame(struct KIOSpace *io, struct KFrame *fr,
                                uint64_t dma, iris_rights_t rights);

/* Take a mapping out, and tell the unit's translation cache.  A revoke that
 * leaves a stale IOTLB entry is a revoke that did not happen.
 *
 * `out_frame` receives the frame the mapping held a reference to, so the
 * caller can release it — the release happens OUTSIDE, because it can run a
 * destructor and this is called with the space's own bookkeeping half torn
 * down. */
iris_error_t kiospace_unmap(struct KIOSpace *io, uint64_t dma,
                            struct KFrame **out_frame);

#endif /* __KERNEL__ */

#endif /* IRIS_NC_KIOSPACE_H */
