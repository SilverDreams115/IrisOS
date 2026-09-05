#include <iris/idt.h>
#include <iris/user_ctx.h>
#include <iris/cpu_local.h>
#include <iris/pic.h>
#include <iris/scheduler.h>
#include <iris/task.h>
#include <iris/irq_routing.h>
#include <iris/lapic.h>
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

/* The frame isr_common builds IS a thread's user context; one definition,
 * shared with struct task (ledger D-1 step 3). */
#define full_frame iris_user_ctx

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
/* IRQ2-15: covered so a spurious or unexpected IRQ cannot triple-fault */
extern void isr34(void); extern void isr35(void); extern void isr36(void);
extern void isr37(void); extern void isr38(void); extern void isr39(void);
extern void isr40(void); extern void isr41(void); extern void isr42(void);
extern void isr43(void); extern void isr44(void); extern void isr45(void);
extern void isr46(void); extern void isr47(void);
extern void isr240(void); /* RESCHEDULE_IPI_VECTOR */

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

/*
 * ── Ledger D-1, step 3: the interrupted user context lives in the TCB ──────
 *
 * Every ring-3 kernel entry now copies the thread's whole register state into
 * its own TCB, and every ring-3 exit rebuilds the frame from the TCB of
 * whatever thread is current at that moment.
 *
 * Today the two are always the same thread, because kernel stacks are still
 * per-thread: a handler that switches away switches stacks too, and comes back
 * to this frame on this stack.  So this pass is, deliberately, a no-op that
 * costs two 22-word copies per interrupt.
 *
 * It is the precondition for the change that is not a no-op.  With ONE kernel
 * stack per core, a handler that hands the CPU to another thread leaves this
 * frame on a stack the incoming thread is about to use — the outgoing thread's
 * context has to already be somewhere else, and the incoming thread's has to
 * come from somewhere other than the stack.  Both of those are the TCB.  Doing
 * the move first, while the frame is still authoritative, means the switch to
 * a per-core stack changes WHERE execution resumes and not WHAT is restored.
 *
 * Ring-0 entries are skipped: a kernel fault does not return to a user context
 * and the idle loop has no TCB worth writing to.
 */
static _Atomic uint64_t irq_ctx_saves = 0;

uint64_t irq_user_ctx_saves(void) {
    return __atomic_load_n(&irq_ctx_saves, __ATOMIC_RELAXED);
}

void isr_save_user_ctx(struct full_frame *frame) {
    if ((frame->cs & 3u) != 3u) return;
    struct task *t = task_current();
    if (!t) return;
    t->user_ctx    = *frame;
    /* Step 3: this thread's resume is now an iretq from its TCB.  Written
     * where it ENTERS the kernel, because that is the event that decides it. */
    t->resume_user = 1u;
    __atomic_fetch_add(&irq_ctx_saves, 1u, __ATOMIC_RELAXED);
}

/*
 * Stage 9-evt step 3 — the exit decision.
 *
 * Called after the handler and before the restore.  If this interrupt came
 * from RING 3 and the thread it interrupted should not go straight back —
 * because its quantum expired, or the handler blocked it, or it died — the CPU
 * goes to the dispatcher instead of returning through this frame.
 *
 * Doing it HERE rather than inside the handler is the whole of step 3's second
 * half.  A handler that switched would hand the CPU away with this interrupt
 * frame live on the stack, and once that stack belongs to the core rather than
 * to the thread, the incoming thread is about to overwrite it.  Leaving from
 * the exit path means the frame is finished with: everything the thread needs
 * is already in its TCB, put there by isr_save_user_ctx on the way in.
 *
 * A ring-0 interrupt never dispatches.  The only ring-0 code that runs with
 * interrupts enabled is the dispatcher's own idle wait, and an interrupt there
 * must return to the `hlt` so the loop can ask again — the loop IS the
 * scheduler at that point, and jumping into it from inside itself would
 * abandon a stack frame it is standing on.
 */
void isr_maybe_dispatch(struct full_frame *frame) {
    if ((frame->cs & 3u) != 3u) return;
    struct task *t = task_current();
    if (!t) return;
    if (t->state == TASK_RUNNING && !t->need_resched) return;
    core_dispatch_enter(t);          /* never returns */
}

void isr_restore_user_ctx(struct full_frame *frame) {
    if ((frame->cs & 3u) != 3u) return;
    struct task *t = task_current();
    if (!t) return;
    /*
     * The vector and error code belong to THIS entry, not to the context: a
     * thread resumed here may have been saved on a different vector, and
     * handing the stubs somebody else's vector would pop the wrong amount of
     * stack.  Everything the CPU and the thread care about comes from the TCB.
     */
    uint64_t vec = frame->vector, err = frame->error_code;
    *frame = t->user_ctx;
    frame->vector     = vec;
    frame->error_code = err;
}

void isr_handler(struct full_frame *frame) {
    if (frame->vector == 32) {
        /* IRQ0 — timer tick. Send EOI first so the PIC is not blocked. */
        pic_eoi(0);
        scheduler_tick();
        /* Preemptive: if the quantum expired, yield from the IRQ context.
         * RFLAGS is saved/restored in context_switch for each task. */
        /*
         * Step 3: the tick does not SWITCH.  It marks, and the exit path
         * below dispatches — because switching here would hand the CPU away
         * with this interrupt frame live on the stack, and once that stack
         * belongs to the core the incoming thread is about to use it.
         */
        return;
    }
    if (frame->vector == 33) {
        /* IRQ1 — PS/2 keyboard: seL4-style deferred ACK.
         * Mask the IRQ line BEFORE sending EOI so the PIC cannot re-assert
         * IRQ1 between EOI and ring-3 reading port 0x60.  The handler calls
         * SYS_IRQ_ACK to unmask after consuming the byte. */
        pic_set_irq_mask(1, 1);
        pic_eoi(1);
        if (irq_routing_signal(1, 0) < 0)
            pic_set_irq_mask(1, 0); /* no handler: unmask immediately */
        return;
    }

    if (frame->vector == RESCHEDULE_IPI_VECTOR) {
        lapic_eoi();
        struct task *ct = task_current();
        if (ct) ct->need_resched = 1;
        return;
    }

    if (frame->vector >= 34 && frame->vector <= 47) {
        uint8_t irq = (uint8_t)(frame->vector - 32);
        /* IRQ7/IRQ15 can be spurious; discard without EOI to avoid
         * acknowledging a real IRQ that hasn't fired. */
        if (irq == 7 || irq == 15) return;
        /* Deferred ACK: mask before EOI, signal, unmask if undelivered. */
        pic_set_irq_mask(irq, 1);
        pic_eoi(irq);
        if (irq_routing_signal(irq, 0) < 0)
            pic_set_irq_mask(irq, 0);
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
            if (ct) {
                uint64_t cr2 = (frame->vector == 14) ? read_cr2() : 0;
                int notified = kprocess_notify_fault(ct, frame->vector,
                                                     frame->error_code,
                                                     frame->rip, cr2);
                if (notified) {
                    /* Blocked, not switched: the exit path sees a thread that
                     * is no longer runnable and dispatches past it. */
                    ct->state = TASK_BLOCKED_FAULT;
                    return;
                }
            }
            /* No exception handler registered — log and terminate the task. */
            kprocess_fault_stat_nohandler();
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

    /* IST1 — #GP and #PF: must not trust the faulting RSP (it may be corrupted
     * or user-controlled at the CPL3→CPL0 boundary).  16 KB per CPU in gdt.c. */
    idt[13].ist = 1;   /* General Protection Fault */
    idt[14].ist = 1;   /* Page Fault */

    /* IST2 — NMI and Machine Check: can fire at any time, including while IST1
     * is active.  Separate stack prevents corruption of an in-flight #PF/#GP frame. */
    idt[2].ist  = 2;   /* NMI */
    idt[18].ist = 2;   /* Machine Check */

    /* IST3 — #DF: Double Fault fires precisely when fault delivery itself faulted,
     * which means IST1 may already have an active frame.  Giving #DF its own IST3
     * ensures the double-fault panic output is coherent even if IST1 is in use.
     * Without this, a #DF during a #PF handler would reset RSP to IST1-top and
     * overwrite the live handler frame, garbling the crash dump. */
    idt[8].ist  = 3;   /* Double Fault */

    /* IRQ0 — timer, IRQ1 — keyboard */
    idt_set_entry(32, isr32);
    idt_set_entry(33, isr33);
    /* IRQ2-15: generic handlers for the full PIC */
    idt_set_entry(34, isr34); idt_set_entry(35, isr35); idt_set_entry(36, isr36);
    idt_set_entry(37, isr37); idt_set_entry(38, isr38); idt_set_entry(39, isr39);
    idt_set_entry(40, isr40); idt_set_entry(41, isr41); idt_set_entry(42, isr42);
    idt_set_entry(43, isr43); idt_set_entry(44, isr44); idt_set_entry(45, isr45);
    idt_set_entry(46, isr46); idt_set_entry(47, isr47);
    idt_set_entry(RESCHEDULE_IPI_VECTOR, isr240);

    idtr.size   = sizeof(idt) - 1;
    idtr.offset = (uint64_t)(uintptr_t)&idt;
    idt_flush((uint64_t)(uintptr_t)&idtr);
}
