/* SPDX-License-Identifier: Apache-2.0 */
#include <iris/panic.h>
#include <iris/serial.h>
#include <stdint.h>
#include <iris/fbcon.h>

__attribute__((noreturn)) void iris_panic(const char *msg) {
    /* Disable interrupts — we are done. */
    __asm__ volatile ("cli");

    /* Take the screen back: a kernel that is stopping has no userspace left
     * to be polite to, and dying behind somebody else's pixels is dying
     * silently on a machine with no serial port. */
    fbcon_reclaim();
    fbcon_write("\n[IRIS][PANIC] ");
    if (msg) fbcon_write(msg);
    fbcon_write("\n[IRIS][PANIC] halting\n");
    serial_write("\n[IRIS][PANIC] ");
    if (msg)
        serial_write(msg);
    serial_write("\n[IRIS][PANIC] halting\n");

    for (;;)
        __asm__ volatile ("hlt");
}
