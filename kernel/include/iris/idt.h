
#ifndef IRIS_IDT_H
#define IRIS_IDT_H

#include <stdint.h>

/* layout of an interrupt frame — pushed by the CPU */
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

void idt_init(void);

/*
 * Load the IDT on an application processor (SMP roadmap §9.3 step 4).
 *
 * The TABLE is one for the whole machine — every core takes the same faults
 * and the same vectors mean the same things — but IDTR is a per-processor
 * register, so an AP that never executes `lidt` has whatever the firmware left
 * it, which on the way out of the trampoline is nothing.  This loads the table
 * idt_init already built; it does not build a second one.
 */
void idt_load_ap(void);

/* Ledger D-1 step 3 — ring-3 kernel entries whose user context was saved into
 * the interrupted thread's TCB.  Exposed through SYS_UNTYPED_QUERY because a
 * path whose effect is currently an identity needs a way to be observed. */
uint64_t irq_user_ctx_saves(void);

#endif
