#include <iris/gdt.h>
#include <iris/tss.h>
#include <stdint.h>

#define GDT_ENTRIES 7   /* null, kcode, kdata, ucode, udata, tss_low, tss_high */

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

/* TSS instance */
static struct tss kernel_tss;

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

    /* 3: user code — ring 3, 64-bit */
    gdt_set_entry(3,
        GDT_PRESENT | GDT_DPL3 | GDT_CODE_SEG | GDT_EXECUTABLE | GDT_RW,
        GDT_LONG_MODE | GDT_GRANULAR);

    /* 4: user data — ring 3 */
    gdt_set_entry(4,
        GDT_PRESENT | GDT_DPL3 | GDT_DATA_SEG | GDT_RW,
        GDT_GRANULAR);

    /* 5-6: TSS (16 bytes) */
    for (uint32_t i = 0; i < sizeof(struct tss); i++)
        ((uint8_t *)&kernel_tss)[i] = 0;
    kernel_tss.iopb_offset = 0xFFFF; /* allow all IO ports from ring 3 */

    gdt_set_tss((uint64_t)(uintptr_t)&kernel_tss, sizeof(struct tss) - 1);

    gdtr.size   = sizeof(gdt) - 1;
    gdtr.offset = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
    tss_flush(GDT_TSS_SEL);
}

void tss_init(void) {
    /* already done in gdt_init, exposed for external use */
}

void tss_set_rsp0(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}
