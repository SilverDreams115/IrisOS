#include <stdint.h>
#include <iris/pic.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

#define ICW1_ICW4    0x01
#define ICW1_INIT    0x10
#define ICW4_8086    0x01

#define PIT_COMMAND  0x43
#define PIT_CHANNEL0 0x40
#define PIT_BASE_FREQUENCY 1193182U

static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static inline void io_wait(void) {
  __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

void pic_eoi(uint8_t irq) {
  if (irq >= 8) {
    outb(PIC2_COMMAND, PIC_EOI);
  }
  outb(PIC1_COMMAND, PIC_EOI);
}

void pic_init(void) {
  uint8_t mask1 = inb(PIC1_DATA);
  uint8_t mask2 = inb(PIC2_DATA);

  outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
  io_wait();
  outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
  io_wait();

  outb(PIC1_DATA, 0x20);
  io_wait();
  outb(PIC2_DATA, 0x28);
  io_wait();

  outb(PIC1_DATA, 4);
  io_wait();
  outb(PIC2_DATA, 2);
  io_wait();

  outb(PIC1_DATA, ICW4_8086);
  io_wait();
  outb(PIC2_DATA, ICW4_8086);
  io_wait();

  mask1 &= (uint8_t)~(1 << 0);
  outb(PIC1_DATA, mask1);
  outb(PIC2_DATA, mask2);
}

void pit_init(uint32_t frequency_hz) {
  uint32_t divisor;

  if (frequency_hz == 0) {
    frequency_hz = 100;
  }

  divisor = PIT_BASE_FREQUENCY / frequency_hz;

  outb(PIT_COMMAND, 0x36);
  outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
  outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}
