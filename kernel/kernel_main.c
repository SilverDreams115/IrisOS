
#include <stdint.h>
#include <iris/kernel.h>
#include <iris/boot_info.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/gdt.h>
#include <iris/idt.h>

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
    char buf[18]; int i = 0;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        buf[i++] = hex[(value >> shift) & 0xF];
    buf[i] = '\0';
    serial_write(buf);
}
static void serial_write_dec(uint64_t value) {
    char buf[21]; int i = 20;
    buf[i] = '\0';
    if (value == 0) { buf[--i] = '0'; }
    else { while (value > 0) { buf[--i] = '0' + (int)(value % 10); value /= 10; } }
    serial_write(&buf[i]);
}

static struct iris_boot_info saved_boot_info;

void iris_kernel_main(struct iris_boot_info *boot_info) {
    serial_init();

    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 6        \n");
    serial_write("====================================\n");
    serial_write("[IRIS][KERNEL] firmware services: OFF\n");

    if (!boot_info || boot_info->magic != IRIS_BOOTINFO_MAGIC) {
        serial_write("[IRIS][KERNEL] FATAL: invalid boot protocol\n");
        for (;;) __asm__ volatile ("hlt");
    }

    /* copiar boot_info al BSS antes de activar paging */
    {
        uint64_t *src   = (uint64_t *)(uintptr_t)boot_info;
        uint64_t *dst   = (uint64_t *)(uintptr_t)&saved_boot_info;
        uint64_t  words = sizeof(struct iris_boot_info) / sizeof(uint64_t);
        for (uint64_t i = 0; i < words; i++) dst[i] = src[i];
    }

    serial_write("[IRIS][KERNEL] boot protocol OK (v");
    serial_write_dec(saved_boot_info.version);
    serial_write(")\n");

    serial_write("[IRIS][KERNEL] framebuffer: ");
    serial_write_dec(saved_boot_info.framebuffer.width);
    serial_write("x");
    serial_write_dec(saved_boot_info.framebuffer.height);
    serial_write(" @ ");
    serial_write_hex(saved_boot_info.framebuffer.base);
    serial_write("\n");

    /* PMM */
    serial_write("[IRIS][PMM] initializing...\n");
    pmm_init(&saved_boot_info);
    serial_write("[IRIS][PMM] free RAM: ");
    serial_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    serial_write(" MB (");
    serial_write_dec(pmm_free_pages());
    serial_write(" pages)\n");

    /* PAGING */
    serial_write("[IRIS][PAGING] initializing...\n");
    paging_init(saved_boot_info.framebuffer.base,
                saved_boot_info.framebuffer.size);
    serial_write("[IRIS][PAGING] virtual memory active\n");

    /* GDT */
    serial_write("[IRIS][GDT] initializing...\n");
    gdt_init();
    serial_write("[IRIS][GDT] kernel descriptors loaded\n");

    /* IDT */
    serial_write("[IRIS][IDT] initializing...\n");
    idt_init();
    serial_write("[IRIS][IDT] interrupt handlers installed\n");

    /* habilitar interrupciones */
    __asm__ volatile ("sti");
    serial_write("[IRIS][IDT] interrupts enabled\n");

    /* prueba: provocar division by zero para verificar el handler */
    serial_write("[IRIS][TEST] triggering divide by zero...\n");
    __asm__ volatile (
        "xorq %rax, %rax\n"
        "xorq %rdx, %rdx\n"
        "xorq %rcx, %rcx\n"
        "divq %rcx\n"
    );

    /* no debería llegar aquí */
    serial_write("[IRIS][TEST] FAIL: exception not caught\n");
    for (;;) __asm__ volatile ("hlt");
}
