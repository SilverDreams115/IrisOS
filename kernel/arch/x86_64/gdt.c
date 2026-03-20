#include <iris/gdt.h>
#include <stdint.h>

#define GDT_ENTRIES 5

#define GDT_PRESENT    (1 << 7)
#define GDT_DPL0       (0 << 5)
#define GDT_DPL3       (3 << 5)
#define GDT_CODE_SEG   (1 << 4)
#define GDT_DATA_SEG   (1 << 4)
#define GDT_EXECUTABLE (1 << 3)
#define GDT_RW         (1 << 1)

#define GDT_LONG_MODE  (1 << 5)
#define GDT_GRANULAR   (1 << 7)

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_descriptor {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed));

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_descriptor gdtr;

static void gdt_set_entry(int index, uint8_t access, uint8_t granularity) {
    gdt[index].limit_low   = 0xFFFF;
    gdt[index].base_low    = 0;
    gdt[index].base_mid    = 0;
    gdt[index].access      = access;
    gdt[index].granularity = granularity;
    gdt[index].base_high   = 0;
}

extern void gdt_flush(uint64_t gdtr_addr);

void gdt_init(void) {
    /* 0: null */
    gdt[0].limit_low   = 0;
    gdt[0].base_low    = 0;
    gdt[0].base_mid    = 0;
    gdt[0].access      = 0;
    gdt[0].granularity = 0;
    gdt[0].base_high   = 0;

    /* 1: kernel code — ring 0, execute/read, 64-bit */
    gdt_set_entry(1,
        GDT_PRESENT | GDT_DPL0 | GDT_CODE_SEG | GDT_EXECUTABLE | GDT_RW,
        GDT_LONG_MODE | GDT_GRANULAR);

    /* 2: kernel data — ring 0, read/write */
    gdt_set_entry(2,
        GDT_PRESENT | GDT_DPL0 | GDT_DATA_SEG | GDT_RW,
        GDT_GRANULAR);

    /* 3: user code — ring 3, execute/read, 64-bit */
    gdt_set_entry(3,
        GDT_PRESENT | GDT_DPL3 | GDT_CODE_SEG | GDT_EXECUTABLE | GDT_RW,
        GDT_LONG_MODE | GDT_GRANULAR);

    /* 4: user data — ring 3, read/write */
    gdt_set_entry(4,
        GDT_PRESENT | GDT_DPL3 | GDT_DATA_SEG | GDT_RW,
        GDT_GRANULAR);

    gdtr.size   = sizeof(gdt) - 1;
    gdtr.offset = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
}
