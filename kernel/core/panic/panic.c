#include <iris/panic.h>
#include <iris/serial.h>
#include <stdint.h>

__attribute__((noreturn)) void iris_panic(const char *msg) {
    /* Disable interrupts — we are done. */
    __asm__ volatile ("cli");

    serial_write("\n[IRIS][PANIC] ");
    if (msg)
        serial_write(msg);
    serial_write("\n[IRIS][PANIC] halting\n");

    for (;;)
        __asm__ volatile ("hlt");
}
