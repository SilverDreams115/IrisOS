
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

/* Ledger D-1 step 3 — ring-3 kernel entries whose user context was saved into
 * the interrupted thread's TCB.  Exposed through SYS_UNTYPED_QUERY because a
 * path whose effect is currently an identity needs a way to be observed. */
uint64_t irq_user_ctx_saves(void);

#endif
