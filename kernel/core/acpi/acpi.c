/*
 * acpi.c — RSDP to the two tables the kernel reads, and nothing else.  See
 * iris/acpi.h for why the "nothing else" is the design rather than an
 * omission.
 *
 * MADT (SMP roadmap §9.3): how many processors, and their LAPIC ids.
 * DMAR (§10.2 step 1): where the DMA remapping units are.
 *
 * Both are read ONCE, at boot, into a static array.  Nothing here runs
 * afterwards and nothing here touches hardware — finding a unit and using one
 * are different steps, and they live in different files for that reason.
 */

#include <iris/acpi.h>
#include <iris/cpu_local.h>
#include <iris/paging.h>
#include <iris/klog.h>

struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* over the first 20 bytes */
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = 2.0+ */
    uint32_t rsdt_address;
    /* 2.0+ only, and only valid when revision >= 2: */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum; /* over `length` bytes */
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_address;
    uint32_t flags;
    /* followed by variable-length entries */
} __attribute__((packed));

struct madt_entry_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_TYPE_LAPIC       0
#define MADT_TYPE_X2APIC      9
#define MADT_LAPIC_ENABLED    (1u << 0)
/* "Online capable": the processor is not usable now but may be hot-added.
 * IRIS does not hot-add, so it is counted as present and NOT enabled. */
#define MADT_LAPIC_ONLINE_CAP (1u << 1)

struct madt_lapic {
    struct madt_entry_hdr hdr;
    uint8_t  acpi_proc_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_x2apic {
    struct madt_entry_hdr hdr;
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t acpi_proc_uid;
} __attribute__((packed));

static struct iris_cpu_desc acpi_cpus[MAX_CPUS];
static uint32_t             acpi_cpu_n;
static uint64_t             acpi_lapic_phys;

uint32_t acpi_cpu_count(void) { return acpi_cpu_n; }
uint64_t acpi_lapic_base(void) { return acpi_lapic_phys; }

const struct iris_cpu_desc *acpi_cpu(uint32_t index) {
    return (index < acpi_cpu_n) ? &acpi_cpus[index] : 0;
}

/* Every ACPI table is checksummed by summing its bytes to zero.  A table that
 * does not is firmware saying "do not trust me", and the honest response is to
 * ignore it rather than to act on numbers that may be garbage. */
static int acpi_checksum_ok(const void *p, uint32_t len) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0u;
}

static const void *phys_ptr(uint64_t phys) {
    return (const void *)(uintptr_t)PHYS_TO_VIRT(phys);
}

static void madt_add(uint32_t apic_id, uint32_t flags) {
    if (acpi_cpu_n >= MAX_CPUS) return;      /* more CPUs than IRIS supports */
    if (apic_id > 0xFFu) return;             /* x2APIC ids beyond 8 bits: the
                                              * IPI path addresses by 8-bit
                                              * destination, so an id it cannot
                                              * name is one it cannot start */
    acpi_cpus[acpi_cpu_n].lapic_id = (uint8_t)apic_id;
    acpi_cpus[acpi_cpu_n].enabled  = (flags & MADT_LAPIC_ENABLED) ? 1u : 0u;
    acpi_cpu_n++;
}

static void madt_walk(const struct acpi_madt *madt) {
    acpi_lapic_phys = (uint64_t)madt->lapic_address;

    const uint8_t *p   = (const uint8_t *)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t *)madt + madt->hdr.length;

    while (p + sizeof(struct madt_entry_hdr) <= end) {
        const struct madt_entry_hdr *e = (const struct madt_entry_hdr *)p;
        /* A zero length would spin here forever, and a table that says so is
         * malformed rather than interesting. */
        if (e->length < sizeof(*e) || p + e->length > end) break;

        if (e->type == MADT_TYPE_LAPIC && e->length >= sizeof(struct madt_lapic)) {
            const struct madt_lapic *l = (const struct madt_lapic *)p;
            madt_add(l->apic_id, l->flags);
        } else if (e->type == MADT_TYPE_X2APIC &&
                   e->length >= sizeof(struct madt_x2apic)) {
            const struct madt_x2apic *x = (const struct madt_x2apic *)p;
            madt_add(x->x2apic_id, x->flags);
        }
        p += e->length;
    }
}

/* Walk an RSDT (32-bit entries) or XSDT (64-bit) looking for one signature.
 *
 * It was `find_madt`, hard-coded to "APIC", and the second caller is what made
 * the signature a parameter.  Everything else about the walk — the checksums,
 * the two entry widths — is the same question asked about a different four
 * bytes. */
static const struct acpi_sdt_header *find_table(uint64_t sdt_phys, int is_xsdt,
                                                const char *sig) {
    if (!sdt_phys) return 0;
    const struct acpi_sdt_header *sdt = phys_ptr(sdt_phys);
    if (sdt->length < sizeof(*sdt)) return 0;
    if (!acpi_checksum_ok(sdt, sdt->length)) return 0;

    uint32_t entry_bytes = is_xsdt ? 8u : 4u;
    uint32_t n = (sdt->length - (uint32_t)sizeof(*sdt)) / entry_bytes;
    const uint8_t *ents = (const uint8_t *)sdt + sizeof(*sdt);

    for (uint32_t i = 0; i < n; i++) {
        uint64_t phys;
        if (is_xsdt) {
            uint64_t v = 0;
            for (int b = 0; b < 8; b++) v |= (uint64_t)ents[i * 8 + b] << (8 * b);
            phys = v;
        } else {
            uint32_t v = 0;
            for (int b = 0; b < 4; b++) v |= (uint32_t)ents[i * 4 + b] << (8 * b);
            phys = v;
        }
        if (!phys) continue;
        const struct acpi_sdt_header *h = phys_ptr(phys);
        if (h->signature[0] == sig[0] && h->signature[1] == sig[1] &&
            h->signature[2] == sig[2] && h->signature[3] == sig[3]) {
            if (h->length >= sizeof(*h) && acpi_checksum_ok(h, h->length))
                return h;
        }
    }
    return 0;
}

/*
 * RSDP → the table with this signature, or NULL.
 *
 * The RSDP validation used to be inline in `acpi_parse_madt`; it is here
 * because a second table needs exactly the same four checks and duplicating
 * them is how the two would eventually disagree about which one is valid.
 */
static const struct acpi_sdt_header *acpi_find(uint64_t rsdp_phys, const char *sig) {
    if (!rsdp_phys) return 0;

    const struct acpi_rsdp *r = phys_ptr(rsdp_phys);
    if (r->signature[0] != 'R' || r->signature[1] != 'S' ||
        r->signature[2] != 'D' || r->signature[3] != ' ' ||
        r->signature[4] != 'P' || r->signature[5] != 'T' ||
        r->signature[6] != 'R' || r->signature[7] != ' ') return 0;
    if (!acpi_checksum_ok(r, 20u)) return 0;

    const struct acpi_sdt_header *h = 0;
    /* Prefer the XSDT: it is the only one that can describe a table above
     * 4 GiB, and a machine that has both is telling us the 64-bit one is
     * authoritative. */
    if (r->revision >= 2 && acpi_checksum_ok(r, r->length))
        h = find_table(r->xsdt_address, 1, sig);
    if (!h)
        h = find_table((uint64_t)r->rsdt_address, 0, sig);
    return h;
}

/*
 * The walk, for the one other table the kernel reads.
 *
 * Exposed as bytes-and-a-length rather than as a struct: this file knows how
 * to FIND a table and validate it, and the caller knows what its table means.
 * Putting the DMAR's layout here too would make acpi.c the place every future
 * table's interpretation accretes, which is the thing iris/acpi.h says it is
 * not.
 */
const void *acpi_find_table(uint64_t rsdp_phys, const char *sig, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    const struct acpi_sdt_header *h = acpi_find(rsdp_phys, sig);
    if (!h) return 0;
    if (out_len) *out_len = h->length;
    return h;
}

uint32_t acpi_parse_madt(uint64_t rsdp_phys) {
    acpi_cpu_n = 0;
    acpi_lapic_phys = 0;

    if (!rsdp_phys) {
        klog_write("[IRIS][ACPI] firmware published no RSDP\n");
        return 0;
    }

    const struct acpi_sdt_header *h = acpi_find(rsdp_phys, "APIC");
    const struct acpi_madt *madt =
        (h && h->length >= sizeof(struct acpi_madt)) ? (const struct acpi_madt *)h : 0;

    if (!madt) {
        klog_write("[IRIS][ACPI] no MADT\n");
        return 0;
    }

    madt_walk(madt);

    klog_write("[IRIS][ACPI] processors: ");
    klog_write_dec(acpi_cpu_n);
    klog_write("\n");
    return acpi_cpu_n;
}
