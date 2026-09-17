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
