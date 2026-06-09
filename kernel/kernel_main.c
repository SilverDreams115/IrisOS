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
#include <iris/nc/kcnode.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/rights.h>
#include <iris/cpu_local.h>
#include <iris/lapic.h>
#include <iris/kslab.h>
#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
#include <iris/phase3_selftest.h>
#endif

static struct iris_boot_info saved_boot_info;

struct iris_fb_params g_iris_fb_params;
int                   g_iris_fb_params_valid = 0;

static inline void _early_putc(char c) {
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
}

void iris_kernel_main(struct iris_boot_info *boot_info) {

    _early_putc('K'); /* raw serial: reached kernel_main */

    /* ── 1. Serial + banner ─────────────────────────────────────── */
    serial_init();
    _early_putc('S'); /* raw serial: serial_init returned */
    klog_write("\n");
    klog_write("====================================\n");
    klog_write("       IRIS KERNEL - PHASE 102\n");
    klog_write("====================================\n");
    klog_write("[IRIS][KERNEL] firmware services: OFF\n");

    /* ── 2. Boot info validation ────────────────────────────────── */
    _early_putc('B'); /* raw serial: boot_info validation */
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
    _early_putc('P'); /* raw serial: pmm_init */
    klog_write("[IRIS][PMM] initializing...\n");
    pmm_init(&saved_boot_info);
    klog_write("[IRIS][PMM] free RAM: ");
    klog_write_dec((pmm_free_pages() * 4096) / (1024 * 1024));
    klog_write(" MB\n");

    _early_putc('G'); /* raw serial: paging_init */
    klog_write("[IRIS][PAGING] initializing...\n");
    paging_init(saved_boot_info.framebuffer.base, saved_boot_info.framebuffer.size);
    paging_enable_pcid();
    _early_putc('g'); /* raw serial: paging done */
    klog_write("[IRIS][PAGING] virtual memory active\n");

    /* Activate the O(log N) buddy allocator now that the physmap is live. */
    pmm_buddy_setup();
    klog_write("[IRIS][PMM] buddy allocator active\n");

    /* Reserve 4 MB (1024 pages) from the PMM as the kernel object slab.
     * All typed kernel object headers (KProcess, KChannel, KEndpoint, …) are
     * allocated from this pool via kslab_alloc instead of directly from the PMM,
     * allowing all remaining PMM blocks to be handed to userspace as KUntyped caps. */
    {
        uint64_t kslab_phys = pmm_alloc_pages(1024u);
        if (kslab_phys == 0) {
            klog_write("[IRIS][KSLAB] FATAL: cannot reserve kernel slab backing\n");
            for (;;) __asm__ volatile ("hlt");
        }
        kslab_init(kslab_phys, 1024u);
        klog_write("[IRIS][KSLAB] kernel object slab active (4 MB)\n");
    }

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
                    /*
                     * Fase 3.5: dual insert — also publish KBootstrapCap in
                     * root CNode slot BOOT_CPTR_BOOTSTRAP_CAP (slot 1).
                     * kcnode_mint takes its own kobject_retain+active_retain,
                     * giving the CNode slot independent ownership from the
                     * legacy handle.  Refcount after:
                     *   handle owns retain+active = 1+1
                     *   CNode  owns retain+active = 1+1
                     * Boot failure on CSpace insert is non-fatal: KBootstrapCap
                     * remains accessible via the legacy bootstrap_cap_h handle.
                     */
                    if (ut->process->cspace_root_h != HANDLE_INVALID) {
                        struct KObject *broot_obj = 0;
                        iris_rights_t   broot_r;
                        if (handle_table_get_object(
                                &ut->process->handle_table,
                                ut->process->cspace_root_h,
                                &broot_obj, &broot_r) == IRIS_OK) {
                            if (broot_obj->type == KOBJ_CNODE) {
                                iris_error_t bme = kcnode_mint(
                                    (struct KCNode *)broot_obj,
                                    BOOT_CPTR_BOOTSTRAP_CAP,
                                    &cap->base,
                                    RIGHT_READ | RIGHT_DUPLICATE |
                                    RIGHT_TRANSFER);
                                if (bme == IRIS_OK)
                                    klog_write("[IRIS][USER] boot bootstrap"
                                               " cap CSpace grants OK\n");
                            }
                            kobject_release(broot_obj);
                        }
                    }
                }
            }
        }
        if (ut) {
            klog_write("[IRIS][USER] bootstrap task created (ring-3 loader), id=");
            klog_write_dec(ut->id);
            klog_write("\n");

                    /*
                     * Fase 4: create KVSpace for userboot/root task and publish
                     * it in root CNode slot BOOT_CPTR_VSPACE (slot 2).
                     *
                     * KVSpace is a non-owning wrapper in Fase 4: it records the
                     * cr3 value and an invalidation flag.  Page tables remain
                     * owned by KProcess; KVSpace does NOT free them.
                     *
                     * Ref-count after this block:
                     *   process->vspace lifecycle ref   → refcount=1
                     *   kcnode_mint (retain+active)     → refcount=2, active=1
                     *
                     * Boot failure on OOM or CSpace insert is non-fatal: the
                     * process still boots and demand paging is unaffected.
                     */
                    if (ut->process->cr3 && ut->process->cspace_root_h != HANDLE_INVALID) {
                        struct KVSpace *vs = kvspace_alloc(ut->process->cr3);
                        if (vs) {
                            /* Store lifecycle ref in process. */
                            kobject_retain(&vs->base);
                            ut->process->vspace = vs;
                            kobject_release(&vs->base); /* drop alloc ref */

                            /* Also publish in root CNode slot 2 (BOOT_CPTR_VSPACE). */
                            struct KObject *vroot_obj = 0;
                            iris_rights_t   vroot_r;
                            if (handle_table_get_object(
                                    &ut->process->handle_table,
                                    ut->process->cspace_root_h,
                                    &vroot_obj, &vroot_r) == IRIS_OK) {
                                if (vroot_obj->type == KOBJ_CNODE) {
                                    iris_error_t vme = kcnode_mint(
                                        (struct KCNode *)vroot_obj,
                                        BOOT_CPTR_VSPACE,
                                        &vs->base,
                                        RIGHT_READ | RIGHT_DUPLICATE |
                                        RIGHT_TRANSFER);
                                    if (vme == IRIS_OK)
                                        klog_write("[IRIS][USER] boot vspace"
                                                   " CSpace grants OK\n");
                                }
                                kobject_release(vroot_obj);
                            }
                        }
                    }

            /*
             * Ph76: drain free buddy blocks into KUntyped caps for userboot.
             *
             * IRIS_PMM_KERNEL_RUNTIME_RESERVE pages are kept in the PMM for
             * kernel-internal runtime allocators that bypass the KUntyped model:
             *
             *   • paging_map_checked_in: page-table intermediate pages (PDPT/PD/PT)
             *   • kprocess_resolve_demand_fault: one page per user demand-fault
             *   • kvmo_create: pages[] metadata array (pmm_alloc_pages)
             *   • sys_initrd_vmo: ELF copy pages (pmm_alloc_page × page_capacity)
             *   • kstack_alloc: 2 pages per task kernel stack
             *   • paging_create_user: 1 page per process PML4
             *
             * The post-alloc check (after pmm_alloc_block) handles the case
             * where a large block would push pmm_free_pages below the reserve;
             * that block is returned to the buddy and the drain stops.
             *
             * PHASE-1-DEBT: as long as IRIS uses kernel-side demand paging the
             * PMM reserve must persist indefinitely.  The seL4-correct fix is to
             * remove demand paging and require userspace to retype frames
             * explicitly via SYS_UNTYPED_RETYPE before mapping them.
             *
             * Fase 3.4 (dual mode): every boot KUntyped is also inserted into
             * slot (BOOT_CPTR_UNTYPED_START + drain_index) of the process's
             * root CNode so userboot can discover it via CPtr.  The legacy
             * handle-table insert is kept for backward compatibility.
             * Rights are identical on both paths; neither reference has greater
             * authority than the other.  Boot failure on CSpace insert is
             * non-fatal: the block remains accessible via the legacy handle.
             */
#define IRIS_PMM_KERNEL_RUNTIME_RESERVE  4096u  /* 16 MB for kernel runtime */
            {
                uint32_t ut_count        = 0;
                uint32_t ut_cspace_count = 0;
                for (;;) {
                    if (pmm_free_pages() <= IRIS_PMM_KERNEL_RUNTIME_RESERVE)
                        break;
                    uint32_t order;
                    uint64_t blk_phys = pmm_alloc_block(&order);
                    if (blk_phys == 0) break;
                    uint32_t blk_pages = 1u << order;
                    /* Post-check: if this block violated the reserve, return it. */
                    if (pmm_free_pages() < IRIS_PMM_KERNEL_RUNTIME_RESERVE) {
                        pmm_free_contig(blk_phys, blk_pages);
                        break;
                    }
                    uint64_t size = (uint64_t)blk_pages * 4096u;
                    struct KUntyped *boot_ut = kuntyped_create(blk_phys, size, 0);
                    if (!boot_ut) {
                        pmm_free_contig(blk_phys, blk_pages);
                        break;
                    }
                    /* Legacy handle-table insert (dual mode: kept for compatibility). */
                    handle_id_t ut_h = handle_table_insert(
                        &ut->process->handle_table, &boot_ut->base,
                        RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
                    kobject_release(&boot_ut->base);
                    if (ut_h == HANDLE_INVALID) break;

                    /* Fase 3.4: also publish into root CNode slot for CPtr discovery.
                     * Slot = BOOT_CPTR_UNTYPED_START + drain_index.
                     * kcnode_mint takes its own kobject_retain + kobject_active_retain
                     * giving the CNode slot independent ownership from the handle.
                     * Refcount after: 2 (handle) + 2 (CNode) = two balanced owners. */
                    uint32_t cspace_slot = BOOT_CPTR_UNTYPED_START + ut_count;
                    if (ut->process->cspace_root_h != HANDLE_INVALID &&
                        cspace_slot < KCNODE_DEFAULT_SLOTS) {
                        struct KObject *root_obj = 0;
                        iris_rights_t   root_r;
                        if (handle_table_get_object(&ut->process->handle_table,
                                                    ut->process->cspace_root_h,
                                                    &root_obj, &root_r) == IRIS_OK) {
                            if (root_obj->type == KOBJ_CNODE) {
                                iris_error_t me = kcnode_mint(
                                    (struct KCNode *)root_obj, cspace_slot,
                                    &boot_ut->base,
                                    RIGHT_READ | RIGHT_WRITE |
                                    RIGHT_DUPLICATE | RIGHT_TRANSFER);
                                if (me == IRIS_OK)
                                    ut_cspace_count++;
                            }
                            kobject_release(root_obj);
                        }
                    }

                    ut_count++;
                }
                klog_write("[IRIS][USER] boot untyped blocks handed to init: ");
                klog_write_dec(ut_count);
                klog_write("\n");
                if (ut_cspace_count > 0) {
                    klog_write("[IRIS][USER] boot untyped CSpace grants: ");
                    klog_write_dec(ut_cspace_count);
                    klog_write("\n");
                    klog_write("[IRIS][USER] boot untyped CSpace grants OK\n");
                }
            }
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
