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
#include <iris/acpi.h>
#include <iris/iommu.h>
#include <iris/smp.h>
#include <iris/tlb.h>
#include <iris/nc/kbootcap.h>
#include <iris/root_bootinfo.h>
#include <iris/abi.h>
#include <iris/nc/kfault.h>
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
     * Phase 28.1: grown 4 MB → 16 MB.  Each spawned process consumes several
     * KB–32 KB of kernel objects (KProcess + a 256-slot root KCNode + KVSpace +
     * page-table nodes + handle table); the old 4 MB arena capped concurrent
     * live processes at ~9 (NO_MEMORY on the 10th), which the multi-target
     * pager suite (16 concurrent targets + the pager + the supervisor + core
     * services) exceeds.  This is a kernel-object-MEMORY bound, wholly distinct
     * from the per-process notification quota Phase 28.1 already resolved via the
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
    pit_init(IRIS_TICK_HZ);   /* ABI fact, shared with the timer service */

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

    /*
     * How many CPUs does this machine have?  (SMP roadmap §9.3 step 3.)
     *
     * Asked here, once, and answered from the MADT — the only description of
     * the machine's processors, and reachable only through an RSDP the
     * firmware handed over before ExitBootServices.  Nothing is STARTED by
     * this: the count and the LAPIC ids are recorded, and bringing an
     * application processor up is the next step.  A machine that answers
     * "one", or does not answer, is a machine IRIS runs on exactly as it did
     * before this call existed.
     */
    (void)acpi_parse_madt(saved_boot_info.acpi_rsdp);

    /*
     * And where the DMA remapping units are (Stage 10-dma §10.2 step 1).
     *
     * Read here because it comes off the same RSDP, from the same table set,
     * which stops existing at ExitBootServices.  Then each unit's capability
     * registers are read (step 2), which decides whether it is what the later
     * steps assume — a translation built on a guess about the hardware is
     * worse than no translation, because it looks like protection.
     *
     * Still nothing mapped and nothing ENABLED: that is step 3.
     *
     * A machine with no DMAR says so and runs exactly as it did before — with
     * every DMA-capable device able to reach all of memory, which is the state
     * this stage exists to end.
     */
    if (iommu_parse_dmar(saved_boot_info.acpi_rsdp))
        (void)iommu_probe_units();

    /* Cross-CPU TLB invalidation (SMP roadmap §9.3 step 2).  Its lock is the
     * only state; the mechanism itself does nothing until a second CPU can be
     * inside an address space somebody else is unmapping from. */
    tlb_init();

#ifdef IRIS_ENABLE_RUNTIME_SELFTESTS
    phase3_selftest_run();
#endif

    /* ── 7. Scheduler core ──────────────────────────────────────── */
    klog_write("[IRIS][SCHED] initializing...\n");
    scheduler_init();
    /* Stage 9-evt step 3: publish this core's kernel stack before anything can
     * park — the dispatcher reads it GS-relative and has no fallback. */
    core_dispatch_init();

    /*
     * Start the application processors (SMP roadmap §9.3 step 3).
     *
     * AFTER `core_dispatch_init`, because that is what fills in the per-core
     * kernel stacks an arriving processor lands on, and after `idt_init` and
     * the LAPIC, because an AP that takes a fault before there is an IDT
     * triple-faults with nothing to say about why.  BEFORE the first user
     * task, because every processor should be up before anything schedules —
     * a CPU arriving mid-run would be a second thing to reason about.
     *
     * They park.  Nothing here makes them schedule; that is step 4.
     */
    (void)smp_start_aps(&saved_boot_info);

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
             * Stage 5 Step 2: the boot authorities are published as SIX
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
                { IRIS_BOOTCAP_SCHED_CONTROL,  BOOT_CPTR_SCHED_CONTROL },
                { IRIS_BOOTCAP_ASID_CONTROL,   BOOT_CPTR_ASID_CONTROL },
                { IRIS_BOOTCAP_DOMAIN_CONTROL, BOOT_CPTR_DOMAIN_CONTROL },
                { IRIS_BOOTCAP_IOSPACE_CONTROL, BOOT_CPTR_IOSPACE_CONTROL },
            };
            for (uint32_t i = 0;
                 ut && i < sizeof(boot_controls) / sizeof(boot_controls[0]);
                 i++) {
                struct KBootstrapCap *cc = kbootcap_alloc(boot_controls[i].kind);
                iris_error_t cme = IRIS_ERR_NO_MEMORY;
                if (cc) {
                    cme = IRIS_ERR_NOT_FOUND;
                    if (ut->cspace_root)
                        cme = kcnode_mint(ut->cspace_root,
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
             * Stage 5 Step 3: the root task's OWN objects, as capabilities.
             *
             * Its root CNode was reachable only through the "arg0 == 0 means
             * my own root" convention, and its thread only by asking
             * SYS_TCB_SELF.  Both are real objects that every other holder
             * names with a CPtr, so the task they belong to should not be the
             * one process that cannot name them — seL4's root task finds
             * seL4_CapInitThreadCNode and seL4_CapInitThreadTCB in its CSpace.
             *
             * The root CNode capability lives INSIDE the CNode it names.  That
             * makes the CSpace reachable from itself, which thread teardown
             * handles by emptying the root's slots before dropping its refs
             * (kcnode_teardown_slots) — a cycle cannot be collected by a
             * refcount the cycle is holding up.
             */
            if (ut && ut->cspace_root) {
                struct KCNode *root = ut->cspace_root;
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
                    ut->cspace_root, BOOT_CPTR_TCB, &ut->base,
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
            struct KUntyped *root_budget = 0;  /* the root task's own, Step 3 */

            klog_write("[IRIS][USER] bootstrap task created (ring-3 loader), id=");
            klog_write_dec(ut->id);
            klog_write("\n");

                    /*
                     * Phase 6.2: KVSpace is now created inside task_create_user_impl
                     * (before bootstrap maps so kframe_map_page can register back-refs).
                     * Here we only publish the existing vspace in root CNode slot
                     * BOOT_CPTR_VSPACE (slot 2).
                     *
                     * Ref-count after this block (same as Phase 4):
                     *   process->vspace lifecycle ref   → refcount=1
                     *   kcnode_mint (retain+active)     → refcount=2, active=1
                     *
                     * Boot failure on CSpace insert is non-fatal: KVSpace was
                     * already created and bootstrap maps are registered.
                     */
                    if (ut->vspace && ut->cspace_root) {
                        /*
                         * RIGHT_WRITE, like the root CNode and TCB above.
                         *
                         * It was READ|DUPLICATE|TRANSFER, and a VSpace without
                         * WRITE cannot be mapped into — so the root task could
                         * not use the address-space capability its own BootInfo
                         * handed it, and reached its address space through
                         * SYS_VSPACE_SELF instead.  An ambient syscall was
                         * covering for an under-powered BootInfo slot, which is
                         * why retiring the syscall is what exposed it.  seL4's
                         * seL4_CapInitThreadVSpace is a full capability.
                         */
                        iris_error_t vme = kcnode_mint(
                            ut->cspace_root,
                            BOOT_CPTR_VSPACE,
                            &ut->vspace->base,
                            RIGHT_READ | RIGHT_WRITE |
                            RIGHT_DUPLICATE | RIGHT_TRANSFER);
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
             *     Stage 6 Step 2 (`paging_map_checked_in_from`).
             *   • paging_create_user: 1 page per process PML4
             *
             * Ledger D-5 took three entries off this list — kvmo_create's page
             * array, sys_initrd_vmo's ELF copy pages, and (with D-1) the
             * per-task kernel stacks.  None of them was a kernel-internal
             * allocation that a holder could not have made itself, which is
             * what the reserve is for.
             *
             * Kernel-side demand paging is gone: user pages come from an
             * Untyped a holder names, at the moment it says so.
             *
             * The post-alloc check (after pmm_alloc_block) handles the case
             * where a large block would push pmm_free_pages below the reserve;
             * that block is returned to the buddy and the drain stops.
             *
             * Phase 3.4 (dual mode): every boot KUntyped is also inserted into
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
                    /* Stage 6-pure Step 3: the FIRST block is the one the
                     * root task's own address space will draw on once the
                     * bootstrap exception ends.  Keep a reference now, while
                     * the object is certainly alive. */
                    if (ut_count == 0u) {
                        kobject_retain(&boot_ut->base);
                        root_budget = boot_ut;
                    }
                    iris_error_t me = IRIS_ERR_NOT_FOUND;
                    if (ut->cspace_root &&
                        cspace_slot < KCNODE_DEFAULT_SLOTS)
                        me = kcnode_mint(
                            ut->cspace_root, cspace_slot,
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
                                                    /*is_device*/0,
                                                    IRIS_UT_KIND_RAM);
                    ut_cspace_count++;
                    ut_count++;
                }

                /*
                 * Ledger D-9 — the framebuffer, as a DEVICE Untyped.
                 *
                 * seL4's BootInfo lists device Untypeds alongside RAM ones;
                 * that is how a driver is handed an MMIO region as a
                 * capability and retypes frames from it.  IRIS described the
                 * shape — `iris_bootinfo_untyped.is_device` has been in the
                 * ABI since v1 — and always wrote 0, so no device Untyped
                 * could exist and the invariants about them (U11/U12)
                 * described an object the system could not construct.  The
                 * framebuffer is the one MMIO region the kernel already knows
                 * the bounds of, so it is the first.
                 *
                 * It is published like any other: minted into a slot and
                 * described in BootInfo.  What it is NOT is usable on its own
                 * — a device region cannot hold the headers of objects carved
                 * from it, so the holder must pair it with a RAM Untyped
                 * (`SYS_UNTYPED_SET_DEVICE_BUDGET`) before the first retype.
                 * Unpaired, the retype refuses rather than quietly spending
                 * the kernel's memory.
                 */
                if (bi_kva && ut->cspace_root && ut_count < bi_capacity &&
                    g_iris_fb_params.phys != 0 && g_iris_fb_params.size != 0) {
                    uint32_t fb_slot = BOOT_CPTR_UNTYPED_START + ut_count;
                    if (fb_slot < KCNODE_DEFAULT_SLOTS) {
                        uint64_t fb_phys = g_iris_fb_params.phys & ~0xFFFULL;
                        uint64_t fb_size = (g_iris_fb_params.size + 0xFFFu)
                                           & ~0xFFFULL;
                        struct KUntyped *fb_ut =
                            kuntyped_create(fb_phys, fb_size, /*is_device*/1);
                        if (fb_ut) {
                            iris_error_t fe = kcnode_mint(
                                ut->cspace_root, fb_slot, &fb_ut->base,
                                RIGHT_READ | RIGHT_WRITE |
                                RIGHT_DUPLICATE | RIGHT_TRANSFER);
                            kobject_release(&fb_ut->base);
                            if (fe == IRIS_OK) {
                                (void)root_bootinfo_add_untyped(
                                    bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                                    (uint64_t)fb_slot, fb_phys, fb_size,
                                    /*is_device*/1, IRIS_UT_KIND_FRAMEBUFFER);
                                ut_cspace_count++;
                                ut_count++;
                                klog_write("[IRIS][USER] framebuffer published "
                                           "as a device untyped\n");
                            }
                        }
                    }
                }
                /*
                 * ...and the firmware's own memory, as DEVICE Untypeds.
                 *
                 * ACPI describes the machine: which processors exist, where
                 * the remapping units are, what the interrupt routing is.  The
                 * kernel reads three of those tables and will never read a
                 * fourth — deciding what a machine IS belongs in ring 3, and a
                 * kernel that grew an AML interpreter would be the largest
                 * policy in the system.
                 *
                 * But ring 3 could not read them either, and that is the gap
                 * this closes.  ACPI tables sit in memory the firmware marked
                 * RECLAIMABLE or NVS, which is neither usable RAM (so it is in
                 * no RAM Untyped) nor unmapped address space (so it is not in
                 * the PCI hole).  There was no capability in the system that
                 * named it, so there was no way to reach it that did not go
                 * through the kernel.
                 *
                 * Published as DEVICE Untypeds for the reason the framebuffer
                 * is: the kernel must not put object headers in firmware
                 * memory.  The RSDP's address rides in BootInfo beside them,
                 * because a region is not a starting point — a reader needs to
                 * know where the pointer that anchors the whole set lives, and
                 * only the bootloader ever knew.
                 *
                 * NVS as well as RECLAIMABLE, and deliberately: "reclaimable"
                 * means an OS may take it back once it has read the tables,
                 * and this one does not, because the only reader is in ring 3
                 * and the kernel does not know when it has finished.
                 */
                if (bi_kva)
                    (void)root_bootinfo_set_acpi_rsdp(
                        bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                        saved_boot_info.acpi_rsdp);

                for (uint64_t i = 0;
                     bi_kva && ut->cspace_root && ut_count < bi_capacity &&
                     i < saved_boot_info.mmap_entry_count; i++) {
                    const struct iris_mmap_entry *e = &saved_boot_info.mmap[i];
                    if (e->type != IRIS_MEM_ACPI_RECLAIMABLE &&
                        e->type != IRIS_MEM_ACPI_NVS) continue;
                    uint64_t ab = e->base & ~0xFFFULL;
                    uint64_t ae = (e->base + e->length + 0xFFFu) & ~0xFFFULL;
                    if (ae <= ab) continue;

                    uint32_t aslot = BOOT_CPTR_UNTYPED_START + ut_count;
                    if (aslot >= KCNODE_DEFAULT_SLOTS) break;
                    struct KUntyped *a_ut =
                        kuntyped_create(ab, ae - ab, /*is_device*/1);
                    if (!a_ut) continue;
                    iris_error_t ae2 = kcnode_mint(
                        ut->cspace_root, aslot, &a_ut->base,
                        RIGHT_READ | RIGHT_WRITE |
                        RIGHT_DUPLICATE | RIGHT_TRANSFER);
                    kobject_release(&a_ut->base);
                    if (ae2 != IRIS_OK) continue;
                    (void)root_bootinfo_add_untyped(
                        bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                        (uint64_t)aslot, ab, ae - ab, /*is_device*/1,
                        IRIS_UT_KIND_ACPI);
                    ut_cspace_count++;
                    ut_count++;
                    klog_write("[IRIS][USER] ACPI 0x");
                    klog_write_hex(ab);
                    klog_write("..0x");
                    klog_write_hex(ae);
                    klog_write(" published as a device untyped\n");
                }

                /*
                 * ...and the rest of the MMIO space, as a DEVICE Untyped.
                 *
                 * The framebuffer above was the first, and it was the only one
                 * the kernel knew the bounds of.  Without a second, a driver
                 * for any OTHER device cannot reach its registers at all — a
                 * PCI device is programmed through its BARs, and a BAR is an
                 * MMIO address.  So "user-space drivers" stopped at the one
                 * device whose region was hard-coded, and Stage 10-dma's claim
                 * that a device's DMA is contained could never be DEMONSTRATED,
                 * because nothing under IRIS's control could issue any.
                 *
                 * seL4 publishes device Untypeds for everything that is not
                 * usable RAM, from the same memory map.  UEFI's map does not
                 * describe the PCI hole at all — it is unmapped address space,
                 * not memory — so the bounds are computed from what IS known:
                 *
                 *   start: above every byte of RAM and above the framebuffer
                 *          region already published, so no two Untypeds ever
                 *          name the same physical page;
                 *   end:   below the lowest address the KERNEL's own devices
                 *          occupy.  This is the part that matters.  Handing
                 *          ring 3 a region containing the IOMMU's registers
                 *          would let a driver switch off the very thing that
                 *          contains it, and the LAPIC and IOAPIC are no better.
                 *          Every one of them sits at or above 0xFEC00000 on
                 *          x86, and the units' own bases are checked as well
                 *          rather than assumed.
                 *
                 * A machine where that leaves nothing publishes nothing.
                 *
                 * What this region does NOT promise is that every page in it
                 * is unclaimed.  It is bounded by RAM and by the kernel's own
                 * devices, and the kernel enumerates no PCI — so a window the
                 * firmware assigned to some device's BAR is inside it, which
                 * is the whole point: that is how a ring-3 driver gets a frame
                 * over its own registers (Stage 10-dma §10.2 step 6).  It also
                 * means the holder is the one that has to know what is there.
                 * The VGA aperture is the live example: its BAR decodes 16 MiB
                 * from the framebuffer's base, while the framebuffer Untyped
                 * covers only the visible part, so the first few megabytes of
                 * this region are video memory nobody is using.  seL4's device
                 * Untypeds carry exactly the same caveat — a device Untyped is
                 * a physical range, not a claim that the range is free — and
                 * the answer is the same: a driver retypes the window it found
                 * by reading a BAR, not the first frame the region offers.
                 */
                if (bi_kva && ut->cspace_root && ut_count < bi_capacity) {
                    uint64_t ram_top = 0;
                    for (uint64_t i = 0; i < saved_boot_info.mmap_entry_count; i++) {
                        const struct iris_mmap_entry *e = &saved_boot_info.mmap[i];
                        uint64_t end = e->base + e->length;
                        if (e->type == IRIS_MEM_USABLE && end > ram_top) ram_top = end;
                    }
                    uint64_t fb_end = g_iris_fb_params.phys + g_iris_fb_params.size;
                    uint64_t start  = ram_top > fb_end ? ram_top : fb_end;
                    start = (start + 0x1FFFFFull) & ~0x1FFFFFull;   /* 2 MiB up */

                    uint64_t end = 0xFEC00000ull;    /* IOAPIC, and everything above */
                    uint64_t lapic = acpi_lapic_base();
                    if (lapic && lapic < end) end = lapic;
                    for (uint32_t u = 0; u < iommu_unit_count(); u++) {
                        const struct iris_iommu_unit *iu = iommu_unit(u);
                        if (iu && iu->reg_base && iu->reg_base < end) end = iu->reg_base;
                    }
                    end &= ~0x1FFFFFull;                            /* 2 MiB down */

                    uint32_t mm_slot = BOOT_CPTR_UNTYPED_START + ut_count;
                    if (start < end && mm_slot < KCNODE_DEFAULT_SLOTS) {
                        struct KUntyped *mm_ut =
                            kuntyped_create(start, end - start, /*is_device*/1);
                        if (mm_ut) {
                            iris_error_t me = kcnode_mint(
                                ut->cspace_root, mm_slot, &mm_ut->base,
                                RIGHT_READ | RIGHT_WRITE |
                                RIGHT_DUPLICATE | RIGHT_TRANSFER);
                            kobject_release(&mm_ut->base);
                            if (me == IRIS_OK) {
                                (void)root_bootinfo_add_untyped(
                                    bi_kva, IRIS_ROOT_BOOTINFO_BYTES,
                                    (uint64_t)mm_slot, start, end - start,
                                    /*is_device*/1, IRIS_UT_KIND_MMIO);
                                ut_cspace_count++;
                                ut_count++;
                                klog_write("[IRIS][USER] MMIO 0x");
                                klog_write_hex(start);
                                klog_write("..0x");
                                klog_write_hex(end);
                                klog_write(" published as a device untyped\n");
                            }
                        }
                    }
                }

                /* Stage 10-abi: which ABI this kernel implements, said out
                 * loud.  It is in BootInfo for the root task to ACT on; it is
                 * here so that a log from a machine somebody else ran answers
                 * the first question anybody asks about it. */
                klog_write("[IRIS][ABI] version ");
                klog_write_dec(IRIS_ABI_VERSION_MAJOR);
                klog_write(".");
                klog_write_dec(IRIS_ABI_VERSION_MINOR);
                klog_write(" - 4 syscall numbers, ");
                klog_write_dec(IRIS_ABI_LABEL_MAX + 1u);
                klog_write(" invocation labels\n");

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

                for (uint32_t pg = 0; bie == IRIS_OK && ut->vspace &&
                                      pg < IRIS_ROOT_BOOTINFO_PAGES; pg++) {
                    uint64_t va = USER_BOOTINFO_BASE + (uint64_t)pg * PMM_PAGE_SIZE;
                    struct KFrame *bif = bootstrap_kframe_map(
                        ut->vspace, bi_phys + (uint64_t)pg * PMM_PAGE_SIZE,
                        va, 0ULL /* read-only, NX */);
                    if (!bif) break;
                    if (kvspace_register_bootstrap_frame(ut->vspace, bif) != IRIS_OK) {
                        kframe_unmap_page(bif, ut->vspace, va);
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
                    /*
                     * Stage 6-pure Step 3 — the bootstrap exception ends here.
                     *
                     * Everything the kernel had to map on the root task's
                     * behalf is mapped: its text, its stack, and the page that
                     * tells it what it holds.  From this line on it can speak
                     * for itself — it holds boot untypeds and a capability to
                     * its own VSpace — so it supplies its own paging levels
                     * like every other address space, and its mapping records
                     * come out of its own budget rather than a kernel arena.
                     *
                     * After this, no address space in IRIS is implicitly funded
                     * while anybody is running.
                     */
                    if (ut->vspace) {
                        if (root_budget)
                            kvspace_set_pt_pool(ut->vspace, root_budget);
                    }
                    klog_write("[IRIS][USER] boot info page mapped, untypeds: ");
                    klog_write_dec(ut_count);
                    klog_write("\n");
                }
            }
            /* Attached or not, this reference was ours; the VSpace took its
             * own if it wanted one. */
            if (root_budget) kobject_release(&root_budget->base);
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

    /*
     * ── 8b. Contain DMA (Stage 10-dma §10.2 step 3) ─────────────────
     *
     * LAST, and after the first user task is built, deliberately.  From this
     * call on, every DMA request from every device is refused by the hardware
     * unless something maps a frame for it — and nothing can yet, so the
     * answer is "refused" for all of them.
     *
     * Last because the firmware's devices do DMA and this is what stops DMA
     * nobody authorised.  IRIS does none of its own after ExitBootServices —
     * the services are linked into the kernel image, so there is no disk read
     * left to break — but "there is nothing left that needs it" is a claim
     * about the boot sequence, so it is made where the boot sequence is
     * finished rather than in the middle of it.
     */
    (void)iommu_enable_blocking();

    /* ── 9. Scheduler start ─────────────────────────────────────── */
    klog_write("[IRIS][SCHED] running\n");
    klog_write("[IRIS][BOOT] waiting for first userland wave\n");
    klog_write("====================================\n");
    __asm__ volatile ("sti");

    /*
     * Stage 9-evt step 3 — boot ends by entering the DISPATCHER, and does not
     * come back.
     *
     * This used to be the idle loop: the boot thread yielding for ever, which
     * is why IRIS had an idle TASK at all, and why a per-thread kernel stack
     * could not go — idle was a thread with a stack like any other, and every
     * "nobody else can run" answer had to be a switch to it.
     *
     * The dispatcher's answer is a `hlt` on the core's own stack.  The boot
     * thread stops being current the moment anything else is picked, is never
     * enqueued (it is `sched_idle_thread`, which the run queue excludes), and its
     * stack is never used again.
     */
    /*
     * Boot is over: seal the kernel heap.  From here the kernel allocates
     * nothing — every object a running system creates comes out of an Untyped
     * somebody named, and the purity gate proves no syscall handler can even
     * reach the arena.  Sealing turns that from a property to check into one
     * the build enforces.
     */
    kslab_seal();

    core_dispatch_enter(task_current());
}
