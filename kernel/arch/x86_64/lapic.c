#include <iris/lapic.h>
#include <iris/paging.h>
#include <stdint.h>

/* IA32_APIC_BASE MSR — physical base and enable bit */
#define IA32_APIC_BASE_MSR   0x1Bu
#define APIC_BASE_ENABLE     (1ULL << 11)

/* LAPIC register offsets (byte offsets; access as 32-bit DWORD) */
#define LAPIC_REG_ID         0x020u
#define LAPIC_REG_EOI        0x0B0u
#define LAPIC_REG_SVR        0x0F0u

static volatile uint32_t *lapic_base = 0;
static int                lapic_active = 0;

static inline uint64_t rdmsr_lapic(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID.01H:EDX[9] — APIC on-chip flag */
static inline int cpuid_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1u), "c"(0u));
    return (edx >> 9) & 1;
}

/* LAPIC SVR bits */
#define LAPIC_SVR_ENABLE   (1u << 8)    /* software enable the LAPIC */
#define LAPIC_SVR_SPURIOUS 0xFFu        /* spurious vector: reserved upper range */

int lapic_probe(void) {
    if (!cpuid_has_apic()) return 0;

    uint64_t apic_msr  = rdmsr_lapic(IA32_APIC_BASE_MSR);
    uint64_t lapic_phys = apic_msr & 0xFFFFF000ULL;
    if (lapic_phys == 0) return 0;

    /* The physmap window (PHYS_TO_VIRT) already covers the LAPIC MMIO page
     * that the BIOS/firmware placed in the first 4 GB of physical space.
     * No additional paging_map() call needed; paging_init() mapped 4 GB. */
    lapic_base   = (volatile uint32_t *)(uintptr_t)PHYS_TO_VIRT(lapic_phys);
    lapic_active = 1;

    return 1;
}

/* lapic_software_enable — write the SVR to software-enable the LAPIC unit.
 * Must be called AFTER idt_init() so the IDT is live before the LAPIC can
 * deliver NMI-class interrupts (NMIs bypass IF=0 and would triple-fault against
 * an uninitialized IDT).  No-op if lapic_probe() was not called or failed. */
void lapic_software_enable(void) {
    if (!lapic_active) return;
    uint32_t svr = lapic_base[LAPIC_REG_SVR / 4];
    svr &= ~0xFFu;             /* clear existing spurious vector field */
    svr |= LAPIC_SVR_SPURIOUS; /* set spurious vector to 0xFF          */
    svr |= LAPIC_SVR_ENABLE;   /* set software enable bit               */
    lapic_base[LAPIC_REG_SVR / 4] = svr;
}

int lapic_is_active(void) {
    return lapic_active;
}

void lapic_eoi(void) {
    if (lapic_active)
        lapic_base[LAPIC_REG_EOI / 4] = 0;
}

uint8_t lapic_id(void) {
    if (!lapic_active) return 0;
    return (uint8_t)(lapic_base[LAPIC_REG_ID / 4] >> 24);
}
