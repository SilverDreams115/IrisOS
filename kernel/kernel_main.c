#include <stdint.h>
#include <iris/klog.h>
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
#include <iris/fb_info.h>
#include <iris/irq_routing.h>
#include <iris/nc/kbootcap.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kobject.h>
#include <iris/nc/rights.h>
#include <iris/cpu_local.h>
#include <iris/lapic.h>
#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
#include <iris/phase3_selftest.h>
#endif

static struct iris_boot_info saved_boot_info;

struct iris_fb_params g_iris_fb_params;
int                   g_iris_fb_params_valid = 0;

void iris_kernel_main(struct iris_boot_info *boot_info) {

    /* ── 1. Serial + banner ─────────────────────────────────────── */
    serial_init();
    klog_write("\n");
    klog_write("====================================\n");
    klog_write("       IRIS KERNEL - PHASE 66       \n");
    klog_write("====================================\n");
    klog_write("[IRIS][KERNEL] firmware services: OFF\n");

    /* ── 2. Boot info validation ────────────────────────────────── */
    if (!boot_info || boot_info->magic != IRIS_BOOTINFO_MAGIC) {
        klog_write("[IRIS][KERNEL] FATAL: invalid boot protocol\n");
        for (;;) __asm__ volatile ("hlt");
    }
    {
        uint64_t *src   = (uint64_t *)(uintptr_t)boot_info;
        uint64_t *dst   = (uint64_t *)(uintptr_t)&saved_boot_info;
        uint64_t  words = sizeof(struct iris_boot_info) / sizeof(uint64_t);
        for (uint64_t i = 0; i < words; i++) dst[i] = src[i];
    }
    klog_write("[IRIS][KERNEL] boot protocol OK (v");
    klog_write_dec(saved_boot_info.version);
    klog_write(")\n");

    /* ── 3. Core memory subsystems ──────────────────────────────── */
    klog_write("[IRIS][PMM] initializing...\n");
    pmm_init(&saved_boot_info);
    klog_write("[IRIS][PMM] free RAM: ");
    klog_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    klog_write(" MB\n");

    klog_write("[IRIS][PAGING] initializing...\n");
    paging_init(saved_boot_info.framebuffer.base, saved_boot_info.framebuffer.size);
    paging_enable_pcid();
    klog_write("[IRIS][PAGING] virtual memory active\n");

    /* ── 4. CPU tables + interrupt infrastructure ───────────────── */
    klog_write("[IRIS][GDT] initializing...\n");
    gdt_init();
    klog_write("[IRIS][GDT] OK\n");

    klog_write("[IRIS][CPU] probing LAPIC...\n");
    int lp = lapic_probe();
    if (lp) {
        cpu_local[0].lapic_id = lapic_id();
        klog_write("[IRIS][CPU] LAPIC present (PIC/PIT remain active as timer source)\n");
    } else {
        klog_write("[IRIS][CPU] no LAPIC (legacy PIC mode)\n");
    }

    klog_write("[IRIS][PIC] remapping IRQs...\n");
    pic_init();
    klog_write("[IRIS][PIT] timer at 100 Hz...\n");
    pit_init(100);

    klog_write("[IRIS][IDT] initializing...\n");
    idt_init();
    klog_write("[IRIS][IDT] OK\n");

    if (lapic_is_active())
        lapic_software_enable();

    /* ── 5. Framebuffer params ─────────────────────────────────────── */
    klog_write("[IRIS][FB] saving params for ring-3 fb service\n");
    g_iris_fb_params.phys   = saved_boot_info.framebuffer.base;
    g_iris_fb_params.size   = saved_boot_info.framebuffer.size;
    g_iris_fb_params.width  = saved_boot_info.framebuffer.width;
    g_iris_fb_params.height = saved_boot_info.framebuffer.height;
    g_iris_fb_params.stride = saved_boot_info.framebuffer.pixels_per_scanline;
    g_iris_fb_params.bpp    = 4u;
    g_iris_fb_params_valid  = 1;

    klog_write("[IRIS][VFS] kernel backend retired from healthy boot\n");

    /* ── 6. Kernel services ─────────────────────────────────────── */
    klog_write("[IRIS][SYSCALL] initializing...\n");
    syscall_init();
    klog_write("[IRIS][SYSCALL] MSRs configured\n");

    klog_write("[IRIS][IRQ] initializing routing table...\n");
    irq_routing_init();

#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
    phase3_selftest_run();
#endif

    /* ── 7. Scheduler core ──────────────────────────────────────── */
    klog_write("[IRIS][SCHED] initializing...\n");
    scheduler_init();

    /* ── 8. First user task ─────────────────────────────────────── */
    klog_write("[IRIS][USER] preparing bootstrap task...\n");
    {
        struct task *ut = 0;

        ut = task_spawn_user(0);
        if (!ut) {
            klog_write("[IRIS][USER] FATAL: task_spawn_user(userboot) failed\n");
        } else {
            struct KBootstrapCap *cap = kbootcap_alloc(
                IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_HW_ACCESS |
                IRIS_BOOTCAP_KDEBUG | IRIS_BOOTCAP_FRAMEBUFFER);
            if (!cap) {
                klog_write("[IRIS][USER] FATAL: kbootcap_alloc failed\n");
                task_abort_spawned_user(ut);
                ut = 0;
            } else {
                handle_id_t cap_h = handle_table_insert(
                    &ut->process->handle_table, &cap->base,
                    RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER);
                kobject_release(&cap->base);
                if (cap_h == HANDLE_INVALID) {
                    klog_write("[IRIS][USER] FATAL: cap handle insert failed\n");
                    task_abort_spawned_user(ut);
                    ut = 0;
                } else {
                    task_set_bootstrap_arg0(ut, (uint64_t)cap_h);
                }
            }
        }
        if (ut) {
            klog_write("[IRIS][USER] bootstrap task created (ring-3 loader), id=");
            klog_write_dec(ut->id);
            klog_write("\n");
        } else {
            klog_write("[IRIS][USER] WARN: could not create bootstrap task\n");
        }
    }

    /* ── 9. Scheduler start ─────────────────────────────────────── */
    klog_write("[IRIS][SCHED] running\n");
    klog_write("[IRIS][BOOT] waiting for first userland wave\n");
    klog_write("====================================\n");
    __asm__ volatile ("sti");

    /* Let the first wave of bootstrap tasks start before the idle loop. */
    task_yield();
    task_yield();
    task_yield();
    task_yield();

    for (;;) {
        __asm__ volatile ("sti");
        task_yield();
    }
}
