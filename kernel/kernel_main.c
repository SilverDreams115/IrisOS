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
#include <iris/root_bootinfo.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kuntyped.h>
#include <iris/nc/kvspace.h>
#include <iris/nc/kframe.h>
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

    /* Reserve 16 MB (4096 pages) from the PMM as the kernel object slab.
     * All typed kernel object headers (KProcess, KVSpace, root KCNode, page
     * tables, KEndpoint, …) are allocated from this pool via kslab_alloc
     * instead of directly from the PMM, allowing all remaining PMM blocks to be
     * handed to userspace as KUntyped caps.
     *
     * Fase 28.1: grown 4 MB → 16 MB.  Each spawned process consumes several
     * KB–32 KB of kernel objects (KProcess + a 256-slot root KCNode + KVSpace +
     * page-table nodes + handle table); the old 4 MB arena capped concurrent
     * live processes at ~9 (NO_MEMORY on the 10th), which the multi-target
     * pager suite (16 concurrent targets + the pager + the supervisor + core
     * services) exceeds.  This is a kernel-object-MEMORY bound, wholly distinct
     * from the per-process notification quota Fase 28.1 already resolved via the
     * single shared fault notification — growing the arena is the honest fix for
     * a memory ceiling (16 MB is 3% of the 512 MB guest). */
    {
        uint64_t kslab_phys = pmm_alloc_pages(4096u);
        if (kslab_phys == 0) {
            klog_write("[IRIS][KSLAB] FATAL: cannot reserve kernel slab backing\n");
            for (;;) __asm__ volatile ("hlt");
        }
        kslab_init(kslab_phys, 4096u);
        klog_write("[IRIS][KSLAB] kernel object slab active (16 MB)\n");
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
        struct task *ut          = 0;
        uint64_t     bi_phys     = 0;
        void        *bi_kva      = 0;
        uint32_t     bi_capacity = 0;

        /* Stage 5: the root task's BootInfo page.
         *
         * Allocated and initialised BEFORE the task exists, because a root
         * task that cannot be TOLD what it holds must not be created: the
         * alternative is a task that has to rediscover its own CSpace by
         * probing it, which is the convention this stage retires.  Everything
         * after this point that grants a capability also records it here, and
         * the record is what the root task reads out of RBX.
         *
         * The page is bootstrap memory in the same category as the root task's
         * text and stack pages (ledger: "kernel stacks / PML4 from the PMM
         * reserve") — it is mapped as a KFrame and registered as a bootstrap
         * frame, so process teardown releases it exactly like the others. */
        bi_phys = pmm_alloc_pages(IRIS_ROOT_BOOTINFO_PAGES);
        if (bi_phys != 0) {
            bi_kva = (void *)(uintptr_t)PHYS_TO_VIRT(bi_phys);
            for (uint64_t b = 0; b < IRIS_ROOT_BOOTINFO_BYTES; b++)
                ((uint8_t *)bi_kva)[b] = 0;
            if (root_bootinfo_init(bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                                   BOOT_CPTR_VSPACE, BOOT_CPTR_CNODE,
                                   BOOT_CPTR_TCB,
                                   KCNODE_DEFAULT_SLOTS) != IRIS_OK) {
                pmm_free_contig(bi_phys, IRIS_ROOT_BOOTINFO_PAGES);
                bi_phys = 0;
                bi_kva  = 0;
            }
        }
        bi_capacity = bi_kva ? root_bootinfo_capacity(IRIS_ROOT_BOOTINFO_BYTES) : 0u;

        if (!bi_kva) {
            klog_write("[IRIS][USER] FATAL: BootInfo page allocation failed\n");
        } else {
            ut = task_spawn_user(0);
        }
        if (bi_kva && !ut) {
            klog_write("[IRIS][USER] FATAL: task_spawn_user(userboot) failed\n");
        } else if (ut) {
            /*
             * Stage 5 Etapa 2: the boot authorities are published as SIX
             * capabilities, one per authority, each in its own slot.
             *
             * There used to be one object here carrying a permission mask —
             * spawn, hardware, debug and framebuffer at once — so every holder
             * of any of them held all of them, and giving one up meant cloning
             * a narrowed copy of the whole thing (SYS_BOOTCAP_RESTRICT, now
             * retired).  kbootcap_alloc refuses a multi-bit kind, so the
             * monolith is not merely absent from this boot path: it cannot be
             * constructed.  BOOT_CPTR_BOOTSTRAP_CAP (slot 1) stays reserved
             * and permanently empty.
             *
             * Fatal on failure, like every other boot grant: a root task that
             * cannot claim hardware cannot bring up a console, and a
             * half-published boot authority is worse than none.
             */
            static const struct { uint32_t kind; uint32_t slot; }
            boot_controls[] = {
                { IRIS_BOOTCAP_IRQ_CONTROL,    BOOT_CPTR_IRQ_CONTROL },
                { IRIS_BOOTCAP_IOPORT_CONTROL, BOOT_CPTR_IOPORT_CONTROL },
                { IRIS_BOOTCAP_DEBUG_CONTROL,  BOOT_CPTR_DEBUG_CONTROL },
                { IRIS_BOOTCAP_PROC_CONTROL,   BOOT_CPTR_PROC_CONTROL },
                { IRIS_BOOTCAP_INITRD_CONTROL, BOOT_CPTR_INITRD_CONTROL },
                { IRIS_BOOTCAP_FB_CONTROL,     BOOT_CPTR_FB_CONTROL },
            };
            for (uint32_t i = 0;
                 ut && i < sizeof(boot_controls) / sizeof(boot_controls[0]);
                 i++) {
                struct KBootstrapCap *cc = kbootcap_alloc(boot_controls[i].kind);
                iris_error_t cme = IRIS_ERR_NO_MEMORY;
                if (cc) {
                    cme = IRIS_ERR_NOT_FOUND;
                    if (ut->process->cspace_root)
                        cme = kcnode_mint(ut->process->cspace_root,
                                          boot_controls[i].slot, &cc->base,
                                          RIGHT_READ | RIGHT_DUPLICATE |
                                          RIGHT_TRANSFER);
                    kobject_release(&cc->base);
                }
                if (cme == IRIS_OK)
                    cme = root_bootinfo_set_control_cap(
                        bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                        boot_controls[i].kind,
                        (uint64_t)boot_controls[i].slot);
                if (cme != IRIS_OK) {
                    klog_write("[IRIS][USER] FATAL: boot control"
                               " cap publish failed\n");
                    task_abort_spawned_user(ut);
                    ut = 0;
                }
            }
            /*
             * Stage 5 Etapa 3: the root task's OWN objects, as capabilities.
             *
             * Its root CNode was reachable only through the "arg0 == 0 means
             * my own root" convention, and its thread only by asking
             * SYS_TCB_SELF.  Both are real objects that every other holder
             * names with a CPtr, so the task they belong to should not be the
             * one process that cannot name them — seL4's root task finds
             * seL4_CapInitThreadCNode and seL4_CapInitThreadTCB in its CSpace.
             *
             * The root CNode capability lives INSIDE the CNode it names.  That
             * makes the CSpace reachable from itself, which kprocess_teardown
             * handles by emptying the root's slots before dropping its refs
             * (kcnode_teardown_slots) — a cycle cannot be collected by a
             * refcount the cycle is holding up.
             */
            if (ut && ut->process->cspace_root) {
                struct KCNode *root = ut->process->cspace_root;
                iris_error_t ce = kcnode_mint(
                    root, BOOT_CPTR_CNODE, &root->base,
                    RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
                if (ce != IRIS_OK) {
                    klog_write("[IRIS][USER] FATAL: root CNode cap"
                               " publish failed\n");
                    task_abort_spawned_user(ut);
                    ut = 0;
                }
            }
            if (ut) {
                iris_error_t te = kcnode_mint(
                    ut->process->cspace_root, BOOT_CPTR_TCB, &ut->base,
                    RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER);
                if (te != IRIS_OK) {
                    klog_write("[IRIS][USER] FATAL: root TCB cap"
                               " publish failed\n");
                    task_abort_spawned_user(ut);
                    ut = 0;
                }
            }
            if (ut)
                klog_write("[IRIS][USER] boot control caps CSpace grants OK\n");
        }
        if (ut) {
            uint32_t ut_count = 0;   /* boot untypeds granted, == BootInfo entries */

            klog_write("[IRIS][USER] bootstrap task created (ring-3 loader), id=");
            klog_write_dec(ut->id);
            klog_write("\n");

                    /*
                     * Fase 6.2: KVSpace is now created inside task_create_user_impl
                     * (before bootstrap maps so kframe_map_page can register back-refs).
                     * Here we only publish the existing vspace in root CNode slot
                     * BOOT_CPTR_VSPACE (slot 2).
                     *
                     * Ref-count after this block (same as Fase 4):
                     *   process->vspace lifecycle ref   → refcount=1
                     *   kcnode_mint (retain+active)     → refcount=2, active=1
                     *
                     * Boot failure on CSpace insert is non-fatal: KVSpace was
                     * already created and bootstrap maps are registered.
                     */
                    if (ut->process->vspace && ut->process->cspace_root) {
                        iris_error_t vme = kcnode_mint(
                            ut->process->cspace_root,
                            BOOT_CPTR_VSPACE,
                            &ut->process->vspace->base,
                            RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER);
                        if (vme == IRIS_OK)
                            klog_write("[IRIS][USER] boot vspace"
                                       " CSpace grants OK\n");
                    }

            /*
             * Ph76: drain free buddy blocks into KUntyped caps for userboot.
             *
             * IRIS_PMM_KERNEL_RUNTIME_RESERVE pages are kept in the PMM for
             * kernel-internal runtime allocators that bypass the KUntyped model:
             *
             *   • paging_map_checked_in: page tables for the KERNEL address
             *     space and for the root task's pre-Untyped bootstrap maps.
             *     Every other user page table is charged to an Untyped since
             *     Stage 6 Etapa 2 (`paging_map_checked_in_from`).
             *   • kvmo_create: pages[] metadata array (pmm_alloc_pages)
             *   • sys_initrd_vmo: ELF copy pages (pmm_alloc_page × page_capacity)
             *   • kstack_alloc: 2 pages per task kernel stack
             *   • paging_create_user: 1 page per process PML4
             *
             * Fase 6: kernel-side demand paging has been removed.  User pages are
             * now allocated eagerly in sys_vmo_map / sys_vmo_map_into and charged
             * against the process quota at syscall time, not at fault time.  The
             * PMM reserve covers only the kernel-internal allocators listed above.
             *
             * The post-alloc check (after pmm_alloc_block) handles the case
             * where a large block would push pmm_free_pages below the reserve;
             * that block is returned to the buddy and the drain stops.
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
                uint32_t ut_cspace_count = 0;
                for (;;) {
                    if (pmm_free_pages() <= IRIS_PMM_KERNEL_RUNTIME_RESERVE)
                        break;
                    /* Stage 5: the BootInfo page bounds the drain.  A block the
                     * root task cannot be told about is a block it cannot name,
                     * so the grant stops where the description stops rather
                     * than handing over a slot nobody documented. */
                    if (ut_count >= bi_capacity)
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
                    /* Stage 4: the boot untypeds are published into CSpace
                     * ONLY.  They used to be dual-inserted, and the handle
                     * half was never invoked — userboot names slot
                     * BOOT_CPTR_UNTYPED_START and mints from it, and init
                     * receives IRIS_CPTR_INIT_UNTYPED as a pre-start mint.
                     * The handle existed so that a failed CSpace publish could
                     * be "non-fatal"; with one namespace the publish IS the
                     * grant, and a failure stops the drain.
                     *
                     * Order matters: kuntyped_create hands back the only
                     * reference, and kcnode_mint takes its own — so the mint
                     * has to happen BEFORE the release, or the release is the
                     * last one and the object is destroyed under the slot. */
                    uint32_t cspace_slot = BOOT_CPTR_UNTYPED_START + ut_count;
                    iris_error_t me = IRIS_ERR_NOT_FOUND;
                    if (ut->process->cspace_root &&
                        cspace_slot < KCNODE_DEFAULT_SLOTS)
                        me = kcnode_mint(
                            ut->process->cspace_root, cspace_slot,
                            &boot_ut->base,
                            RIGHT_READ | RIGHT_WRITE |
                            RIGHT_DUPLICATE | RIGHT_TRANSFER);
                    kobject_release(&boot_ut->base);
                    if (me != IRIS_OK) break;
                    /* Cannot fail: capacity was checked before the mint, and
                     * the descriptor names the slot that mint just filled. */
                    (void)root_bootinfo_add_untyped(bi_kva,
                                                    IRIS_ROOT_BOOTINFO_BYTES,
                                                    (uint64_t)cspace_slot,
                                                    blk_phys, size,
                                                    /*is_device*/0);
                    ut_cspace_count++;
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

            /*
             * Stage 5: hand the root task its BootInfo.
             *
             * Everything above has finished granting, so the description is
             * now complete and the free range is known: the slots past the
             * last untyped are the ones the kernel did not use.  The page is
             * mapped read-only and non-executable — it is a statement of fact,
             * not a channel — and its address travels in RBX, the register
             * that carried a bootstrap HANDLE until Stage 4 deleted the handle
             * namespace and left it carrying 0.
             *
             * Failure here is FATAL for the same reason the bootstrap cap
             * publish is: the root task's whole job is to distribute authority
             * it can name, and an unmapped or unwritten BootInfo means it
             * would have to go back to guessing.
             */
            {
                iris_error_t bie = root_bootinfo_set_empty_range(
                    bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                    BOOT_CPTR_UNTYPED_START + ut_count, KCNODE_DEFAULT_SLOTS);
                uint32_t mapped = 0;

                for (uint32_t pg = 0; bie == IRIS_OK && ut->process->vspace &&
                                      pg < IRIS_ROOT_BOOTINFO_PAGES; pg++) {
                    uint64_t va = USER_BOOTINFO_BASE + (uint64_t)pg * PMM_PAGE_SIZE;
                    struct KFrame *bif = bootstrap_kframe_map(
                        ut->process->vspace, bi_phys + (uint64_t)pg * PMM_PAGE_SIZE,
                        va, 0ULL /* read-only, NX */);
                    if (!bif) break;
                    if (kprocess_register_bootstrap_frame(ut->process, bif) != IRIS_OK) {
                        kframe_unmap_page(bif, ut->process->vspace, va);
                        kobject_release(&bif->base);
                        break;
                    }
                    mapped++;
                }
                if (mapped != IRIS_ROOT_BOOTINFO_PAGES) {
                    klog_write("[IRIS][USER] FATAL: BootInfo map failed\n");
                    task_abort_spawned_user(ut);
                    ut = 0;
                } else {
                    task_set_bootstrap_arg0(ut, USER_BOOTINFO_BASE);
                    klog_write("[IRIS][USER] boot info page mapped, untypeds: ");
                    klog_write_dec(ut_count);
                    klog_write("\n");
                }
            }
        }
        if (!ut) {
            klog_write("[IRIS][USER] WARN: could not create bootstrap task\n");
            /* Every abort above leaves the root task un-created, so nothing
             * owns the BootInfo pages: the frames that would have carried them
             * into its address space either were never built or went back with
             * task_abort_spawned_user.  Returning them here keeps the one
             * allocation in this block that has an owner-on-success and no
             * owner-on-failure from being the one that leaks. */
            if (bi_phys) pmm_free_contig(bi_phys, IRIS_ROOT_BOOTINFO_PAGES);
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
