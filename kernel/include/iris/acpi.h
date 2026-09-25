/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_ACPI_H
#define IRIS_ACPI_H

/*
 * acpi.h — the two things the kernel reads out of ACPI.
 *
 * MADT (SMP roadmap §9.3 step 3): how many CPUs this machine has, and what
 * their LAPIC ids are.  DMAR (§10.2 step 1): where the DMA remapping units
 * are — interpreted in `iris/iommu.h`, which this file only hands the bytes
 * to.
 *
 * WHY THE KERNEL PARSES ANY OF IT.  An application processor is started by
 * sending it an INIT and then a STARTUP IPI, and an IPI is addressed by LAPIC
 * id.  There is no way to learn those ids except from firmware, and no way to
 * learn them later — the RSDP comes out of a boot-services table that stops
 * existing at ExitBootServices.  So the address is forwarded in BootInfo and
 * the kernel walks from it to the MADT.
 *
 * WHY IT PARSES LITTLE ELSE, and this is the part worth defending.  ACPI
 * describes power states, thermal zones, PCI interrupt routing, embedded
 * controllers — and every one of those is a ring-3 concern in IRIS, reached
 * through a capability.  A kernel with a general table parser is a kernel that
 * grows a driver, and the charter's P1/P2 are the rules that would be bent one
 * table at a time.
 *
 * The second table earns its place by the same test the first did: a DMA
 * remapping unit is addressed by a register base that only firmware knows, and
 * without it a driver's containment is a claim the kernel cannot keep.  What
 * this file does for it is find the bytes and check the checksum — the layout
 * is read elsewhere, so this does not become the place tables accumulate.
 *
 * Nothing here runs after boot: the result is a static array read once.
 */

#include <stdint.h>

/* What the MADT said.  `count` is 0 when there is no RSDP, no MADT, or the
 * tables did not checksum — in which case the machine is treated as having
 * exactly the CPU that is already running, which is the truth as far as
 * anybody can prove it. */
struct iris_cpu_desc {
    uint8_t lapic_id;
    uint8_t enabled;        /* the MADT's "this processor is usable" flag */
};

/* Parse from the RSDP BootInfo forwarded.  Safe to call with 0.  Returns the
 * number of processors found, which is also `acpi_cpu_count()` afterwards. */
uint32_t acpi_parse_madt(uint64_t rsdp_phys);

uint32_t acpi_cpu_count(void);
/* Processors the firmware described that this build could not hold.
 * Non-zero means the count above is a ceiling, not a census. */
uint32_t acpi_cpu_dropped_count(void);
const struct iris_cpu_desc *acpi_cpu(uint32_t index);

/*
 * Find a table by its four-character signature, checksum-validated, and hand
 * back a pointer into the physmap window plus its length.  NULL when there is
 * no RSDP, no such table, or it did not checksum.
 *
 * Bytes and a length, not a struct: this file knows how to FIND a table, and
 * the caller knows what its own table means.  The alternative is that every
 * future table's layout accretes here, which is exactly what the note at the
 * top of this header says does not happen.
 */
const void *acpi_find_table(uint64_t rsdp_phys, const char *sig, uint32_t *out_len);

/* The LAPIC's physical base as the MADT reports it, or 0.  The LAPIC driver
 * has its own default (0xFEE00000) and firmware is allowed to move it. */
uint64_t acpi_lapic_base(void);

#endif /* IRIS_ACPI_H */
