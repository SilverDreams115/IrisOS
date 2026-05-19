#ifndef IRIS_LAPIC_H
#define IRIS_LAPIC_H

#include <stdint.h>

/*
 * Local APIC (LAPIC) detection and minimal interface.
 *
 * Phase 58 scope: probe CPUID / IA32_APIC_BASE, map the MMIO window,
 * expose lapic_eoi() for future IRQ routing.  The PIC/PIT remain the
 * active timer and IRQ source; LAPIC timer activation and AP bringup
 * are deferred to a later phase.
 *
 * lapic_probe()  — detect + map.  Returns 1 if LAPIC is present and mapped,
 *                  0 if absent or if mapping fails.  Called once from
 *                  kernel_main, after paging_init() (physmap must be live).
 *
 * lapic_is_active() — 1 after a successful lapic_probe(), 0 otherwise.
 *
 * lapic_eoi()    — write the LAPIC EOI register (offset 0xB0) if active.
 *                  Safe to call unconditionally; no-op when LAPIC is absent.
 *                  Must be used instead of (or alongside) pic_eoi() when the
 *                  LAPIC timer delivers interrupts.
 *
 * lapic_software_enable() — write SVR to software-enable the LAPIC unit.
 *                  Must be called AFTER idt_init().  No-op if lapic_probe()
 *                  was not called or returned 0.
 *
 * lapic_id()     — hardware LAPIC ID of the calling CPU (offset 0x20 >> 24).
 */
int      lapic_probe(void);
int      lapic_is_active(void);
void     lapic_software_enable(void);
void     lapic_eoi(void);
uint8_t  lapic_id(void);

#endif
