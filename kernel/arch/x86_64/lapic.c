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

#define LAPIC_REG_ICR_LO  0x300u
#define LAPIC_REG_ICR_HI  0x310u

/* ICR_LO fields. */
#define ICR_DELIVERY_FIXED   (0u << 8)
#define ICR_DELIVERY_INIT    (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_TRIGGER_LEVEL    (1u << 15)
#define ICR_SEND_PENDING     (1u << 12)   /* read-only: delivery in progress */

/*
 * Wait for the previous IPI to be accepted.
 *
 * The ICR holds ONE message.  Writing a second before the first has been
 * delivered loses it, and the startup sequence below writes four in a row —
 * so every write is preceded by this.  It is a spin with no timeout on
 * purpose: the alternative is to carry on having silently failed to start a
 * processor, and a machine that hangs here is telling the truth about its
 * interrupt controller.
 */
static void lapic_ipi_wait(void) {
    if (!lapic_active) return;
    while (lapic_base[LAPIC_REG_ICR_LO / 4] & ICR_SEND_PENDING)
        __asm__ volatile ("pause");
}

static void lapic_icr_write(uint8_t dest, uint32_t low) {
    lapic_ipi_wait();
    lapic_base[LAPIC_REG_ICR_HI / 4] = (uint32_t)dest << 24;
    lapic_base[LAPIC_REG_ICR_LO / 4] = low;
}

void lapic_send_ipi(uint8_t dest_lapic_id, uint8_t vector) {
    if (!lapic_active) return;
    lapic_icr_write(dest_lapic_id,
                    (uint32_t)vector | ICR_DELIVERY_FIXED | ICR_LEVEL_ASSERT);
}

/*
 * INIT: put the target processor into its wait-for-SIPI state.
 *
 * Sent as assert then de-assert, which is what the older multiprocessor
 * specification required and what every firmware still expects to see.  Modern
 * parts ignore the de-assert; sending it costs one ICR write and removes a
 * class of "works on my machine" that is very hard to debug from the other
 * side, because the symptom is a processor that never runs anything.
 */
void lapic_send_init(uint8_t dest_lapic_id) {
    if (!lapic_active) return;
    lapic_icr_write(dest_lapic_id,
                    ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    lapic_icr_write(dest_lapic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL);
    lapic_ipi_wait();
}

/*
 * STARTUP: begin executing at `vector << 12`, in 16-bit real mode.
 *
 * The vector is a PAGE NUMBER below 1 MiB, not an address — which is why the
 * trampoline has to live in low memory whatever the rest of the kernel does.
 */
void lapic_send_startup(uint8_t dest_lapic_id, uint8_t vector) {
    if (!lapic_active) return;
    lapic_icr_write(dest_lapic_id,
                    (uint32_t)vector | ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT);
    lapic_ipi_wait();
}
