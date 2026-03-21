#include <iris/serial.h>
#include <stdint.h>

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

void serial_write_dec(uint64_t value) {
    char buf[21]; int i = 20;
    buf[i] = '\0';
    if (value == 0) { buf[--i] = '0'; }
    else { while (value > 0) { buf[--i] = '0' + (int)(value % 10); value /= 10; } }
    serial_write(&buf[i]);
}

void serial_write_hex(uint64_t value) {
    const char *hex = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(value >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    serial_write(buf);
}

void serial_write_hex16(uint16_t value) {
    const char *hex = "0123456789ABCDEF";
    char buf[7]; /* 0x + 4 digits + null */
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(value >> 12) & 0xF];
    buf[3] = hex[(value >>  8) & 0xF];
    buf[4] = hex[(value >>  4) & 0xF];
    buf[5] = hex[(value >>  0) & 0xF];
    buf[6] = '\0';
    serial_write(buf);
}

void serial_write_hex8(uint8_t value) {
    const char *hex = "0123456789ABCDEF";
    char buf[5]; /* 0x + 2 digits + null */
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = hex[(value >> 4) & 0xF];
    buf[3] = hex[(value >> 0) & 0xF];
    buf[4] = '\0';
    serial_write(buf);
}
