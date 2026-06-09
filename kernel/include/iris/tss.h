#ifndef IRIS_TSS_H
#define IRIS_TSS_H

#include <stdint.h>

/* x86_64 TSS — 104 bytes per Intel SDM Vol.3A §7.2.1 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;       /* kernel stack pointer for ring 0 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];     /* IST1-IST7: interrupt stack table entries */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

/*
 * Per-CPU TSS model (Fase 2.1 groundwork):
 *
 *   gdt.c maintains kernel_tss[MAX_CPUS] internally.  The BSP (cpu_id=0) is
 *   wired during gdt_init().  tss_set_rsp0() and tss_set_ist() index by
 *   cpu_self()->cpu_id so they always update THIS CPU's TSS — correct for the
 *   BSP today, correct for APs once they initialise cpu_local[ap_id].cpu_id.
 *
 *   IST assignment (see idt.c for IDT side):
 *     IST1 — #GP, #PF          (16 KB per CPU; critical ring-0/ring-3 faults)
 *     IST2 — NMI, #MC          (4 KB per CPU; must not share with IST1)
 *     IST3 — #DF               (4 KB per CPU; isolated so #DF can't corrupt
 *                                an in-flight #GP/#PF IST1 frame)
 *
 *   AP bring-up (future, NOT implemented here):
 *     Each AP copies the BSP GDT, patches slots 5-6 to point to
 *     kernel_tss[ap_id], executes lgdt + ltr(GDT_TSS_SEL), then initialises
 *     cpu_local[ap_id] and sets IA32_GS_BASE = &cpu_local[ap_id].
 *     Stack slots fault_ist1_stacks[ap_id], nmi_ist2_stacks[ap_id], and
 *     df_ist3_stacks[ap_id] are pre-allocated in gdt.c BSS — no PMM needed.
 */
void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);
void tss_set_ist(int index, uint64_t rsp);

/* GDT selectors */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x1B   /* 0x18 | 3 (RPL=3) — sysretq SS */
#define GDT_USER_CODE    0x23   /* 0x20 | 3 (RPL=3) — sysretq CS */
#define GDT_TSS_SEL      0x28   /* TSS selector */

#endif
