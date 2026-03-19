#include <stdint.h>
#include <iris/kernel.h>
#include <iris/boot_info.h>

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
    while (!serial_ready()) {
    }
    outb(COM1_PORT, (uint8_t)c);
}

static void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

void iris_kernel_main(struct iris_boot_info *boot_info) {
    serial_init();

    serial_write("[IRIS][KERNEL] ELF kernel entered\n");

    if (boot_info && boot_info->magic == IRIS_BOOTINFO_MAGIC) {
        serial_write("[IRIS][KERNEL] boot protocol received\n");
    } else {
        serial_write("[IRIS][KERNEL] invalid boot protocol\n");
    }

    if (boot_info && boot_info->framebuffer.base != 0) {
        serial_write("[IRIS][KERNEL] framebuffer info available\n");
    } else {
        serial_write("[IRIS][KERNEL] framebuffer info unavailable\n");
    }

    serial_write("[IRIS][KERNEL] halting CPU\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
