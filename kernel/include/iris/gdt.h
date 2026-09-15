#ifndef IRIS_GDT_H
#define IRIS_GDT_H

#include <stdint.h>

void gdt_init(void);
/* Give an application processor its own GDT, TSS and per-CPU GS base — steps
 * 1, 2 and 4 of the bring-up recipe in gdt.c.  Interrupts stay OFF: an AP that
 * ends this has no scheduler state and nothing to do with a tick. */
void gdt_init_ap(uint32_t cpu_id);

/* ASM stubs */
extern void gdt_flush(uint64_t gdtr_addr);
extern void tss_flush(uint16_t tss_sel);

#endif
