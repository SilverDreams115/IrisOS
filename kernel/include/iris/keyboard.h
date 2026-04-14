#ifndef IRIS_KEYBOARD_H
#define IRIS_KEYBOARD_H

#include <stdint.h>

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64

/* Minimal kernel-side PS/2 controller bring-up.
 * Runtime keyboard ownership lives in the userland `kbd` service. */
void kbd_init(void);

#endif
