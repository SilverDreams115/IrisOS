#include <stdint.h>
#include <iris/kernel.h>
#include <iris/boot_info.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/gdt.h>
#include <iris/idt.h>
#include <iris/pic.h>
#include <iris/scheduler.h>
#include <iris/task.h>

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x01);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
    outb(COM1_PORT + 4, 0x0B);
}
static int serial_ready(void) { return inb(COM1_PORT + 5) & 0x20; }
static void serial_putc(char c) {
    while (!serial_ready()) {}
    outb(COM1_PORT, (uint8_t)c);
}
static void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}
static void serial_write_dec(uint64_t value) {
    char buf[21]; int i = 20;
    buf[i] = '\0';
    if (value == 0) { buf[--i] = '0'; }
    else { while (value > 0) { buf[--i] = '0' + (int)(value % 10); value /= 10; } }
    serial_write(&buf[i]);
}

static struct iris_boot_info saved_boot_info;

/* ── tareas ────────────────────────────────────────────────────────── */

static volatile uint64_t task_a_ticks = 0;
static volatile uint64_t task_b_ticks = 0;

static void task_a(void) {
    for (;;) {
        task_a_ticks++;
        serial_write("[TASK A] tick ");
        serial_write_dec(task_a_ticks);
        serial_write("\n");
        task_yield();
    }
}

static void task_b(void) {
    for (;;) {
        task_b_ticks++;
        serial_write("[TASK B] tick ");
        serial_write_dec(task_b_ticks);
        serial_write("\n");
        task_yield();
    }
}

/* ── kernel entry ─────────────────────────────────────────────────── */

void iris_kernel_main(struct iris_boot_info *boot_info) {
    serial_init();

    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 7        \n");
    serial_write("====================================\n");
    serial_write("[IRIS][KERNEL] firmware services: OFF\n");

    if (!boot_info || boot_info->magic != IRIS_BOOTINFO_MAGIC) {
        serial_write("[IRIS][KERNEL] FATAL: invalid boot protocol\n");
        for (;;) __asm__ volatile ("hlt");
    }

    {
        uint64_t *src   = (uint64_t *)(uintptr_t)boot_info;
        uint64_t *dst   = (uint64_t *)(uintptr_t)&saved_boot_info;
        uint64_t  words = sizeof(struct iris_boot_info) / sizeof(uint64_t);
        for (uint64_t i = 0; i < words; i++) dst[i] = src[i];
    }

    serial_write("[IRIS][KERNEL] boot protocol OK (v");
    serial_write_dec(saved_boot_info.version);
    serial_write(")\n");

    serial_write("[IRIS][PMM] initializing...\n");
    pmm_init(&saved_boot_info);
    serial_write("[IRIS][PMM] free RAM: ");
    serial_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    serial_write(" MB\n");

    serial_write("[IRIS][PAGING] initializing...\n");
    paging_init(saved_boot_info.framebuffer.base,
                saved_boot_info.framebuffer.size);
    serial_write("[IRIS][PAGING] virtual memory active\n");

    serial_write("[IRIS][GDT] initializing...\n");
    gdt_init();
    serial_write("[IRIS][GDT] OK\n");

    serial_write("[IRIS][PIC] remapping IRQs...\n");
    pic_init();
    serial_write("[IRIS][PIT] timer at 100 Hz...\n");
    pit_init(100);

    serial_write("[IRIS][IDT] initializing...\n");
    idt_init();
    serial_write("[IRIS][IDT] OK\n");

    serial_write("[IRIS][SCHED] initializing...\n");
    scheduler_init();
    scheduler_add_task(task_a);
    scheduler_add_task(task_b);
    serial_write("[IRIS][SCHED] tasks A and B created\n");

    serial_write("[IRIS][SCHED] enabling interrupts...\n");
    __asm__ volatile ("sti");

    serial_write("[IRIS][SCHED] running — cooperative scheduler, timer IRQ alive\n");
    serial_write("====================================\n");

    /* primer arranque cooperativo: saltar desde idle/kernel al primer task */
    task_yield();

    for (;;) __asm__ volatile ("hlt");
}
