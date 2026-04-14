#include <iris/keyboard.h>
#include <iris/serial.h>
#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void kbd_init(void) {
    /* Flush any pending bytes before userland takes ownership of IRQ1. */
    while (inb(KBD_STATUS_PORT) & 0x01)
        (void)inb(KBD_DATA_PORT);
    serial_write("[IRIS][KBD] PS/2 controller ready for routed IRQ service\n");
}
