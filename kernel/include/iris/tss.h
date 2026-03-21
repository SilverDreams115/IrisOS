#ifndef IRIS_TSS_H
#define IRIS_TSS_H

#include <stdint.h>

/* x86_64 TSS — 104 bytes */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;       /* kernel stack pointer for ring 0 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];     /* interrupt stack table */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);

/* GDT selectors */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x1B   /* 0x18 | 3 (RPL=3) */
#define GDT_USER_DATA    0x23   /* 0x20 | 3 (RPL=3) */
#define GDT_TSS_SEL      0x28   /* TSS selector */

#endif
