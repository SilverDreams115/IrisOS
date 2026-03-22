#include <stdint.h>
#include <iris/serial.h>
#include <iris/kernel.h>
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
#include <iris/pci.h>
#include <iris/keyboard.h>
#include <iris/fb.h>
#include <iris/vfs.h>
#include <iris/irq_routing.h>
#include <iris/nameserver.h>
/* transicional bootstrap helpers */
#include <iris/svcmgr_bootstrap.h>
/* legacy demo — only included when IRIS_ENABLE_IPC_DEMO is defined */
#ifdef IRIS_ENABLE_IPC_DEMO
#include <iris/ipc_demo.h>
#endif

#define FB_ORANGE 0x00FF8800

static struct iris_boot_info saved_boot_info;

void iris_kernel_main(struct iris_boot_info *boot_info) {

    /* ── 1. Serial + banner ─────────────────────────────────────── */
    serial_init();
    serial_write("\n");
    serial_write("====================================\n");
    serial_write("       IRIS KERNEL - STAGE 13       \n");
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

    serial_write("[IRIS][VFS] initializing...\n");
    vfs_init();
    vfs_mkdir("dev");
    serial_write("[IRIS][VFS] /dev created\n");
    /* VFS smoke test — create, write, read back, stat */
    {
        const char *msg = "Hello from IrisOS VFS!\n";
        uint32_t msglen = 0;
        while (msg[msglen]) msglen++;
        int32_t fd = vfs_open("iris.txt", VFS_O_CREATE | VFS_O_WRITE);
        vfs_write(fd, msg, msglen);
        vfs_close(fd);

        char rdbuf[64];
        for (int i = 0; i < 64; i++) rdbuf[i] = 0;
        fd = vfs_open("iris.txt", VFS_O_READ);
        int32_t n = vfs_read(fd, rdbuf, 63);
        vfs_close(fd);
        serial_write("[IRIS][VFS] read iris.txt (");
        serial_write_dec((uint64_t)n);
        serial_write(" bytes): ");
        serial_write(rdbuf);

        uint32_t fsize = 0;
        vfs_stat("iris.txt", &fsize);
        serial_write("[IRIS][VFS] stat iris.txt size=");
        serial_write_dec(fsize);
        serial_write("\n");
        serial_write("[IRIS][VFS] OK\n");
    }

    serial_write("[IRIS][PCI] initializing...\n");
    pci_init();

    serial_write("[IRIS][KBD] initializing...\n");
    kbd_init();

    /* ── 6. Kernel services ─────────────────────────────────────── */
    serial_write("[IRIS][SYSCALL] initializing...\n");
    syscall_init();
    serial_write("[IRIS][SYSCALL] MSRs configured\n");

    serial_write("[IRIS][IRQ] initializing routing table...\n");
    irq_routing_init();

    serial_write("[IRIS][NS] initializing...\n");
    ns_init();
    serial_write("[IRIS][NS] bootstrap registry ready\n");

    /* ── 7. Scheduler + demo tasks (legacy) ─────────────────────── */
    serial_write("[IRIS][SCHED] initializing...\n");
    scheduler_init();
#ifdef IRIS_ENABLE_IPC_DEMO
    /* Legacy producer/consumer IPC demo.  Gated so the real boot path
     * can run without demo noise.  Remove IRIS_ENABLE_IPC_DEMO from
     * KERNEL_CFLAGS in the Makefile to disable. */
    ipc_demo_start();
#endif

    /* ── 8. Bootstrap transicional: service wiring ──────────────── */
    svcmgr_bootstrap_init(); /* spawn svcmgr + queue kbd spawn request */

    /* ── 9. User init process ───────────────────────────────────── */
    serial_write("[IRIS][USER] preparing init process...\n");
    extern void user_init(void);
    struct task *ut = task_create_user((uint64_t)(uintptr_t)user_init);
    if (ut) {
        serial_write("[IRIS][USER] init task created, id=");
        serial_write_dec(ut->id);
        serial_write("\n");
    } else {
        serial_write("[IRIS][USER] WARN: could not create user task\n");
    }

    /* ── 10. Scheduler start ────────────────────────────────────── */
    __asm__ volatile ("sti");
    serial_write("[IRIS][SCHED] IPC running\n");
    serial_write("====================================\n");

    /* yield a few times to let all tasks run including user_init */
    task_yield(); /* -> producer */
    task_yield(); /* -> consumer */
    task_yield(); /* -> user_init */
    task_yield(); /* -> back */

    for (;;) __asm__ volatile ("hlt");
}
