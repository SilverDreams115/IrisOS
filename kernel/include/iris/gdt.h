#ifndef IRIS_GDT_H
#define IRIS_GDT_H

#include <stdint.h>

void gdt_init(void);

/* ASM stubs */
extern void gdt_flush(uint64_t gdtr_addr);
extern void tss_flush(uint16_t tss_sel);

#endif
