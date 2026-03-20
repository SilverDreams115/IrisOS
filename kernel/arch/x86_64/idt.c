#include <iris/idt.h>
#include <iris/pic.h>
#include <iris/scheduler.h>
#include <stdint.h>

#define IDT_ENTRIES        256
#define IDT_TYPE_INTERRUPT 0x8E
#define GDT_KERNEL_CODE    0x08

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_descriptor {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed));

struct full_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_descriptor idtr;

#define DECLARE_ISR(n) extern void isr##n(void);
DECLARE_ISR(0)  DECLARE_ISR(1)  DECLARE_ISR(2)  DECLARE_ISR(3)
DECLARE_ISR(4)  DECLARE_ISR(5)  DECLARE_ISR(6)  DECLARE_ISR(7)
DECLARE_ISR(8)  DECLARE_ISR(9)  DECLARE_ISR(10) DECLARE_ISR(11)
DECLARE_ISR(12) DECLARE_ISR(13) DECLARE_ISR(14) DECLARE_ISR(15)
DECLARE_ISR(16) DECLARE_ISR(17) DECLARE_ISR(18) DECLARE_ISR(19)
DECLARE_ISR(20) DECLARE_ISR(21) DECLARE_ISR(22) DECLARE_ISR(23)
DECLARE_ISR(24) DECLARE_ISR(25) DECLARE_ISR(26) DECLARE_ISR(27)
DECLARE_ISR(28) DECLARE_ISR(29) DECLARE_ISR(30) DECLARE_ISR(31)
extern void isr32(void); /* IRQ0 — timer */

extern void idt_flush(uint64_t idtr_addr);

static void idt_set_entry(int vector, void (*handler)(void)) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = IDT_TYPE_INTERRUPT;
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].zero        = 0;
}

static const char *exception_names[32] = {
    "Divide By Zero",       "Debug",
    "NMI",                  "Breakpoint",
    "Overflow",             "Bound Range Exceeded",
    "Invalid Opcode",       "Device Not Available",
    "Double Fault",         "Coprocessor Segment Overrun",
    "Invalid TSS",          "Segment Not Present",
    "Stack Segment Fault",  "General Protection Fault",
    "Page Fault",           "Reserved",
    "x87 Floating Point",   "Alignment Check",
    "Machine Check",        "SIMD Floating Point",
    "Virtualization",       "Reserved",
    "Reserved",             "Reserved",
    "Reserved",             "Reserved",
    "Reserved",             "Reserved",
    "Reserved",             "Reserved",
    "Security Exception",   "Reserved",
};

static inline void outb_direct(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb_direct(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}
static void panic_putc(char c) {
    while (!(inb_direct(0x3F8 + 5) & 0x20)) {}
    outb_direct(0x3F8, (uint8_t)c);
}
static void panic_write(const char *s) {
    while (*s) { if (*s == '\n') panic_putc('\r'); panic_putc(*s++); }
}
static void panic_hex(uint64_t v) {
    const char h[] = "0123456789ABCDEF";
    char buf[18]; int i = 0;
    buf[i++] = '0'; buf[i++] = 'x';
    for (int s = 60; s >= 0; s -= 4) buf[i++] = h[(v >> s) & 0xF];
    buf[i] = 0;
    panic_write(buf);
}

void isr_handler(struct full_frame *frame) {
    if (frame->vector == 32) {
        /* IRQ0 — timer */
        pic_eoi(0);
        scheduler_tick();
        return;
    }

    if (frame->vector < 32) {
        /* excepción CPU */
        panic_write("\n====================================\n");
        panic_write("[IRIS][EXCEPTION] ");
        panic_write(exception_names[frame->vector]);
        panic_write("\n");
        panic_write("  vector     : "); panic_hex(frame->vector);     panic_write("\n");
        panic_write("  error_code : "); panic_hex(frame->error_code); panic_write("\n");
        if (frame->vector == 14) {
            panic_write("  cr2        : "); panic_hex(read_cr2());       panic_write("\n");
        }
        panic_write("  rip        : "); panic_hex(frame->rip);        panic_write("\n");
        panic_write("  rsp        : "); panic_hex(frame->rsp);        panic_write("\n");
        panic_write("  rflags     : "); panic_hex(frame->rflags);     panic_write("\n");
        panic_write("[IRIS][EXCEPTION] halting\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

void idt_init(void) {
    void (*isrs[32])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    };
    for (int i = 0; i < 32; i++)
        idt_set_entry(i, isrs[i]);

    /* IRQ0 — timer en vector 0x20 */
    idt_set_entry(32, isr32);

    idtr.size   = sizeof(idt) - 1;
    idtr.offset = (uint64_t)(uintptr_t)&idt;
    idt_flush((uint64_t)(uintptr_t)&idtr);
}
