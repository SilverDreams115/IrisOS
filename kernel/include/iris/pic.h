#ifndef IRIS_PIC_H
#define IRIS_PIC_H

#include <stdint.h>

void pic_init(void);
void pic_eoi(uint8_t irq);
void pit_init(uint32_t hz);
void pic_set_irq_mask(uint8_t irq, int masked);

#endif
