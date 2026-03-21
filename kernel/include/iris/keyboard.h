#ifndef IRIS_KEYBOARD_H
#define IRIS_KEYBOARD_H

#include <stdint.h>

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_BUFFER_SIZE  64

void    kbd_init(void);
void    kbd_irq_handler(void);
int32_t kbd_getchar(void);
int32_t kbd_trygetchar(void);

#endif
