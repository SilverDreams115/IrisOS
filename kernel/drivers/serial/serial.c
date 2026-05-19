#include <iris/serial.h>
#include <stdint.h>

/*
 * Early-boot serial driver — ring-0, boot-phase and fatal-path ONLY.
 *
 * serial_init()  : called once by kernel_main before ring-3 exists.
 * serial_write() : used by fatal/panic paths (kstack, kpage, phase3_selftest).
 *
 * Normal output goes through klog → SYS_KLOG_DRAIN → ring-3 console service.
 * No serial I/O should happen in ring-0 after the first task switch.
 */

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x01);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
    outb(COM1_PORT + 4, 0x0B);
}

static int serial_ready(void) { return inb(COM1_PORT + 5) & 0x20; }

void serial_putc(char c) {
    while (!serial_ready()) {}
    outb(COM1_PORT, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}
