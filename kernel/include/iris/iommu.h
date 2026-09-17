#ifndef IRIS_IOMMU_H
#define IRIS_IOMMU_H

/*
 * iommu.h — DMA remapping (Stage 10-dma).
 *
 * ── Why this exists at all ──────────────────────────────────────────────────
 *
 * A driver in IRIS holds an I/O port capability and an IRQ capability and
 * nothing else, and the claim that goes with that is containment: a driver
 * that is compromised can reach only what its capabilities name.  Against a
 * DMA-capable device that claim is FICTION.  The driver programs the device
 * with a physical address and the device writes there — through no page table,
 * past every check the kernel makes, into the kernel's own memory if it likes.
 *
 * An IOMMU is the page table for that path.  With one, a device's reach is
 * exactly the set of frames somebody mapped for it; without one, a device's
 * reach is all of memory and every other guarantee in this kernel is void
 * against a driver that misbehaves.
 *
 * ── What the kernel reads, and what it deliberately does not ────────────────
 *
 * It reads the DMAR table to learn WHERE the remapping units are.  It does not
 * enumerate PCI.  To install a translation for a device you need its
 * source-id — the bus:device:function that rides on every DMA request it makes
 * — and you do not need to have DISCOVERED that: the source-id is a PARAMETER
 * of the authority, travelling on the capability, exactly as an I/O port range
 * travels on an IOPORT_CONTROL derivation.  seL4 is built the same way; a root
 * task that scans the bus asks for an IOSpace for a device it found.
 *
 * So PCI enumeration is what ring 3 needs to make this USEFUL.  It is not what
 * the kernel needs to make it SAFE, and a kernel that grew a bus scanner would
 * be bending charter P1/P2 for nothing.
 *
 * ── Where the line between this file and acpi.c falls ───────────────────────
 *
 * `acpi.c` READS the table.  This TALKS to the hardware.  Finding a unit and
 * using one are separate claims that can fail separately, which is the same
 * reason SMP kept "the processors are up" apart from "the processors schedule".
 */

#include <stdint.h>

/* How many remapping units this kernel will describe.  A machine with more is
 * reported and the extra ones ignored rather than silently half-handled: a
 * device behind a unit IRIS does not know about is a device with no
 * containment, and that must be visible. */
#define IRIS_IOMMU_MAX_UNITS 8u

/*
 * One DMA Remapping Hardware Unit, as the DMAR described it.
 *
 * `include_pci_all` is the DRHD flag that says this unit covers every PCI
 * device in its segment that no other unit claims by scope.
 *
 * MEASURED, not assumed: QEMU's `-device intel-iommu` emits a 64-byte DRHD
 * with this flag CLEAR and six explicit device scopes.  That matters for the
 * step that grants a device its address space — with the flag clear, a
 * source-id no scope names is a source-id this unit does not claim, and the
 * honest answer to a request for its IOSpace is a refusal rather than a
 * translation installed on a guess.  The field is recorded so that decision
 * can be made from the table rather than from a belief about it.
 */
struct iris_iommu_unit {
    uint64_t reg_base;        /* physical base of the unit's registers */
    uint16_t segment;         /* PCI segment it belongs to */
    uint8_t  include_pci_all; /* DRHD flags bit 0 */
    uint8_t  scope_count;     /* device scopes named explicitly */

    /* ── what the unit says it can do (§10.2 step 2) ──────────────────────
     *
     * Filled by `iommu_probe_units`, which is a separate call from the DMAR
     * parse for the same reason the DMAR parse is separate from using the
     * hardware: reading a table and touching a device are different claims,
     * and a machine where the second fails should not look like a machine
     * where the first did.
     *
     * `usable` is the one field the later steps consult.  Everything else is
     * here so that a refusal can say WHICH assumption the hardware broke —
     * "the IOMMU is not usable" is a sentence that helps nobody. */
    uint8_t  probed;          /* the registers were read at all */
    uint8_t  usable;          /* ...and they are what steps 3+ require */
    uint8_t  version_major;
    uint8_t  version_minor;
    uint64_t cap;             /* raw, so a reader can check the decode */
    uint64_t ecap;
    uint8_t  sagaw;           /* supported adjusted guest address widths, bitmask */
    uint8_t  mgaw_bits;       /* maximum guest address width, in bits */
    uint8_t  domain_id_bits;  /* how many bits of domain id the unit has */
    uint8_t  caching_mode;    /* CAP.CM: non-present entries are cached too */
    uint8_t  rwbf;            /* CAP.RWBF: the write buffer must be flushed */
    uint8_t  coherent;        /* ECAP.C: the unit's page walk snoops the cache */
    uint8_t  queued_inval;    /* ECAP.QI */
    uint16_t iotlb_offset;    /* ECAP.IRO, already in bytes */
    uint16_t fault_offset;    /* CAP.FRO, already in bytes */
    uint8_t  fault_regs;      /* CAP.NFR + 1 */
    uint8_t  translating;     /* §10.2 step 3: the unit is enforcing */
};

/*
 * MEASURED on the hardware IRIS is tested against, because both facts decide
 * how the next step has to be written:
 *
 *   · QEMU's unit offers SAGAW bit 1 only — THREE levels, a 39-bit DMA address
 *     space.  A walker written for four levels would build a table this unit
 *     reads as garbage.
 *   · ECAP.C is 0: the page walk is NOT cache-coherent.  Every root, context
 *     and page-table line the kernel writes must be flushed out of the CPU
 *     cache before the unit reads it.  Skipping that installs translations the
 *     hardware never sees — and the symptom is a device that still reaches
 *     everything, which is protection that is not there being reported as
 *     present.
 */

/* SAGAW bits, as the specification numbers them: bit N means "this unit
 * supports a page table with N+2 levels".  IRIS needs one of the two middle
 * ones; 30-bit (2-level) cannot address enough and 57-bit (5-level) is not
 * something the tested hardware offers. */
#define IOMMU_SAGAW_39BIT (1u << 1)   /* 3 levels */
#define IOMMU_SAGAW_48BIT (1u << 2)   /* 4 levels */

/*
 * Parse the DMAR from the RSDP BootInfo forwarded.  Safe to call with 0.
 * Returns how many remapping units were found — 0 on a machine with no IOMMU,
 * which is a machine IRIS runs on exactly as it did before this existed.
 *
 * Nothing is mapped and nothing is enabled by this call.
 */
uint32_t iommu_parse_dmar(uint64_t rsdp_phys);

uint32_t iommu_unit_count(void);
const struct iris_iommu_unit *iommu_unit(uint32_t index);

/* The DMAR's host address width field, as "how many address bits the DMA path
 * carries" (the table stores it as width-1).  0 when there is no DMAR. */
uint32_t iommu_host_address_width(void);

/*
 * Read every found unit's capability registers and decide whether it is what
 * the later steps require (§10.2 step 2).  Returns how many are USABLE.
 *
 * A unit that is not usable is reported with the reason and left alone.  A
 * translation built on a guess about the hardware is worse than no
 * translation, because it looks like protection.
 */
uint32_t iommu_probe_units(void);

/* How many units are usable — 0 until `iommu_probe_units` has run. */
uint32_t iommu_usable_count(void);

/*
 * Turn translation ON, with nothing permitted (§10.2 step 3).
 *
 * Every usable unit is given a root table in which NO entry is present, and
 * then switched into translating mode.  A DMA request from any device then
 * faults on its root entry and is refused by the hardware — which is §10.1's
 * property standing on its own, before any object exists that could relax it:
 * a device whose source-id no capability names reaches nothing.
 *
 * An empty root table is also the cheapest default there is.  One 4 KiB page
 * per unit, no context tables at all: a request from any bus stops at the root
 * entry, so building 256 context tables to hold nothing would be the same
 * answer at a thousand times the memory.
 *
 * Returns how many units are now translating.  A unit that cannot be switched
 * is reported and left alone — the devices behind it keep their unrestricted
 * reach, and that has to be said rather than inferred from a number.
 *
 * Call it LAST in boot.  The firmware's devices do DMA, and the whole point of
 * this call is to stop DMA that nobody authorised.
 */
uint32_t iommu_enable_blocking(void);

/* How many units are translating — 0 until `iommu_enable_blocking` has run. */
uint32_t iommu_enabled_count(void);

/*
 * A unit's fault status register, or 0.
 *
 * The only evidence a test environment offers that the enforcement is real
 * rather than a bit that was set and does nothing: a device refused while
 * translating was on leaves a record here.  A non-zero value proves the unit
 * is enforcing; a zero is consistent with nothing having tried.  Reported, not
 * acted on — a fault is a fact about a device somebody else owns.
 */
uint32_t iommu_fault_status(uint32_t index);

/*
 * ── What the object layer asks of the hardware layer (§10.2 step 4) ────────
 *
 * The IOSpace and its page tables are capability objects and live in
 * `new_core`; the remapping unit is hardware and lives here.  These four calls
 * are the whole of the line between them.
 */

/* Which unit claims this source-id, or IOMMU_NO_UNIT.
 *
 * "Claims" is read from the DMAR, not assumed.  A unit with INCLUDE_PCI_ALL
 * takes everything in its segment; one without takes only what its device
 * scopes name.  A source-id nobody claims gets no IOSpace — a capability that
 * promises containment nothing enforces is worse than a refusal. */
#define IOMMU_NO_UNIT 0xFFFFFFFFu
uint32_t iommu_unit_for_source(uint16_t source_id);

/* How many page-table levels that unit's translation needs (3 or 4). */
uint32_t iommu_levels(uint32_t unit);

/* Claim a translation domain on a unit, or 0 if none is free.  Domain 0 is
 * never handed out: the specification reserves it, and a bug that returns
 * "no domain" as 0 would otherwise install one device's translations under
 * another's. */
uint16_t iommu_domain_claim(uint32_t unit);
void     iommu_domain_release(uint32_t unit, uint16_t domain);

/* Install (or remove) the context entry that points a device at a page table.
 * `root_pt_phys` of 0 removes it, which is what makes the device blocked
 * again.  Flushes what has to be flushed and invalidates what has to be
 * invalidated; returns non-zero on success. */
int iommu_context_set(uint32_t unit, uint16_t source_id, uint16_t domain,
                      uint64_t root_pt_phys, uint32_t levels);

/* Push a range out of the CPU's caches so a non-coherent unit can read it.
 * A no-op on a unit whose page walk snoops. */
void iommu_flush_tables(uint32_t unit, const void *addr, uint64_t len);

/* Empty a unit's IOTLB.  Global, because the register interface's per-domain
 * form buys nothing at the rate IRIS changes translations. */
void iommu_invalidate_iotlb(uint32_t unit);

/* Is DMA on this machine contained?  True only when every unit the DMAR named
 * is translating.  A machine with no IOMMU answers false, which is the truth:
 * a device there reaches all of memory. */
int iommu_dma_is_contained(void);

#endif /* IRIS_IOMMU_H */
