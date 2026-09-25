/* SPDX-License-Identifier: Apache-2.0 */
/*
 * iommu.c — finding the DMA remapping units (Stage 10-dma, §10.2 step 1).
 *
 * See iris/iommu.h for why the kernel reads this table at all and why it reads
 * no PCI.  This step FINDS the units and stops: nothing is mapped, nothing is
 * enabled, and a machine with no DMAR runs exactly as it did before the file
 * existed.  "The hardware is there and I found it" is a claim that can fail
 * entirely on its own, and keeping it apart from "and I am using it" means a
 * failure in either is legible — the same reason SMP kept the processors being
 * up separate from the processors scheduling.
 */

#include <iris/iommu.h>
#include <iris/acpi.h>
#include <iris/klog.h>
#include <iris/paging.h>

/*
 * The DMAR table, as the VT-d specification lays it out.
 *
 * Only the header is a struct.  The remapping structures after it are a
 * variable-length sequence of (type, length) records, walked by length rather
 * than indexed — the standard shape for ACPI, and the reason a walk must trust
 * `length` for stepping but never for reading past the table's own end.
 */
struct dmar_table {
    char     signature[4];        /* "DMAR" */
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint8_t  host_address_width;  /* width - 1 */
    uint8_t  flags;
    uint8_t  reserved[10];
} __attribute__((packed));

struct dmar_record {
    uint16_t type;
    uint16_t length;
} __attribute__((packed));

#define DMAR_TYPE_DRHD 0u

struct dmar_drhd {
    uint16_t type;                /* 0 */
    uint16_t length;
    uint8_t  flags;               /* bit 0: INCLUDE_PCI_ALL */
    uint8_t  reserved;
    uint16_t segment;
    uint64_t register_base;
    /* device scopes follow, each (type, length, ...) */
} __attribute__((packed));

struct dmar_scope {
    uint8_t  type;
    uint8_t  length;
    uint16_t reserved;
    uint8_t  enumeration_id;
    uint8_t  start_bus;
    /* path: (device, function) pairs */
} __attribute__((packed));

static struct iris_iommu_unit units[IRIS_IOMMU_MAX_UNITS];
static uint32_t               unit_n;
static uint32_t               host_addr_width;
static uint32_t               units_ignored;

uint32_t iommu_unit_count(void) { return unit_n; }
uint32_t iommu_host_address_width(void) { return host_addr_width; }

const struct iris_iommu_unit *iommu_unit(uint32_t index) {
    if (index >= unit_n) return 0;
    return &units[index];
}

/* How many device scopes a DRHD names, counted by walking them.
 *
 * Bounded by the DRHD's own length and by each scope's, and a scope whose
 * length is zero ends the walk: a record that does not advance would otherwise
 * be an infinite loop driven by a table the kernel did not write. */
static uint8_t drhd_scope_count(const struct dmar_drhd *d) {
    uint32_t off = (uint32_t)sizeof(*d);
    uint32_t n   = 0;
    while (off + sizeof(struct dmar_scope) <= d->length && n < 255u) {
        const struct dmar_scope *sc =
            (const struct dmar_scope *)((const uint8_t *)d + off);
        if (sc->length == 0u) break;
        if (off + sc->length > d->length) break;
        off += sc->length;
        n++;
    }
    return (uint8_t)n;
}

uint32_t iommu_parse_dmar(uint64_t rsdp_phys) {
    unit_n          = 0;
    host_addr_width = 0;
    units_ignored   = 0;

    uint32_t len = 0;
    const struct dmar_table *t =
        (const struct dmar_table *)acpi_find_table(rsdp_phys, "DMAR", &len);
    if (!t || len < sizeof(*t)) {
        /* No IOMMU on this machine.  Not an error and not a warning: it is the
         * ordinary case for the hardware IRIS is tested on, and the line that
         * matters is the one Stage 10-dma will print when a driver asks for
         * containment that cannot be given. */
        klog_write("[IRIS][IOMMU] no DMAR; DMA is unrestricted on this machine\n");
        return 0;
    }

    host_addr_width = (uint32_t)t->host_address_width + 1u;

    uint32_t off = (uint32_t)sizeof(*t);
    while (off + sizeof(struct dmar_record) <= len) {
        const struct dmar_record *r =
            (const struct dmar_record *)((const uint8_t *)t + off);
        if (r->length < sizeof(*r)) break;        /* a record that cannot advance */
        if (off + r->length > len) break;         /* past the table's own end */

        if (r->type == DMAR_TYPE_DRHD && r->length >= sizeof(struct dmar_drhd)) {
            const struct dmar_drhd *d = (const struct dmar_drhd *)r;
            if (unit_n < IRIS_IOMMU_MAX_UNITS) {
                units[unit_n].reg_base        = d->register_base;
                units[unit_n].segment         = d->segment;
                units[unit_n].include_pci_all = (uint8_t)(d->flags & 1u);
                units[unit_n].scope_count     = drhd_scope_count(d);
                unit_n++;
            } else {
                /* Counted, not dropped quietly.  A device behind a unit this
                 * kernel does not know about is a device with no containment,
                 * and that has to be visible rather than inferred from a count
                 * that stopped going up. */
                units_ignored++;
            }
        }
        off += r->length;
    }

    klog_write("[IRIS][IOMMU] remapping units: ");
    klog_write_dec(unit_n);
    klog_write(" (address width ");
    klog_write_dec(host_addr_width);
    klog_write(" bits");
    if (units_ignored) {
        klog_write(", ");
        klog_write_dec(units_ignored);
        klog_write(" IGNORED - devices behind them are uncontained");
    }
    klog_write(")\n");

    for (uint32_t i = 0; i < unit_n; i++) {
        klog_write("[IRIS][IOMMU]   unit ");
        klog_write_dec(i);
        klog_write(" regs ");
        klog_write_dec(units[i].reg_base);
        klog_write(" segment ");
        klog_write_dec(units[i].segment);
        klog_write(units[i].include_pci_all ? " covers-all" : " scoped");
        klog_write(" scopes ");
        klog_write_dec(units[i].scope_count);
        klog_write("\n");
    }
    return unit_n;
}

/* ── §10.2 step 2: what can each unit actually do? ────────────────────────
 *
 * The DMAR said where the registers are.  This reads them.
 *
 * The registers are reached through the physmap window, exactly as the LAPIC's
 * are: `paging_init` maps the low 4 GiB, and a remapping unit lives at
 * 0xFED90000 on the hardware IRIS is tested on.  No new mapping is needed, and
 * one that had to be made would be a mapping the kernel holds for the life of
 * the system for a device — worth avoiding while the window already exists.
 *
 * Every read is 32 or 64 bits wide and `volatile`: these are device registers,
 * and the compiler must not fold, widen or reorder them.
 */

#define VTD_REG_VER   0x00u   /* 32-bit */
#define VTD_REG_CAP   0x08u   /* 64-bit */
#define VTD_REG_ECAP  0x10u   /* 64-bit */

static uint32_t usable_n;

uint32_t iommu_usable_count(void) { return usable_n; }

static uint32_t vtd_read32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)PHYS_TO_VIRT(base + off);
}

static uint64_t vtd_read64(uint64_t base, uint32_t off) {
    return *(volatile uint64_t *)(uintptr_t)PHYS_TO_VIRT(base + off);
}

/*
 * Decode CAP and ECAP into the fields the later steps ask about.
 *
 * The bit positions are the VT-d specification's and are written out rather
 * than hidden behind macros with the same names, because the only way to check
 * a decode like this is to read it against the document with the raw value in
 * hand — which is why the raw values are kept in the struct too.
 */
static void unit_decode(struct iris_iommu_unit *u) {
    uint64_t cap = u->cap, ecap = u->ecap;

    u->domain_id_bits = (uint8_t)(4u + 2u * (uint32_t)(cap & 0x7u));
    u->rwbf           = (uint8_t)((cap >> 4)  & 1u);
    u->caching_mode   = (uint8_t)((cap >> 7)  & 1u);
    u->sagaw          = (uint8_t)((cap >> 8)  & 0x1Fu);
    u->mgaw_bits      = (uint8_t)(((cap >> 16) & 0x3Fu) + 1u);
    u->fault_offset   = (uint16_t)(((cap >> 24) & 0x3FFu) * 16u);
    u->fault_regs     = (uint8_t)(((cap >> 40) & 0xFFu) + 1u);

    u->coherent     = (uint8_t)(ecap & 1u);
    u->queued_inval = (uint8_t)((ecap >> 1) & 1u);
    u->iotlb_offset = (uint16_t)(((ecap >> 8) & 0x3FFu) * 16u);
}

/*
 * Is this unit what steps 3 and later assume?
 *
 * Two requirements and one non-requirement, and the third is worth stating:
 *
 *   · a page-table depth IRIS can build — 3 levels (39-bit) or 4 (48-bit).
 *     Anything else means writing a second table walker for hardware that is
 *     not in front of us.
 *   · at least one domain id beyond zero, because every device IRIS isolates
 *     needs one and a unit with a single domain cannot isolate anything from
 *     anything.
 *   · queued invalidation is NOT required.  The register-based interface is
 *     always present and is what step 5 will use; a unit that also offers the
 *     queue is not more usable, only faster.
 */
static int unit_is_usable(const struct iris_iommu_unit *u, const char **why) {
    if (!(u->sagaw & (IOMMU_SAGAW_39BIT | IOMMU_SAGAW_48BIT))) {
        *why = "no 3- or 4-level page table support (SAGAW)";
        return 0;
    }
    if (u->domain_id_bits < 2u) {
        *why = "fewer than four domain ids";
        return 0;
    }
    if (u->mgaw_bits < 39u) {
        *why = "guest address width below 39 bits";
        return 0;
    }
    return 1;
}

uint32_t iommu_probe_units(void) {
    usable_n = 0;

    for (uint32_t i = 0; i < unit_n; i++) {
        struct iris_iommu_unit *u = &units[i];
        uint32_t ver = vtd_read32(u->reg_base, VTD_REG_VER);
        u->version_major = (uint8_t)((ver >> 4) & 0xFu);
        u->version_minor = (uint8_t)(ver & 0xFu);
        u->cap  = vtd_read64(u->reg_base, VTD_REG_CAP);
        u->ecap = vtd_read64(u->reg_base, VTD_REG_ECAP);
        u->probed = 1;

        /*
         * An all-ones or all-zero CAP is not a capability register, it is a
         * bus that answered nothing — an unmapped address reads as ~0 and a
         * decoded ~0 says the unit supports everything, which is exactly the
         * wrong conclusion to reach silently.
         */
        if (u->cap == 0u || u->cap == ~0ull || u->ecap == ~0ull) {
            klog_write("[IRIS][IOMMU] unit ");
            klog_write_dec(i);
            klog_write(" did not answer (cap 0x");
            klog_write_hex(u->cap);
            klog_write(") - left alone\n");
            u->usable = 0;
            continue;
        }

        unit_decode(u);

        const char *why = 0;
        u->usable = (uint8_t)unit_is_usable(u, &why);
        if (u->usable) usable_n++;

        klog_write("[IRIS][IOMMU] unit ");
        klog_write_dec(i);
        klog_write(" v");
        klog_write_dec(u->version_major);
        klog_write(".");
        klog_write_dec(u->version_minor);
        klog_write(" cap 0x");
        klog_write_hex(u->cap);
        klog_write(" ecap 0x");
        klog_write_hex(u->ecap);
        klog_write("\n");

        klog_write("[IRIS][IOMMU]   guest-width ");
        klog_write_dec(u->mgaw_bits);
        klog_write(" levels");
        if (u->sagaw & IOMMU_SAGAW_39BIT) klog_write(" 3");
        if (u->sagaw & IOMMU_SAGAW_48BIT) klog_write(" 4");
        klog_write(" domain-bits ");
        klog_write_dec(u->domain_id_bits);
        if (u->caching_mode) klog_write(" caching-mode");
        if (u->rwbf)         klog_write(" write-buffer-flush");
        if (u->coherent)     klog_write(" coherent");
        if (u->queued_inval) klog_write(" queued-invalidation");
        klog_write("\n");

        if (!u->usable) {
            klog_write("[IRIS][IOMMU]   NOT USABLE: ");
            klog_write(why ? why : "unknown");
            klog_write(" - devices behind it are uncontained\n");
        }
    }

    if (unit_n) {
        klog_write("[IRIS][IOMMU] usable units: ");
        klog_write_dec(usable_n);
        klog_write(" of ");
        klog_write_dec(unit_n);
        klog_write("\n");
    }
    return usable_n;
}

/* ── §10.2 step 3: translation on, and nothing permitted ──────────────────
 *
 * The unit is switched into translating mode with a root table in which no
 * entry is present.  Every DMA request then faults on its root entry, which is
 * the property this whole stage exists to establish, standing on its own
 * before any object exists that could relax it.
 *
 * ── The three things that make this harder than "set a bit" ────────────────
 *
 * 1. GCMD IS WRITE-ONLY, AND ONE-SHOT.  The register holds a command, not a
 *    state: software writes the intended value of every bit at once, and the
 *    unit performs whichever one changed.  Reading it back is not defined, so
 *    the enabled bits are SHADOWED here — a read-modify-write on a register
 *    that cannot be read is a bug that works until the second command.
 *
 * 2. THE PAGE WALK IS NOT CACHE-COHERENT on the hardware IRIS is tested on
 *    (ECAP.C == 0, measured — see iris/iommu.h).  A root table written by the
 *    CPU sits in the CPU's cache, and the unit reads memory.  Without a flush
 *    the unit walks whatever was there before, which is not "translations that
 *    do not work" but "translations the hardware never sees" — and a device
 *    that still reaches everything, reported as contained.
 *
 * 3. THE CACHES HAVE TO BE EMPTIED, in order.  The unit caches root and
 *    context entries and separately caches translations; both may hold
 *    whatever the firmware left, and both are invalidated through
 *    register-based commands that must be waited on.
 */

#define VTD_REG_GCMD   0x18u  /* 32-bit, WRITE-ONLY */
#define VTD_REG_GSTS   0x1Cu  /* 32-bit */
#define VTD_REG_RTADDR 0x20u  /* 64-bit */
#define VTD_REG_CCMD   0x28u  /* 64-bit */

#define VTD_GCMD_TE    (1u << 31)  /* translation enable */
#define VTD_GCMD_SRTP  (1u << 30)  /* set root table pointer */
#define VTD_GCMD_WBF   (1u << 27)  /* write buffer flush */

#define VTD_GSTS_TES   (1u << 31)  /* translation enabled status */
#define VTD_GSTS_RTPS  (1u << 30)  /* root table pointer status */
#define VTD_GSTS_WBFS  (1u << 27)

#define VTD_CCMD_ICC   (1ull << 63)     /* invalidate context cache */
#define VTD_CCMD_GLOBAL (1ull << 61)    /* CIRG = global */

#define VTD_IOTLB_IVT    (1ull << 63)   /* invalidate IOTLB */
#define VTD_IOTLB_GLOBAL (1ull << 60)   /* IIRG = global */

/* One 4 KiB root table per unit, and the command bits currently set in GCMD.
 *
 * Static rather than allocated: there are at most IRIS_IOMMU_MAX_UNITS of
 * them, they live for the life of the machine, and a page the kernel takes
 * from the PMM at boot for a device is a page the PMM can never hand back.
 * Sixteen kilobytes of BSS against an allocation that has no free is the
 * cheaper trade, and it also means this path reaches no allocator at all —
 * which is what keeps the purity gate's answer unchanged. */
static uint64_t root_tables[IRIS_IOMMU_MAX_UNITS][512]
    __attribute__((aligned(4096)));
static uint32_t gcmd_shadow[IRIS_IOMMU_MAX_UNITS];
static uint32_t enabled_n;

uint32_t iommu_enabled_count(void) { return enabled_n; }

int iommu_dma_is_contained(void) {
    return unit_n != 0u && enabled_n == unit_n;
}

static void vtd_write32(uint64_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)PHYS_TO_VIRT(base + off) = v;
}

static void vtd_write64(uint64_t base, uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(uintptr_t)PHYS_TO_VIRT(base + off) = v;
}

/*
 * Push a range out of the CPU's caches so the unit can see it.
 *
 * Only when the unit says its walk is not coherent.  `clflush` takes one
 * cache line at a time and 64 bytes is the smallest line any x86-64 part has,
 * so stepping by 64 covers every line of the range on every part — stepping by
 * the reported line size would be the same loop with one more thing that can
 * be wrong.  The fence after it is what orders the flushes before the register
 * write that tells the unit to read.
 */
static void iommu_flush_for_device(const struct iris_iommu_unit *u,
                                   const void *addr, uint64_t len) {
    if (u->coherent) return;
    const uint8_t *p = (const uint8_t *)addr;
    for (uint64_t off = 0; off < len; off += 64u)
        __asm__ volatile ("clflush (%0)" :: "r"(p + off) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

/* Wait for a status bit, bounded.  A unit that never answers is reported and
 * left alone rather than spun on for ever: unlike a TLB shootdown, where the
 * only alternative to waiting is freeing memory another core can still write,
 * here the honest outcome is "this unit is not translating" — which the caller
 * turns into "the devices behind it are uncontained". */
static int vtd_wait_status(uint64_t base, uint32_t mask, int want_set) {
    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        uint32_t sts = vtd_read32(base, VTD_REG_GSTS);
        if (want_set ? (sts & mask) : !(sts & mask)) return 1;
        __asm__ volatile ("pause");
    }
    return 0;
}

/* Issue a command by writing the shadow plus one bit.  See the note above on
 * why GCMD cannot be read back. */
static int vtd_command(struct iris_iommu_unit *u, uint32_t idx,
                       uint32_t bit, uint32_t status_bit, int want_set) {
    if (want_set) gcmd_shadow[idx] |= bit;
    else          gcmd_shadow[idx] &= ~bit;
    vtd_write32(u->reg_base, VTD_REG_GCMD, gcmd_shadow[idx]);
    /* SRTP and WBF are self-clearing: the shadow must not keep them, or the
     * next command would ask for them again. */
    if (bit == VTD_GCMD_SRTP || bit == VTD_GCMD_WBF) gcmd_shadow[idx] &= ~bit;
    return vtd_wait_status(u->reg_base, status_bit, want_set);
}

/* Empty the unit's context cache and its IOTLB, globally.
 *
 * Both are register-based commands: set the request bit and wait for the unit
 * to clear it.  Queued invalidation would be faster and is not used — the
 * register interface is always present, and step 5 is where a faster one earns
 * its place. */
static int vtd_invalidate_all(const struct iris_iommu_unit *u) {
    vtd_write64(u->reg_base, VTD_REG_CCMD, VTD_CCMD_ICC | VTD_CCMD_GLOBAL);
    for (uint32_t spin = 0; ; spin++) {
        if (!(vtd_read64(u->reg_base, VTD_REG_CCMD) & VTD_CCMD_ICC)) break;
        if (spin > 10000000u) return 0;
        __asm__ volatile ("pause");
    }

    /* The IOTLB registers are not at a fixed offset: the unit reports where
     * they are in ECAP.IRO, and the command register is the second 64-bit
     * word of that block. */
    uint32_t iotlb_cmd = (uint32_t)u->iotlb_offset + 8u;
    vtd_write64(u->reg_base, iotlb_cmd, VTD_IOTLB_IVT | VTD_IOTLB_GLOBAL);
    for (uint32_t spin = 0; ; spin++) {
        if (!(vtd_read64(u->reg_base, iotlb_cmd) & VTD_IOTLB_IVT)) break;
        if (spin > 10000000u) return 0;
        __asm__ volatile ("pause");
    }
    return 1;
}

/*
 * Has the unit RECORDED a translation fault?
 *
 * This is the only evidence available in a test environment that the
 * enforcement is real rather than a bit that was set and does nothing.  A
 * device that issued a DMA request while translating was on, and was refused,
 * leaves a record here — so a non-zero count is proof the unit is enforcing,
 * and a zero is consistent with nothing having tried.
 *
 * Reported rather than acted on.  A fault is a fact about a DEVICE somebody
 * else owns, and the kernel deciding what to do about it would be the kernel
 * holding a policy about hardware it does not drive.
 */
#define VTD_REG_FSTS 0x34u   /* 32-bit: fault status */

uint32_t iommu_fault_status(uint32_t index) {
    if (index >= unit_n || !units[index].translating) return 0;
    return vtd_read32(units[index].reg_base, VTD_REG_FSTS);
}

/*
 * The fault RECORDING registers, which are where the detail lives.
 *
 * Each record is 128 bits at CAP.FRO + n*16.  The high half carries the
 * source-id in its low 16 bits, the reason code at 39:32, the request type at
 * bit 62 (1 = read) and the valid bit at 63; the low half carries the address
 * the device asked for, in its top 52 bits.  Bit 63 is write-1-to-clear, and
 * so is FSTS.PFO — the unit stops recording once the ring is full of records
 * nobody drained, so a reader that wants the NEXT fault has to clear this one.
 *
 * The ring is scanned from the bottom rather than indexed by FSTS.FRI: FRI is
 * only meaningful while PPF is set, and a caller polling for a fault that has
 * not happened yet would read whatever index was left over from the last one.
 * Scanning costs a handful of uncached reads and cannot be wrong.
 */
int iommu_fault_record(uint32_t index, struct iris_iommu_fault *out, int clear) {
    if (!out) return 0;
    out->address = 0; out->source_id = 0; out->reason = 0; out->is_read = 0;
    out->status = 0;
    if (index >= unit_n || !units[index].translating) return 0;

    struct iris_iommu_unit *u = &units[index];
    out->status = vtd_read32(u->reg_base, VTD_REG_FSTS);

    for (uint32_t n = 0; n < u->fault_regs; n++) {
        uint32_t off  = (uint32_t)u->fault_offset + n * 16u;
        uint64_t high = vtd_read64(u->reg_base, off + 8u);
        if (!(high & (1ull << 63))) continue;          /* no record here */

        out->address   = vtd_read64(u->reg_base, off) & ~0xFFFull;
        out->source_id = (uint16_t)(high & 0xFFFFu);
        out->reason    = (uint8_t)((high >> 32) & 0xFFu);
        out->is_read   = (uint8_t)((high >> 62) & 1u);

        if (clear) {
            /* The record first, then the summary bit it raised.  The other
             * way round re-raises PPF the moment the record is still set. */
            vtd_write64(u->reg_base, off + 8u, 1ull << 63);
            vtd_write32(u->reg_base, VTD_REG_FSTS, out->status & 0x3u);
        }
        return 1;
    }
    return 0;
}

uint32_t iommu_enable_blocking(void) {
    enabled_n = 0;

    for (uint32_t i = 0; i < unit_n; i++) {
        struct iris_iommu_unit *u = &units[i];
        if (!u->usable) continue;

        /* A root table with no entry present.  Every DMA request stops here. */
        for (uint32_t e = 0; e < 512u; e++) root_tables[i][e] = 0;
        iommu_flush_for_device(u, root_tables[i], sizeof(root_tables[i]));

        uint64_t root_phys = paging_virt_to_phys((uint64_t)(uintptr_t)root_tables[i]);
        if (!root_phys) {
            klog_write("[IRIS][IOMMU] unit ");
            klog_write_dec(i);
            klog_write(" root table has no physical address - NOT enabled\n");
            continue;
        }

        gcmd_shadow[i] = 0;

        /* The write buffer, when the unit says it has one that matters. */
        if (u->rwbf && !vtd_command(u, i, VTD_GCMD_WBF, VTD_GSTS_WBFS, 1)) {
            klog_write("[IRIS][IOMMU] unit ");
            klog_write_dec(i);
            klog_write(" write-buffer flush timed out - NOT enabled\n");
            continue;
        }

        /* Root table pointer, then the command that makes the unit read it.
         * Legacy mode: the low bits of RTADDR select the table format and 0 is
         * the root/context pair this kernel builds. */
        vtd_write64(u->reg_base, VTD_REG_RTADDR, root_phys);
        if (!vtd_command(u, i, VTD_GCMD_SRTP, VTD_GSTS_RTPS, 1)) {
            klog_write("[IRIS][IOMMU] unit ");
            klog_write_dec(i);
            klog_write(" root pointer not accepted - NOT enabled\n");
            continue;
        }

        /* Whatever the firmware left cached is not ours. */
        if (!vtd_invalidate_all(u)) {
            klog_write("[IRIS][IOMMU] unit ");
            klog_write_dec(i);
            klog_write(" invalidation timed out - NOT enabled\n");
            continue;
        }

        if (!vtd_command(u, i, VTD_GCMD_TE, VTD_GSTS_TES, 1)) {
            klog_write("[IRIS][IOMMU] unit ");
            klog_write_dec(i);
            klog_write(" translation would not enable - NOT enabled\n");
            continue;
        }

        u->translating = 1;
        enabled_n++;
    }

    if (unit_n) {
        klog_write("[IRIS][IOMMU] translating units: ");
        klog_write_dec(enabled_n);
        klog_write(" of ");
        klog_write_dec(unit_n);
        klog_write(enabled_n == unit_n ? " - DMA is contained"
                                       : " - SOME DEVICES ARE UNCONTAINED");
        klog_write("\n");
    }
    return enabled_n;
}

/* ── §10.2 step 4: what the object layer asks of the hardware ──────────────
 *
 * The IOSpace is a capability and lives in `new_core`; the remapping unit is a
 * device and lives here.  Everything below is the line between them, and it is
 * deliberately narrow: the object layer never writes a register and this layer
 * never holds a capability.
 */

/*
 * The context tables, from a POOL claimed at boot.
 *
 * A context table is 4 KiB per (unit, PCI bus), and a machine has 256 buses
 * per unit — a megabyte each, nearly all of it empty on any real machine.  So
 * they are claimed on demand from a fixed pool, and a bus that finds the pool
 * empty is REFUSED.
 *
 * A bounded pool with a visible refusal, rather than an allocation, because
 * this runs on a syscall path: the kernel reaching an allocator from a
 * capability invocation is the thing charter M1 and the purity gate exist to
 * prevent.  The bound is declared here and the refusal names itself, which is
 * what makes "IRIS supports eight device buses" a fact somebody can read
 * instead of a limit somebody discovers.
 */
#define IOMMU_CONTEXT_TABLES 8u

static uint64_t context_tables[IOMMU_CONTEXT_TABLES][512]
    __attribute__((aligned(4096)));
static uint32_t context_unit[IOMMU_CONTEXT_TABLES];
static uint16_t context_bus[IOMMU_CONTEXT_TABLES];
static uint8_t  context_used[IOMMU_CONTEXT_TABLES];

/*
 * Translation domains, as a bitmap per unit.
 *
 * The unit reports how many it has (16 bits' worth on the tested hardware, so
 * 65536).  IRIS uses 64, which is a declared bound for the same reason the
 * context pool is one: a device that cannot get a domain is refused rather
 * than silently sharing another device's translations, which would be two
 * devices in one address space and the containment claim quietly false.
 */
#define IOMMU_DOMAINS 64u
static uint64_t domain_bitmap[IRIS_IOMMU_MAX_UNITS];  /* bit d set = in use */

uint32_t iommu_levels(uint32_t unit) {
    if (unit >= unit_n) return 0;
    /* Prefer the shallower table the unit supports: fewer levels is fewer
     * objects the holder has to pay for, and a 39-bit DMA address space is
     * five hundred gigabytes. */
    if (units[unit].sagaw & IOMMU_SAGAW_39BIT) return 3u;
    if (units[unit].sagaw & IOMMU_SAGAW_48BIT) return 4u;
    return 0u;
}

uint32_t iommu_unit_for_source(uint16_t source_id) {
    (void)source_id;
    /*
     * Which unit claims this device?
     *
     * A DRHD with INCLUDE_PCI_ALL takes every device in its segment that no
     * other unit names by scope.  That is the easy case and the first branch.
     *
     * The second branch is the one worth defending, because it looks like a
     * guess and is not.  QEMU's DRHD has the flag CLEAR and names six scopes,
     * none of them the device a driver would drive — so on the machine IRIS is
     * tested against, the table never says which unit covers a given endpoint.
     *
     * With exactly ONE remapping unit there is nothing to be uncertain about.
     * A remapping unit is the hardware every DMA request in its segment passes
     * through; if there is one, every device's requests go through it, whatever
     * the table's scope list happens to enumerate.  Attributing a device to the
     * only unit that could possibly translate it is not hope, it is the only
     * arrangement the hardware admits.
     *
     * With MORE than one and no INCLUDE_PCI_ALL, that argument disappears: the
     * scope list is the only thing that says which unit sees which device, and
     * IRIS records the scope COUNT but not the paths.  So it refuses.  An
     * IOSpace installed on the wrong unit is a capability that promises
     * containment the hardware never enforces — present in the books, absent
     * in fact — and a refusal is the only other honest answer.  Recording the
     * scope paths at parse time is what lifts this, and it is a change to the
     * parser rather than to this decision.
     */
    uint32_t only = IOMMU_NO_UNIT;
    uint32_t n    = 0;

    for (uint32_t i = 0; i < unit_n; i++) {
        if (!units[i].translating) continue;
        if (units[i].include_pci_all) return i;
        only = i;
        n++;
    }
    return (n == 1u) ? only : IOMMU_NO_UNIT;
}

uint16_t iommu_domain_claim(uint32_t unit) {
    if (unit >= unit_n) return 0;
    /* Domain 0 is reserved by the specification, and skipping it also means a
     * "no domain free" answer of 0 can never be mistaken for a real one. */
    for (uint16_t d = 1; d < IOMMU_DOMAINS; d++) {
        if (domain_bitmap[unit] & (1ull << d)) continue;
        domain_bitmap[unit] |= (1ull << d);
        return d;
    }
    return 0;
}

void iommu_domain_release(uint32_t unit, uint16_t domain) {
    if (unit >= unit_n || domain == 0u || domain >= IOMMU_DOMAINS) return;
    domain_bitmap[unit] &= ~(1ull << domain);
}

void iommu_flush_tables(uint32_t unit, const void *addr, uint64_t len) {
    if (unit >= unit_n) return;
    iommu_flush_for_device(&units[unit], addr, len);
}

void iommu_invalidate_iotlb(uint32_t unit) {
    if (unit >= unit_n || !units[unit].translating) return;
    (void)vtd_invalidate_all(&units[unit]);
}

/* The context table for (unit, bus), claiming one from the pool if this is the
 * first device on that bus.  NULL when the pool is spent. */
static uint64_t *context_table_for(uint32_t unit, uint16_t bus) {
    for (uint32_t i = 0; i < IOMMU_CONTEXT_TABLES; i++)
        if (context_used[i] && context_unit[i] == unit && context_bus[i] == bus)
            return context_tables[i];

    for (uint32_t i = 0; i < IOMMU_CONTEXT_TABLES; i++) {
        if (context_used[i]) continue;
        for (uint32_t e = 0; e < 512u; e++) context_tables[i][e] = 0;
        context_used[i] = 1;
        context_unit[i] = unit;
        context_bus[i]  = bus;
        return context_tables[i];
    }
    return 0;
}

int iommu_context_set(uint32_t unit, uint16_t source_id, uint16_t domain,
                      uint64_t root_pt_phys, uint32_t levels) {
    if (unit >= unit_n || !units[unit].translating) return 0;

    uint16_t bus  = (uint16_t)(source_id >> 8);
    uint32_t devfn = (uint32_t)(source_id & 0xFFu);

    uint64_t *ctx = context_table_for(unit, bus);
    if (!ctx) return 0;                    /* the pool is spent; say so */

    /*
     * A context entry is two 64-bit words.  The low one carries Present, the
     * translation type and the page-table pointer; the high one the address
     * width and the domain id.  Written high word FIRST: a unit that saw
     * Present set with a stale domain would translate into another device's
     * address space for as long as the gap lasted.
     */
    if (root_pt_phys) {
        uint32_t aw = levels - 2u;        /* AW 1 = 39-bit/3-level, 2 = 48/4 */
        ctx[devfn * 2u + 1u] = ((uint64_t)domain << 8) | (uint64_t)aw;
        ctx[devfn * 2u]      = (root_pt_phys & PAGE_PA_MASK) | 1ull;  /* Present */
    } else {
        ctx[devfn * 2u]      = 0;         /* not present: the device is blocked */
        ctx[devfn * 2u + 1u] = 0;
    }
    iommu_flush_for_device(&units[unit], &ctx[devfn * 2u], 16u);

    /* The root entry for this bus, installed the first time a device on it
     * gets a context table.  Same ordering rule: the pointer before Present. */
    uint64_t ctx_phys = paging_virt_to_phys((uint64_t)(uintptr_t)ctx);
    if (!ctx_phys) return 0;
    uint64_t *root = root_tables[unit];
    if (!(root[bus * 2u] & 1ull)) {
        root[bus * 2u + 1u] = 0;
        root[bus * 2u]      = (ctx_phys & PAGE_PA_MASK) | 1ull;
        iommu_flush_for_device(&units[unit], &root[bus * 2u], 16u);
    }

    /* The unit caches both, and neither cache knows what just changed. */
    return vtd_invalidate_all(&units[unit]);
}
