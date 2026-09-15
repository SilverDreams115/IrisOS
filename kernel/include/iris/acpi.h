#ifndef IRIS_ACPI_H
#define IRIS_ACPI_H

/*
 * acpi.h — the ONE thing the kernel reads out of ACPI (SMP roadmap §9.3
 * step 3): how many CPUs this machine has, and what their LAPIC ids are.
 *
 * WHY THE KERNEL PARSES ANY OF IT.  An application processor is started by
 * sending it an INIT and then a STARTUP IPI, and an IPI is addressed by LAPIC
 * id.  There is no way to learn those ids except from firmware, and no way to
 * learn them later — the RSDP comes out of a boot-services table that stops
 * existing at ExitBootServices.  So the address is forwarded in BootInfo and
 * the kernel walks from it to the MADT.
 *
 * WHY IT PARSES NOTHING ELSE, and this is the part worth defending.  ACPI
 * describes power states, thermal zones, PCI interrupt routing, embedded
 * controllers — and every one of those is a ring-3 concern in IRIS, reached
 * through a capability.  A kernel with a general table parser is a kernel that
 * grows a driver, and the charter's P1/P2 are the rules that would be bent one
 * table at a time.  This walks RSDP to XSDT to MADT, reads processor entries,
 * and stops.
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
const struct iris_cpu_desc *acpi_cpu(uint32_t index);

/* The LAPIC's physical base as the MADT reports it, or 0.  The LAPIC driver
 * has its own default (0xFEE00000) and firmware is allowed to move it. */
uint64_t acpi_lapic_base(void);

#endif /* IRIS_ACPI_H */
