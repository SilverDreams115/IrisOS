/*
 * kiospace.c — a device's address space (Stage 10-dma, §10.2 steps 4 and 5).
 *
 * See kiospace.h for what this is and why a device needs one.  What lives here
 * is the walk: installing a level, mapping a frame, taking one out again, and
 * the teardown that has to happen when the last capability to the space goes.
 *
 * ── The rule this file exists to keep ──────────────────────────────────────
 *
 * Every write into one of these tables is followed by a flush, and every
 * change to what a device may reach is followed by an IOTLB invalidation.
 * Both because the reader is a DEVICE, not this core: it does not share the
 * cache the write landed in, and it keeps its own translation cache that
 * nothing here can see.  A kernel that skips either installs translations the
 * hardware never reads, or leaves a device reaching a frame the books say it
 * cannot — and the second is a revoke that did not happen.
 */

#include <iris/nc/kiospace.h>
#include <iris/nc/kiopagetable.h>
#include <iris/nc/kframe.h>
#include <iris/nc/kuntyped.h>
#include <iris/iommu.h>
#include <iris/paging.h>
#include <stdatomic.h>

static _Atomic uint32_t kiospace_live;

uint32_t kiospace_live_count(void) {
    return atomic_load_explicit(&kiospace_live, memory_order_relaxed);
}

/* A second-level page-table entry: read, write, and a 4 KiB-aligned address.
 * There is no Present bit — an entry with neither R nor W is not present, and
 * a device that walks into one takes a translation fault. */
#define IOPTE_READ  (1ull << 0)
#define IOPTE_WRITE (1ull << 1)
#define IOPTE_ADDR  0x0000FFFFFFFFF000ull

static uint64_t *io_table(const struct KIOPageTable *pt) {
    return (uint64_t *)(uintptr_t)PHYS_TO_VIRT(pt->paddr);
}

/* The index this DMA address uses at a given level.  Level 1 is the leaf, so
 * its shift is 12; every level above adds nine bits. */
static uint32_t io_index(uint64_t dma, uint32_t level) {
    return (uint32_t)((dma >> (12u + 9u * (level - 1u))) & 0x1FFu);
}

static void iospace_track(struct KIOSpace *io, struct KIOPageTable *pt) {
    pt->next   = io->tables;
    io->tables = pt;
}

/*
 * Follow the walk as far as it goes, and say where it stops.
 *
 * `out_tab` is the deepest table that exists, `out_level` its level.  A caller
 * installing a level uses it to check the level it is handing over is the one
 * that is missing; a caller mapping a frame uses it to check the walk is
 * complete.
 */
static void iospace_walk(struct KIOSpace *io, uint64_t dma,
                         struct KIOPageTable **out_tab, uint32_t *out_level) {
    *out_tab   = io->root;
    *out_level = io->root ? io->levels : 0u;
    if (!io->root) return;

    struct KIOPageTable *cur = io->root;
    uint32_t level = io->levels;
    while (level > KIOPT_LEVEL_LEAF) {
        uint64_t e = io_table(cur)[io_index(dma, level)];
        if (!(e & (IOPTE_READ | IOPTE_WRITE))) break;
        uint64_t next_phys = e & IOPTE_ADDR;
        struct KIOPageTable *next = 0;
        for (struct KIOPageTable *t = io->tables; t; t = t->next)
            if (t->paddr == next_phys) { next = t; break; }
        if (!next) break;              /* a level the space does not own */
        cur   = next;
        level--;
        *out_tab   = cur;
        *out_level = level;
    }
}

iris_error_t kiospace_map_table(struct KIOSpace *io, struct KIOPageTable *pt,
                                uint64_t dma) {
    if (!io || !pt) return IRIS_ERR_INVALID_ARG;
    if (io->unit == 0xFFu) return IRIS_ERR_NOT_SUPPORTED;   /* names no device */
    if (dma & 0xFFFull) return IRIS_ERR_INVALID_ARG;
    if (pt->level != KIOPT_LEVEL_UNMAPPED) return IRIS_ERR_ALREADY_EXISTS;

    kiopagetable_zero(pt);
    iommu_flush_tables(io->unit, io_table(pt), 4096u);

    /* The top level, if the space has none: this is what the context entry
     * will point at, and it is what makes the device stop being blocked. */
    if (!io->root) {
        pt->level      = io->levels;
        pt->mapped_io  = io;
        pt->mapped_dma = 0;
        io->root       = pt;
        iospace_track(io, pt);

        uint16_t dom = iommu_domain_claim(io->unit);
        if (!dom) { io->root = 0; io->tables = pt->next; pt->level = KIOPT_LEVEL_UNMAPPED;
                    pt->mapped_io = 0; return IRIS_ERR_NO_MEMORY; }
        io->domain_id = dom;

        if (!iommu_context_set(io->unit, io->source_id, dom, pt->paddr, io->levels)) {
            iommu_domain_release(io->unit, dom);
            io->domain_id = 0;
            io->root = 0; io->tables = pt->next;
            pt->level = KIOPT_LEVEL_UNMAPPED; pt->mapped_io = 0;
            return IRIS_ERR_NO_MEMORY;
        }
        io->installed = 1;
        return IRIS_OK;
    }

    struct KIOPageTable *parent; uint32_t level;
    iospace_walk(io, dma, &parent, &level);
    if (level <= KIOPT_LEVEL_LEAF) return IRIS_ERR_ALREADY_EXISTS;

    /* Install under the deepest level that exists.  Read and write on the
     * INTERIOR entry: a second-level walk uses them as "this entry may be
     * followed", and the leaf is where a device's actual rights are decided. */
    uint64_t *tab = io_table(parent);
    uint32_t  idx = io_index(dma, level);
    pt->level      = level - 1u;
    pt->mapped_io  = io;
    pt->mapped_dma = dma;
    iospace_track(io, pt);

    tab[idx] = (pt->paddr & IOPTE_ADDR) | IOPTE_READ | IOPTE_WRITE;
    iommu_flush_tables(io->unit, &tab[idx], 8u);
    iommu_invalidate_iotlb(io->unit);
    return IRIS_OK;
}

iris_error_t kiospace_map_frame(struct KIOSpace *io, struct KFrame *fr,
                                uint64_t dma, iris_rights_t rights) {
    if (!io || !fr) return IRIS_ERR_INVALID_ARG;
    if (io->unit == 0xFFu) return IRIS_ERR_NOT_SUPPORTED;
    if (dma & 0xFFFull) return IRIS_ERR_INVALID_ARG;
    if (!io->root)      return IRIS_ERR_MISSING_TABLE;

    struct KIOPageTable *leaf; uint32_t level;
    iospace_walk(io, dma, &leaf, &level);
    if (level != KIOPT_LEVEL_LEAF) return IRIS_ERR_MISSING_TABLE;

    uint64_t *tab = io_table(leaf);
    uint32_t  idx = io_index(dma, KIOPT_LEVEL_LEAF);
    if (tab[idx] & (IOPTE_READ | IOPTE_WRITE)) return IRIS_ERR_ALREADY_EXISTS;

    /*
     * The device's rights, and they are the frame capability's rights narrowed
     * by what the caller asked for — never widened.  A holder that could hand
     * a device more than it holds itself would be a holder laundering
     * authority through a device.
     */
    uint64_t e = fr->paddr & IOPTE_ADDR;
    if (rights & RIGHT_READ)  e |= IOPTE_READ;
    if (rights & RIGHT_WRITE) e |= IOPTE_WRITE;
    if (!(e & (IOPTE_READ | IOPTE_WRITE))) return IRIS_ERR_ACCESS_DENIED;

    /* Record it BEFORE the entry goes in.  A mapping the device can reach and
     * the space cannot name is a mapping nothing will ever take back. */
    uint32_t slot = KIOSPACE_MAX_MAPPINGS;
    for (uint32_t i = 0; i < KIOSPACE_MAX_MAPPINGS; i++)
        if (!io->maps[i].frame) { slot = i; break; }
    if (slot == KIOSPACE_MAX_MAPPINGS) return IRIS_ERR_NO_MEMORY;

    io->maps[slot].dma   = dma;
    io->maps[slot].frame = fr;
    io->map_count++;

    tab[idx] = e;
    iommu_flush_tables(io->unit, &tab[idx], 8u);
    iommu_invalidate_iotlb(io->unit);
    return IRIS_OK;
}

iris_error_t kiospace_unmap(struct KIOSpace *io, uint64_t dma,
                            struct KFrame **out_frame) {
    if (out_frame) *out_frame = 0;
    if (!io) return IRIS_ERR_INVALID_ARG;
    if (io->unit == 0xFFu) return IRIS_ERR_NOT_SUPPORTED;
    if (dma & 0xFFFull) return IRIS_ERR_INVALID_ARG;
    if (!io->root) return IRIS_ERR_NOT_FOUND;

    struct KIOPageTable *leaf; uint32_t level;
    iospace_walk(io, dma, &leaf, &level);
    if (level != KIOPT_LEVEL_LEAF) return IRIS_ERR_NOT_FOUND;

    uint64_t *tab = io_table(leaf);
    uint32_t  idx = io_index(dma, KIOPT_LEVEL_LEAF);
    if (!(tab[idx] & (IOPTE_READ | IOPTE_WRITE))) return IRIS_ERR_NOT_FOUND;

    tab[idx] = 0;
    /* Flush, THEN invalidate.  The other order leaves a window in which the
     * unit refills its cache from a table line the CPU has not written back —
     * which is a revoke that undoes itself. */
    iommu_flush_tables(io->unit, &tab[idx], 8u);
    iommu_invalidate_iotlb(io->unit);

    for (uint32_t i = 0; i < KIOSPACE_MAX_MAPPINGS; i++) {
        if (!io->maps[i].frame || io->maps[i].dma != dma) continue;
        if (out_frame) *out_frame = io->maps[i].frame;
        io->maps[i].frame = 0;
        io->maps[i].dma   = 0;
        io->map_count--;
        break;
    }
    return IRIS_OK;
}

/*
 * The device is blocked again, and every level goes back.
 *
 * Runs when the last capability to the space is gone.  The context entry is
 * removed FIRST: until it is, the device has a live translation into tables
 * that are about to be handed back to an Untyped, and the order is the whole
 * difference between a teardown and a use-after-free with a bus master on the
 * other end.
 */
static void kiospace_obj_close(struct KObject *obj) {
    struct KIOSpace *io = (struct KIOSpace *)obj;

    if (io->unit != 0xFFu && io->installed) {
        (void)iommu_context_set(io->unit, io->source_id, 0, 0, io->levels);
        io->installed = 0;
    }
    if (io->unit != 0xFFu && io->domain_id) {
        iommu_domain_release(io->unit, io->domain_id);
        io->domain_id = 0;
    }

    /* Every frame this device could reach, released.  The entries are gone
     * already — the context entry was removed above, so the device is blocked
     * whatever the tables still say — and these are the references that kept
     * the frames alive while it could. */
    for (uint32_t i = 0; i < KIOSPACE_MAX_MAPPINGS; i++) {
        struct KFrame *fr = io->maps[i].frame;
        if (!fr) continue;
        io->maps[i].frame = 0;
        io->maps[i].dma   = 0;
        kobject_release(&fr->base);
    }
    io->map_count = 0;

    /* Release the levels.  Each was retained when it was installed, so this is
     * the matching release and not a free. */
    struct KIOPageTable *t = io->tables;
    io->tables = 0;
    io->root   = 0;
    while (t) {
        struct KIOPageTable *next = t->next;
        t->next       = 0;
        t->mapped_io  = 0;
        t->level      = KIOPT_LEVEL_UNMAPPED;
        kobject_release(&t->base);
        t = next;
    }
}

static void kiospace_obj_destroy(struct KObject *obj) {
    atomic_fetch_sub_explicit(&kiospace_live, 1u, memory_order_relaxed);
    kobject_storage_free(obj, (uint32_t)sizeof(struct KIOSpace), 0);
}

static const struct KObjectOps kiospace_ops = {
    .close   = kiospace_obj_close,
    .destroy = kiospace_obj_destroy,
};

struct KIOSpace *kiospace_alloc_at(void *mem) {
    if (!mem) return 0;

    struct KIOSpace *io = (struct KIOSpace *)mem;   /* arrives zeroed */
    kobject_init_in_untyped(&io->base, KOBJ_IOSPACE, &kiospace_ops,
                            (uint32_t)sizeof(struct KIOSpace));
    io->source_id = 0;
    io->domain_id = 0;
    io->unit      = 0xFFu;      /* unbound: no unit claims it yet */
    io->installed = 0;
    io->levels    = 0;
    io->root      = 0;
    io->tables    = 0;
    io->map_count = 0;
    atomic_fetch_add_explicit(&kiospace_live, 1u, memory_order_relaxed);
    return io;
}

iris_error_t kiospace_bind(struct KIOSpace *io, uint16_t source_id) {
    if (!io) return IRIS_ERR_INVALID_ARG;
    if (io->unit != 0xFFu) return IRIS_ERR_ALREADY_EXISTS;

    uint32_t unit = iommu_unit_for_source(source_id);
    if (unit == IOMMU_NO_UNIT) return IRIS_ERR_NOT_SUPPORTED;

    uint32_t levels = iommu_levels(unit);
    if (levels < 3u || levels > 4u) return IRIS_ERR_NOT_SUPPORTED;

    io->source_id = source_id;
    io->unit      = (uint8_t)unit;
    io->levels    = (uint8_t)levels;
    return IRIS_OK;
}
