#include <iris/gdt.h>
#include <iris/tss.h>
#include <iris/cpu_local.h>
#include <stdint.h>

/* Per-CPU data blocks; cpu_local[0] is the BSP block initialised in gdt_init. */
struct iris_cpu_local cpu_local[MAX_CPUS];

#define GDT_ENTRIES 7   /* null, kcode, kdata, udata, ucode, tss_low, tss_high */

#define GDT_PRESENT    (1 << 7)
#define GDT_DPL0       (0 << 5)
#define GDT_DPL3       (3 << 5)
#define GDT_CODE_SEG   (1 << 4)
#define GDT_DATA_SEG   (1 << 4)
#define GDT_EXECUTABLE (1 << 3)
#define GDT_RW         (1 << 1)
#define GDT_LONG_MODE  (1 << 5)
#define GDT_GRANULAR   (1 << 7)
#define GDT_TSS_TYPE   0x89   /* present, DPL0, 64-bit TSS available */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* TSS descriptor is 16 bytes in long mode */
struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_descriptor {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed));

/*
 * Per-CPU TSS and IST stacks (Fase 2.1 groundwork).
 *
 * Arrays are indexed by cpu_id.  gdt_init() initialises [0] for the BSP and
 * wires the GDT TSS descriptor to &kernel_tss[0].  tss_set_rsp0() and
 * tss_set_ist() use cpu_self()->cpu_id so they always update the active CPU's
 * TSS — correct for BSP today, correct for future APs without code changes.
 *
 * AP bring-up (future): each AP must
 *   1. Copy the BSP GDT into a per-AP buffer.
 *   2. Patch GDT slots 5-6 to point to &kernel_tss[ap_id] via gdt_set_tss().
 *   3. Execute lgdt, ltr(GDT_TSS_SEL), then enable interrupts.
 *   4. Initialise cpu_local[ap_id].{self,cpu_id,lapic_id,rq} and write
 *      IA32_GS_BASE = &cpu_local[ap_id], IA32_KERNEL_GS_BASE = 0.
 *   Stacks for cpu_id N are in [N] slots below — no further allocation needed.
 *
 * IST assignments:
 *   IST1 — #GP (13), #PF (14): critical faults that must not use a
 *           potentially-corrupted RSP from the faulting context.
 *   IST2 — NMI (2), #MC (18): can fire at any time, including during an
 *           active IST1 handler.  Separate stack prevents stack corruption.
 *   IST3 — #DF (8): double fault fires exactly when the IST1 stack may be
 *           in active use by a #GP/#PF handler; using IST1 for #DF too would
 *           reset the stack pointer and corrupt the in-flight handler frame.
 *           IST3 isolates #DF so the panic output is always coherent.
 */
static struct tss  kernel_tss[MAX_CPUS];
static uint8_t fault_ist1_stacks[MAX_CPUS][16384] __attribute__((aligned(16)));
static uint8_t nmi_ist2_stacks[MAX_CPUS][4096]    __attribute__((aligned(16)));
static uint8_t df_ist3_stacks[MAX_CPUS][4096]     __attribute__((aligned(16)));

static struct gdt_entry   gdt[GDT_ENTRIES];
static struct gdt_descriptor gdtr;

static void gdt_set_entry(int index, uint8_t access, uint8_t granularity) {
    gdt[index].limit_low   = 0xFFFF;
    gdt[index].base_low    = 0;
    gdt[index].base_mid    = 0;
    gdt[index].access      = access;
    gdt[index].granularity = granularity;
    gdt[index].base_high   = 0;
}

static void gdt_set_tss(uint64_t tss_addr, uint32_t tss_size) {
    /* TSS uses entries 5 and 6 (16 bytes total) */
    struct gdt_tss_entry *tss_entry = (struct gdt_tss_entry *)&gdt[5];
    tss_entry->limit_low   = (uint16_t)(tss_size & 0xFFFF);
    tss_entry->base_low    = (uint16_t)(tss_addr & 0xFFFF);
    tss_entry->base_mid    = (uint8_t)((tss_addr >> 16) & 0xFF);
    tss_entry->access      = GDT_TSS_TYPE;
    tss_entry->granularity = 0;
    tss_entry->base_high   = (uint8_t)((tss_addr >> 24) & 0xFF);
    tss_entry->base_upper  = (uint32_t)((tss_addr >> 32) & 0xFFFFFFFF);
    tss_entry->reserved    = 0;
}

extern void gdt_flush(uint64_t gdtr_addr);
extern void tss_flush(uint16_t tss_sel);

void gdt_init(void) {
    /* 0: null */
    gdt[0].limit_low = gdt[0].base_low = 0;
    gdt[0].base_mid  = gdt[0].access   = 0;
    gdt[0].granularity = gdt[0].base_high = 0;

    /* 1: kernel code — ring 0, 64-bit */
    gdt_set_entry(1,
        GDT_PRESENT | GDT_DPL0 | GDT_CODE_SEG | GDT_EXECUTABLE | GDT_RW,
        GDT_LONG_MODE | GDT_GRANULAR);

    /* 2: kernel data — ring 0 */
    gdt_set_entry(2,
        GDT_PRESENT | GDT_DPL0 | GDT_DATA_SEG | GDT_RW,
        GDT_GRANULAR);

    /* 3: user data — ring 3 (selector 0x1B = 0x18|3)
     * SYSRETQ: SS = (STAR[63:48]+8)|3 = (0x13+8)|3 = 0x1B → must be DATA */
    gdt_set_entry(3,
        GDT_PRESENT | GDT_DPL3 | GDT_DATA_SEG | GDT_RW,
        GDT_GRANULAR);

    /* 4: user code — ring 3, 64-bit (selector 0x23 = 0x20|3)
     * SYSRETQ: CS = (STAR[63:48]+16)|3 = (0x13+16)|3 = 0x23 → must be CODE */
    gdt_set_entry(4,
        GDT_PRESENT | GDT_DPL3 | GDT_CODE_SEG | GDT_EXECUTABLE | GDT_RW,
        GDT_LONG_MODE | GDT_GRANULAR);

    /* 5-6: TSS (16 bytes) for BSP (cpu_id=0) — static arrays are zero-initialised */
    kernel_tss[0].iopb_offset = sizeof(struct tss); /* no IO ports from ring-3 */
    kernel_tss[0].ist[0] = (uint64_t)(uintptr_t)(fault_ist1_stacks[0] + sizeof(fault_ist1_stacks[0])); /* IST1: #GP/#PF */
    kernel_tss[0].ist[1] = (uint64_t)(uintptr_t)(nmi_ist2_stacks[0]   + sizeof(nmi_ist2_stacks[0]));   /* IST2: NMI/#MC */
    kernel_tss[0].ist[2] = (uint64_t)(uintptr_t)(df_ist3_stacks[0]    + sizeof(df_ist3_stacks[0]));    /* IST3: #DF (isolated from IST1) */

    gdt_set_tss((uint64_t)(uintptr_t)&kernel_tss[0], sizeof(struct tss) - 1);

    gdtr.size   = sizeof(gdt) - 1;
    gdtr.offset = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
    tss_flush(GDT_TSS_SEL);

    /* SWAPGS ABI (Fase 2):
     *
     * Ring-0 resting state: GS_BASE = &cpu_local[0], KGS_BASE = 0.
     * Ring-3 user state:    GS_BASE = 0 (null),      KGS_BASE = &cpu_local[0].
     *
     * Syscall/ISR entry from ring-3: SWAPGS → GS_BASE=&cpu_local, KGS_BASE=0 (kernel).
     * Syscall/ISR exit to ring-3:   SWAPGS → GS_BASE=0,           KGS_BASE=&cpu_local.
     * ISR from ring-0: GS_BASE=&cpu_local already — NO SWAPGS needed.
     *
     * Setting GS_BASE=&cpu_local here (not KGS_BASE) means cpu_self() is safe from
     * any ring-0 context including the idle loop and ring-0 ISRs, without any SWAPGS.
     *
     * cpu_local[0].self must be set before the WRMSR so %gs:0 is valid immediately. */
    cpu_local[0].self    = &cpu_local[0];
    cpu_local[0].cpu_id  = 0;
    {
        uint64_t addr = (uint64_t)(uintptr_t)&cpu_local[0];
        uint32_t lo   = (uint32_t)(addr & 0xFFFFFFFFULL);
        uint32_t hi   = (uint32_t)(addr >> 32);
        /* IA32_GS_BASE = &cpu_local[0]  (ring-0 can use cpu_self() immediately) */
        __asm__ volatile ("wrmsr" :: "c"(0xC0000101u), "a"(lo), "d"(hi));
        /* IA32_KERNEL_GS_BASE = 0  (SWAPGS at exit will push this to user GS_BASE) */
        __asm__ volatile ("wrmsr" :: "c"(0xC0000102u), "a"(0u), "d"(0u));
    }
}

void tss_init(void) {
    /* already done in gdt_init, exposed for external use */
}

void tss_set_ist(int index, uint64_t rsp) {
    if (index < 1 || index > 7) return;
    /* cpu_self() safe: only called post-gdt_init() from ring-0 with GS_BASE valid */
    kernel_tss[cpu_self()->cpu_id].ist[index - 1] = rsp;
}

void tss_set_rsp0(uint64_t rsp0) {
    /* cpu_self() safe: always called from task_yield() which runs post-gdt_init()
     * ring-0 with GS_BASE = &cpu_local[cpu_id]. Updates THIS CPU's TSS.rsp0. */
    kernel_tss[cpu_self()->cpu_id].rsp0 = rsp0;
}
