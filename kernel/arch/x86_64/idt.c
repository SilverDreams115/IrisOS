#include <iris/idt.h>
#include <iris/pic.h>
#include <iris/scheduler.h>
#include <iris/task.h>
#include <iris/irq_routing.h>
#include <iris/nc/kprocess.h>
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
extern void isr33(void); /* IRQ1 — keyboard */
/* IRQ2-15: cubiertos para no triple-faultear ante IRQs espurios/inesperados */
extern void isr34(void); extern void isr35(void); extern void isr36(void);
extern void isr37(void); extern void isr38(void); extern void isr39(void);
extern void isr40(void); extern void isr41(void); extern void isr42(void);
extern void isr43(void); extern void isr44(void); extern void isr45(void);
extern void isr46(void); extern void isr47(void);

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
static void panic_dec(uint32_t v) {
    char buf[12]; int i = 11;
    buf[i] = 0;
    if (v == 0) { buf[--i] = '0'; }
    else { while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; } }
    panic_write(buf + i);
}

void isr_handler(struct full_frame *frame) {
    if (frame->vector == 32) {
        /* IRQ0 — timer tick. Enviar EOI primero para no bloquear el PIC. */
        pic_eoi(0);
        scheduler_tick();
        /* Preemptive: si el quantum expiró, yield desde el contexto del IRQ.
         * RFLAGS se salva/restaura en context_switch para cada tarea. */
        struct task *ct = task_current();
        if (ct && ct->need_resched)
            task_yield();
        return;
    }
    if (frame->vector == 33) {
        /* IRQ1 — PS/2 keyboard: route raw scancode to the userland kbd service. */
        uint8_t sc = inb_direct(0x60);
        (void)irq_routing_signal(1, sc);
        pic_eoi(1);
        return;
    }

    if (frame->vector >= 34 && frame->vector <= 47) {
        uint8_t irq = (uint8_t)(frame->vector - 32);
        /* IRQ7 (v39) y IRQ15 (v47) pueden ser espurios del PIC.
         * Para IRQ15 espurio hay que enviar EOI solo al PIC1, no al PIC2.
         * Por ahora descartamos ambos sin EOI al slave; el resto recibe EOI. */
        if (irq != 7 && irq != 15)
            pic_eoi(irq);
        return;
    }

    if (frame->vector < 32) {
        /* Detect origin: ring-3 user fault vs ring-0 kernel fault.
         * Double Fault (#DF=8), NMI (2), and Machine Check (18) are always
         * fatal regardless of CPL — they indicate unrecoverable hardware or
         * kernel state. All other exceptions from ring-3 kill only the
         * faulting task and let the scheduler continue. */
        int from_ring3 = (frame->cs & 3) == 3;
        int always_fatal = (frame->vector == 2 ||
                            frame->vector == 8 ||
                            frame->vector == 18);

        if (from_ring3 && !always_fatal) {
            struct task *ct = task_current();
            if (frame->vector == 14 && ct) {
                if (kprocess_resolve_demand_fault(ct, read_cr2()) == IRIS_OK)
                    return; /* PTE installed; CPU will retry the faulting instruction */
            }
            panic_write("[IRIS][FAULT] userland exception: ");
            panic_write(exception_names[frame->vector]);
            panic_write(" task=");
            panic_dec(ct ? ct->id : 0xFFFFFFFFu);
            panic_write(" rip="); panic_hex(frame->rip);
            if (frame->vector == 14) {
                panic_write(" cr2="); panic_hex(read_cr2());
            }
            panic_write(" err="); panic_hex(frame->error_code);
            panic_write("\n");
            if (ct) {
                uint64_t cr2 = (frame->vector == 14) ? read_cr2() : 0;
                kprocess_notify_fault(ct, frame->vector, frame->error_code,
                                      frame->rip, cr2);
            }
            task_exit_current();
            /* unreachable — task_exit_current() calls task_yield() */
        }

        /* Kernel exception or always-fatal: halt the machine. */
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

    /* Use IST1 for faults that can happen during CPL3 transition.
     * This prevents blind triple-fault resets and lets us print the real exception. */
    idt[8].ist  = 1;   /* Double Fault */
    idt[13].ist = 1;   /* General Protection Fault */
    idt[14].ist = 1;   /* Page Fault */

    /* IRQ0 — timer, IRQ1 — keyboard */
    idt_set_entry(32, isr32);
    idt_set_entry(33, isr33);
    /* IRQ2-15: handlers genéricos para PIC completo */
    idt_set_entry(34, isr34); idt_set_entry(35, isr35); idt_set_entry(36, isr36);
    idt_set_entry(37, isr37); idt_set_entry(38, isr38); idt_set_entry(39, isr39);
    idt_set_entry(40, isr40); idt_set_entry(41, isr41); idt_set_entry(42, isr42);
    idt_set_entry(43, isr43); idt_set_entry(44, isr44); idt_set_entry(45, isr45);
    idt_set_entry(46, isr46); idt_set_entry(47, isr47);

    idtr.size   = sizeof(idt) - 1;
    idtr.offset = (uint64_t)(uintptr_t)&idt;
    idt_flush((uint64_t)(uintptr_t)&idtr);
}
