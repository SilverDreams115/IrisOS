/* SPDX-License-Identifier: Apache-2.0 */
/*
 * iommu_excluded.c — the IOMMU, outside IRIS-V's trusted computing base.
 *
 * Linked in place of `iommu.c` when `IRIS_VERIFIED_CONFIG` is set.  Every
 * function answers exactly what it answers on a machine that has no VT-d unit
 * — a path the kernel already supports and already gates, because the tree
 * requires that a kernel which finds no unit SAYS so rather than pretending.
 *
 * That is the whole reason this file is stubs rather than `#ifdef`s through
 * `kernel_main.c`.  A proof about conditionally-compiled code is a proof about
 * whichever branch the preprocessor chose, and reading the source no longer
 * tells you which one that was.  Here the caller is unchanged, the control
 * flow is unchanged, and the boundary is visible as a file.
 *
 * See `iris/verified_config.h` for what IRIS-V is and what it gives up.
 */
#include <iris/iommu.h>
#include <iris/klog.h>
#include <stdint.h>

/*
 * Nothing was parsed, because nothing was looked at — and it SAYS SO.
 *
 * This is the one stub that is not silent, and the reason is the rule the
 * headless lane enforces: "a kernel that silently found nothing and a kernel
 * that silently skipped looking read the same from outside".  IRIS-V skipped
 * looking, so every DMA-capable device on it can reach all of memory, and the
 * verified configuration must not be QUIETER about its own weaknesses than
 * the unverified one.  It prints the same line `iommu.c` prints on a machine
 * with no DMAR, because the consequence for the machine is the same.
 */
uint32_t iommu_parse_dmar(uint64_t rsdp_phys) {
    (void)rsdp_phys;
    klog_write("[IRIS][IOMMU] no DMAR; DMA is unrestricted on this machine\n");
    klog_write("[IRIS][IOMMU] (IRIS-V: the unit is outside the TCB by "
               "configuration, not absent from the machine)\n");
    return 0u;
}
uint32_t iommu_unit_count(void)               { return 0u; }
uint32_t iommu_host_address_width(void)       { return 0u; }
uint32_t iommu_probe_units(void)              { return 0u; }
uint32_t iommu_usable_count(void)             { return 0u; }
uint32_t iommu_enable_blocking(void)          { return 0u; }
uint32_t iommu_enabled_count(void)            { return 0u; }

const struct iris_iommu_unit *iommu_unit(uint32_t index) {
    (void)index; return 0;
}

uint32_t iommu_fault_status(uint32_t index) { (void)index; return 0u; }

int iommu_fault_record(uint32_t index, struct iris_iommu_fault *out, int clear) {
    (void)index; (void)out; (void)clear; return 0;
}

/* No unit serves any source, which is what "unit for this device" means on a
 * machine with no units.  `IOMMU_NO_UNIT` is the tree's own spelling. */
uint32_t iommu_unit_for_source(uint16_t source_id) {
    (void)source_id; return IOMMU_NO_UNIT;
}

uint32_t iommu_levels(uint32_t unit)        { (void)unit; return 0u; }
uint16_t iommu_domain_claim(uint32_t unit)  { (void)unit; return 0u; }

void iommu_domain_release(uint32_t unit, uint16_t domain) {
    (void)unit; (void)domain;
}

int iommu_context_set(uint32_t unit, uint16_t source_id, uint16_t domain,
                      uint64_t root_pt_phys, uint32_t levels) {
    (void)unit; (void)source_id; (void)domain; (void)root_pt_phys; (void)levels;
    return -1;                 /* there is no context table to set */
}

void iommu_flush_tables(uint32_t unit, const void *addr, uint64_t len) {
    (void)unit; (void)addr; (void)len;
}

void iommu_invalidate_iotlb(uint32_t unit) { (void)unit; }

/*
 * And the one answer that must NOT be optimistic.
 *
 * "Is device DMA contained?" is NO on a machine with no unit, and IRIS-V is
 * such a machine by construction.  A stub that answered yes would turn a
 * scoping decision into a false security claim, which is the exact failure
 * this file exists to avoid.
 */
int iommu_dma_is_contained(void) { return 0; }
