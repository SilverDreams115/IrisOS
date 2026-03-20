
#ifndef IRIS_IDT_H
#define IRIS_IDT_H

#include <stdint.h>

/* estructura de un frame de interrupción — pushed por el CPU */
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

void idt_init(void);

#endif
