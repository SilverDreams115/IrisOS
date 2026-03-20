#include <stdint.h>
#include <iris/kernel.h>
#include <iris/boot_info.h>
#include "../mm/pmm/pmm.h"

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
static int serial_ready(void) { return inb(COM1_PORT + 5) & 0x20; }
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
static void serial_write_hex(uint64_t value) {
    const char hex[] = "0123456789ABCDEF";
    char buf[18];
    int i = 0;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        buf[i++] = hex[(value >> shift) & 0xF];
    buf[i] = '\0';
    serial_write(buf);
}
static void serial_write_dec(uint64_t value) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (value == 0) { buf[--i] = '0'; }
    else { while (value > 0) { buf[--i] = '0' + (int)(value % 10); value /= 10; } }
    serial_write(&buf[i]);
}

void iris_kernel_main(struct iris_boot_info *boot_info) {
    serial_init();

    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 4        \n");
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

    serial_write("[IRIS][KERNEL] framebuffer: ");
    serial_write_dec(boot_info->framebuffer.width);
    serial_write("x");
    serial_write_dec(boot_info->framebuffer.height);
    serial_write(" @ ");
    serial_write_hex(boot_info->framebuffer.base);
    serial_write("\n");

    /* ── PMM ──────────────────────────────────────────────────────── */
    serial_write("[IRIS][PMM] initializing...\n");
    pmm_init(boot_info);

    serial_write("[IRIS][PMM] total pages : ");
    serial_write_dec(pmm_total_pages());
    serial_write("\n");
    serial_write("[IRIS][PMM] used  pages : ");
    serial_write_dec(pmm_used_pages());
    serial_write("\n");
    serial_write("[IRIS][PMM] free  pages : ");
    serial_write_dec(pmm_free_pages());
    serial_write("\n");
    serial_write("[IRIS][PMM] free  RAM   : ");
    serial_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    serial_write(" MB\n");

    /* prueba: alloc / free */
    serial_write("[IRIS][PMM] test: alloc 3 pages\n");
    {
        uint64_t p1 = pmm_alloc_page();
        uint64_t p2 = pmm_alloc_page();
        uint64_t p3 = pmm_alloc_page();

        serial_write("  page 1: "); serial_write_hex(p1); serial_write("\n");
        serial_write("  page 2: "); serial_write_hex(p2); serial_write("\n");
        serial_write("  page 3: "); serial_write_hex(p3); serial_write("\n");

        if (p2 != p1 + 4096 && p2 == p1) {
            serial_write("[IRIS][PMM] WARN: pages not sequential (fragmented map)\n");
        }

        serial_write("[IRIS][PMM] test: free page 2\n");
        pmm_free_page(p2);

        uint64_t p4 = pmm_alloc_page();
        serial_write("  realloc: "); serial_write_hex(p4); serial_write("\n");

        if (p4 == p2)
            serial_write("[IRIS][PMM] test PASSED: realloc returned freed page\n");
        else
            serial_write("[IRIS][PMM] test OK: realloc returned different page\n");

        pmm_free_page(p1);
        pmm_free_page(p3);
        pmm_free_page(p4);
    }

    serial_write("[IRIS][PMM] free after cleanup: ");
    serial_write_dec(pmm_free_pages());
    serial_write(" pages\n");

    serial_write("[IRIS][KERNEL] Stage 4 complete\n");
    serial_write("[IRIS][KERNEL] halting CPU — next: paging + VMM\n");

    for (;;) __asm__ volatile ("hlt");
}
