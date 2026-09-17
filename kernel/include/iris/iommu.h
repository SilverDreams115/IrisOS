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

#endif /* IRIS_IOMMU_H */
