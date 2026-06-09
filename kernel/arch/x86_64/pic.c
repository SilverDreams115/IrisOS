#include <iris/pic.h>
#include <iris/tsc.h>
#include <iris/klog.h>
#include <stdint.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20
#define ICW1_INIT 0x11
#define ICW4_8086 0x01

#define PIT_CH0          0x40
#define PIT_CH2          0x42
#define PIT_CMD          0x43
#define PIT_GATE_PORT    0x61
#define PIT_HZ           1193182UL
#define PIT_CH2_CAL_DIV  11932u   /* ~10 ms one-shot window */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void io_wait(void) { outb(0x80, 0); }

void pic_set_irq_mask(uint8_t irq, int masked) {
    uint16_t data_port;
    uint8_t bit;
    uint8_t mask;

    if (irq >= 16) return;
    if (irq < 8) {
        data_port = PIC1_DATA;
        bit = irq;
    } else {
        data_port = PIC2_DATA;
        bit = (uint8_t)(irq - 8);
    }

    mask = inb(data_port);
    if (masked)
        mask = (uint8_t)(mask | (uint8_t)(1u << bit));
    else
        mask = (uint8_t)(mask & (uint8_t)~(uint8_t)(1u << bit));
    outb(data_port, mask);
}

void pic_init(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD,  ICW1_INIT); io_wait();
    outb(PIC2_CMD,  ICW1_INIT); io_wait();
    outb(PIC1_DATA, 0x20);      io_wait();
    outb(PIC2_DATA, 0x28);      io_wait();
    outb(PIC1_DATA, 0x04);      io_wait();
    outb(PIC2_DATA, 0x02);      io_wait();
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, mask1 & 0xFE); /* enable IRQ0 (timer); keep IRQ1 masked until routed */
    outb(PIC2_DATA, mask2);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

volatile uint64_t tsc_hz   = 0;
volatile uint64_t tsc_boot = 0;

void pit_init(uint32_t hz) {
    uint32_t divisor = (uint32_t)(PIT_HZ / hz);
    outb(PIT_CMD, 0x36u);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFFu));

    /* TSC calibration: PIT CH2 mode-0 one-shot (~10 ms).
     * Bit 0 of port 0x61 = CH2 gate; bit 1 = speaker; bit 5 = CH2 output. */
    outb(PIT_GATE_PORT, (uint8_t)((inb(PIT_GATE_PORT) & 0xFDu) | 0x01u));
    outb(PIT_CMD, 0xB0u);   /* CH2, LSB+MSB, mode 0, binary */
    outb(PIT_CH2, (uint8_t)(PIT_CH2_CAL_DIV & 0xFFu));
    outb(PIT_CH2, (uint8_t)((PIT_CH2_CAL_DIV >> 8) & 0xFFu));

    uint64_t t0 = iris_rdtsc();
    while (!(inb(PIT_GATE_PORT) & 0x20u)) { __asm__ volatile ("pause"); }
    uint64_t t1 = iris_rdtsc();

    tsc_hz   = (t1 - t0) * (uint64_t)PIT_HZ / (uint64_t)PIT_CH2_CAL_DIV;
    tsc_boot = t0;
    klog_write("[IRIS][TSC] calibrated: ");
    klog_write_dec(tsc_hz / 1000000ULL);
    klog_write(" MHz\n");
}
