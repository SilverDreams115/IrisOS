#include <stdint.h>
#include <iris/kernel.h>
#include <iris/boot_info.h>

/* ── serial ─────────────────────────────────────────────────────────── */

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x01);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
    outb(COM1_PORT + 4, 0x0B);
}

static int serial_ready(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

static void serial_putc(char c) {
    while (!serial_ready()) {}
    outb(COM1_PORT, (uint8_t)c);
}

static void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* ── utilidades numéricas (sin libc) ────────────────────────────────── */

static void serial_write_hex(uint64_t value) {
    const char hex[] = "0123456789ABCDEF";
    char buf[18];
    int  i = 0;

    buf[i++] = '0';
    buf[i++] = 'x';

    for (int shift = 60; shift >= 0; shift -= 4)
        buf[i++] = hex[(value >> shift) & 0xF];

    buf[i] = '\0';
    serial_write(buf);
}

static void serial_write_dec(uint64_t value) {
    char buf[21];
    int  i = 20;

    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0) {
            buf[--i] = '0' + (int)(value % 10);
            value /= 10;
        }
    }
    serial_write(&buf[i]);
}

/* ── nombre del tipo de región ──────────────────────────────────────── */

static const char *mmap_type_name(uint32_t type) {
    switch (type) {
        case IRIS_MEM_USABLE:           return "USABLE";
        case IRIS_MEM_RESERVED:         return "RESERVED";
        case IRIS_MEM_ACPI_RECLAIMABLE: return "ACPI_RECLAIM";
        case IRIS_MEM_ACPI_NVS:         return "ACPI_NVS";
        case IRIS_MEM_BAD:              return "BAD";
        case IRIS_MEM_BOOTLOADER:       return "BOOTLOADER";
        case IRIS_MEM_KERNEL:           return "KERNEL";
        case IRIS_MEM_FRAMEBUFFER:      return "FRAMEBUFFER";
        default:                        return "UNKNOWN";
    }
}

/* ── entrada del kernel ─────────────────────────────────────────────── */

void iris_kernel_main(struct iris_boot_info *boot_info) {
    serial_init();

    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 3        \n");
    serial_write("====================================\n");
    serial_write("[IRIS][KERNEL] ELF kernel entered\n");
    serial_write("[IRIS][KERNEL] firmware services: OFF\n");

    if (!boot_info || boot_info->magic != IRIS_BOOTINFO_MAGIC) {
        serial_write("[IRIS][KERNEL] FATAL: invalid boot protocol\n");
        for (;;) __asm__ volatile ("hlt");
    }

    serial_write("[IRIS][KERNEL] boot protocol OK (v");
    serial_write_dec(boot_info->version);
    serial_write(")\n");

    /* framebuffer */
    if (boot_info->framebuffer.base != 0) {
        serial_write("[IRIS][KERNEL] framebuffer: ");
        serial_write_dec(boot_info->framebuffer.width);
        serial_write("x");
        serial_write_dec(boot_info->framebuffer.height);
        serial_write(" @ ");
        serial_write_hex(boot_info->framebuffer.base);
        serial_write("\n");
    } else {
        serial_write("[IRIS][KERNEL] framebuffer: unavailable\n");
    }

    /* memory map */
    serial_write("[IRIS][KERNEL] memory map (");
    serial_write_dec(boot_info->mmap_entry_count);
    serial_write(" entries):\n");

    uint64_t total_usable = 0;

    for (uint64_t i = 0; i < boot_info->mmap_entry_count; i++) {
        struct iris_mmap_entry *e = &boot_info->mmap[i];

        serial_write("  [");
        serial_write_dec(i);
        serial_write("] ");
        serial_write_hex(e->base);
        serial_write(" + ");
        serial_write_hex(e->length);
        serial_write("  ");
        serial_write(mmap_type_name(e->type));
        serial_write("\n");

        if (e->type == IRIS_MEM_USABLE)
            total_usable += e->length;
    }

    serial_write("[IRIS][KERNEL] total usable RAM: ");
    serial_write_dec(total_usable / (1024 * 1024));
    serial_write(" MB\n");

    serial_write("[IRIS][KERNEL] Stage 3 complete\n");
    serial_write("[IRIS][KERNEL] halting CPU — next: paging + PMM\n");

    for (;;) __asm__ volatile ("hlt");
}
