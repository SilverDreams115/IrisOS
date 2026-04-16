#include <stdint.h>
#include <iris/serial.h>
#include <iris/boot_info.h>
#include <iris/pmm.h>
#include <iris/paging.h>
#include <iris/gdt.h>
#include <iris/idt.h>
#include <iris/pic.h>
#include <iris/scheduler.h>
#include <iris/task.h>
#include <iris/tss.h>
#include <iris/syscall.h>
#include <iris/fb.h>
#include <iris/irq_routing.h>
#include <iris/initrd.h>
#include <iris/elf_loader.h>
#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
#include <iris/phase3_selftest.h>
#endif
/* Transitional bootstrap services: real current boot path. */
#include <iris/svcmgr_bootstrap.h>

#define FB_ORANGE 0x00FF8800

static struct iris_boot_info saved_boot_info;

void iris_kernel_main(struct iris_boot_info *boot_info) {

    /* ── 1. Serial + banner ─────────────────────────────────────── */
    serial_init();
    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 14       \n");
    serial_write("====================================\n");
    serial_write("[IRIS][KERNEL] firmware services: OFF\n");

    /* ── 2. Boot info validation ────────────────────────────────── */
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

    /* ── 3. Core memory subsystems ──────────────────────────────── */
    serial_write("[IRIS][PMM] initializing...\n");
    pmm_init(&saved_boot_info);
    serial_write("[IRIS][PMM] free RAM: ");
    serial_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    serial_write(" MB\n");

    serial_write("[IRIS][PAGING] initializing...\n");
    paging_init(saved_boot_info.framebuffer.base, saved_boot_info.framebuffer.size);
    serial_write("[IRIS][PAGING] virtual memory active\n");

    /* ── 4. CPU tables + interrupt infrastructure ───────────────── */
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

    /* ── 5. Drivers ─────────────────────────────────────────────── */
    serial_write("[IRIS][FB] initializing...\n");
    fb_init(&saved_boot_info);
    fb_fill(FB_BLACK);
    {
        uint32_t stripe = saved_boot_info.framebuffer.height / 7;
        fb_draw_rect(0, stripe * 0, saved_boot_info.framebuffer.width, stripe, FB_RED);
        fb_draw_rect(0, stripe * 1, saved_boot_info.framebuffer.width, stripe, FB_ORANGE);
        fb_draw_rect(0, stripe * 2, saved_boot_info.framebuffer.width, stripe, FB_YELLOW);
        fb_draw_rect(0, stripe * 3, saved_boot_info.framebuffer.width, stripe, FB_GREEN);
        fb_draw_rect(0, stripe * 4, saved_boot_info.framebuffer.width, stripe, FB_CYAN);
        fb_draw_rect(0, stripe * 5, saved_boot_info.framebuffer.width, stripe, FB_BLUE);
        fb_draw_rect(0, stripe * 6, saved_boot_info.framebuffer.width, stripe, FB_IRIS);
    }
    serial_write("[IRIS][FB] framebuffer painted\n");

    serial_write("[IRIS][VFS] kernel backend retired from healthy boot\n");

    /* ── 6. Kernel services ─────────────────────────────────────── */
    serial_write("[IRIS][SYSCALL] initializing...\n");
    syscall_init();
    serial_write("[IRIS][SYSCALL] MSRs configured\n");

    serial_write("[IRIS][IRQ] initializing routing table...\n");
    irq_routing_init();

#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
    phase3_selftest_run();
#endif

    /* ── 7. Scheduler core ──────────────────────────────────────── */
    serial_write("[IRIS][SCHED] initializing...\n");
    scheduler_init();
    /* ── 8. Transitional bootstrap: service wiring ──────────────── */
    svcmgr_bootstrap_init(); /* spawn svcmgr and retain the root bootstrap channel */

    /* ── 9. First user task ─────────────────────────────────────── */
    serial_write("[IRIS][USER] preparing init process...\n");
#ifndef IRIS_ENABLE_RUNTIME_SELFTESTS
    /* Phase 21: spawn init as an ELF service from the initrd. */
    {
        const void *init_elf = 0;
        uint32_t    init_sz  = 0;
        struct task *ut = 0;

        if (!initrd_find("init", &init_elf, &init_sz)) {
            serial_write("[IRIS][USER] FATAL: init not found in initrd\n");
        } else {
            iris_elf_image_t img;
            iris_error_t lerr = elf_loader_load(init_elf, init_sz, &img);
            if (lerr != IRIS_OK) {
                serial_write("[IRIS][USER] FATAL: elf_loader_load(init) failed\n");
            } else {
                handle_id_t sm_bootstrap_h = HANDLE_INVALID;
                ut = task_spawn_elf(&img, 0);
                if (!ut) {
                    serial_write("[IRIS][USER] FATAL: task_spawn_elf(init) failed\n");
                    elf_loader_free_image(&img);
                } else {
                    iris_error_t br = svcmgr_bootstrap_attach_client(ut, RIGHT_WRITE,
                                                                      &sm_bootstrap_h);
                    if (br != IRIS_OK) {
                        serial_write("[IRIS][USER] WARN: svcmgr bootstrap attach failed\n");
                        task_abort_spawned_user(ut);
                        ut = 0;
                    } else {
                        task_set_bootstrap_arg0(ut, (uint64_t)sm_bootstrap_h);
                    }
                }
            }
        }
        if (ut) {
            serial_write("[IRIS][USER] init task created (ELF), id=");
            serial_write_dec(ut->id);
            serial_write("\n");
        } else {
            serial_write("[IRIS][USER] WARN: could not create init task\n");
        }
    }
#else
    {
        extern void user_selftest(void);
        struct task *st = task_spawn_user((uint64_t)(uintptr_t)user_selftest, 0);
        if (st) {
            handle_id_t sm_bootstrap_h = HANDLE_INVALID;
            iris_error_t br = svcmgr_bootstrap_attach_client(st, RIGHT_WRITE, &sm_bootstrap_h);
            if (br != IRIS_OK) {
                serial_write("[IRIS][USER] WARN: selftest bootstrap attach failed\n");
                task_abort_spawned_user(st);
                st = 0;
            } else {
                task_set_bootstrap_arg0(st, (uint64_t)sm_bootstrap_h);
            }
        }
        if (st) {
            serial_write("[IRIS][USER] selftest task created, id=");
            serial_write_dec(st->id);
            serial_write("\n");
        } else {
            serial_write("[IRIS][USER] WARN: could not create selftest task\n");
        }
    }
#endif

    /* ── 10. Scheduler start ────────────────────────────────────── */
    __asm__ volatile ("sti");
    serial_write("[IRIS][SCHED] running\n");
    serial_write("====================================\n");

    /* Let the first wave of bootstrap tasks start before the idle loop. */
    task_yield();
    task_yield();
    task_yield();
    task_yield();

    for (;;) __asm__ volatile ("hlt");
}
