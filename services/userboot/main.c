#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/endpoint_proto.h>
#include <iris/boot_info.h>
#include <iris/root_bootinfo.h>
#include "../../services/common/svc_loader.h"

static inline long ub_sys0(long nr) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(0L), "S"(0L), "d"(0L)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(0L), "d"(0L)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(0L)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

/* Stage 4: nothing userboot holds is a handle.  ub_close was the release
 * path for the kernel's dual-inserted bootstrap cap, which is CSpace-only
 * now. */
static void ub_close(handle_id_t h) { (void)h; }

/* Fase 28: bootstrap diagnostic.  A bootstrap-fatal condition (a broken initrd
 * catalog) must never manifest as a SILENT dead system.  userboot holds the
 * root KBootstrapCap (HW_ACCESS), so it can mint a serial KIoPort and emit a
 * diagnostic line directly to COM1 before exiting — visible even though no
 * console/svcmgr service has come up yet.  Crude (no LSR polling), but a boot
 * that reaches this path is already fatal. */
/* Fase S4: the KIoPort is published into a CSpace slot as an MDB child of the
 * bootstrap-cap slot; the authority argument must be a CPtr.
 *
 * Stage 5: the destination slot is an ARGUMENT, taken from the free range the
 * BootInfo declares.  UB_PANIC_IOPORT_SLOT survives only as the last-resort
 * slot for the one panic that fires when the BootInfo itself is unreadable —
 * there is nothing to consult then, and a diagnostic that guesses wrong is
 * silent rather than wrong. */
#define UB_PANIC_IOPORT_SLOT 40u
static void ub_boot_panic(uint64_t bootstrap_cptr, uint64_t ioport_slot,
                          const char *msg) {
    long r = ub_sys4(SYS_CAP_CREATE_IOPORT, (long)bootstrap_cptr,
                     0x3F8, 8, (long)ioport_slot);
    if (r == 0) {
        long io = (long)ioport_slot;
        for (const char *p = msg; *p; p++) {
            if (*p == '\n') (void)ub_sys3(SYS_IOPORT_OUT, io, 0, (long)'\r');
            (void)ub_sys3(SYS_IOPORT_OUT, io, 0, (long)(uint8_t)*p);
        }
    }
}

/* ub_msg_zero retired — Fase 13/Track I (no KChannel bootstrap message). */

/* ub_send_spawn_cap retired — Fase 13/Track I (init's spawn cap is a pre-start
 * IRIS_CPTR_SPAWN_CAP mint now, no KChannel SPAWN_CAP send). */

static void ub_park_root_bootstrap(void) {
    /* Root bootstrap task policy:
     * after delegating authority it keeps no live handles and remains parked.
     * That makes the lifecycle explicit without putting first-task teardown
     * back on the critical healthy-path IPC boundary. */
    for (;;) (void)ub_sys1(SYS_SLEEP, 60000);
}

/* Stage 5: validate one BootInfo untyped descriptor against the capability it
 * claims to describe.  SYS_UNTYPED_INFO answers from the slot itself, so a
 * mismatch means the page and the CSpace disagree — which is exactly the class
 * of bug a written-down layout is supposed to make impossible, and the reason
 * this check exists at all rather than trusting the page. */
static int ub_untyped_matches(const struct iris_bootinfo_untyped *e) {
    uint64_t phys = 0, avail = 0;

    if (ub_sys3(SYS_UNTYPED_INFO, (long)e->cptr, (long)(uintptr_t)&phys,
                (long)(uintptr_t)&avail) != 0)
        return 0;
    if (phys != e->paddr)      return 0;
    /* Fresh boot untypeds are untouched, so everything is still available;
     * more than the region holds would mean the descriptor understates it. */
    if (avail > e->size_bytes) return 0;
    return 1;
}

void iris_userboot_main(uint64_t bootinfo_va) {
    /* Stage 5: RBX carries the address of the BootInfo page.
     *
     * It carried a bootstrap HANDLE until Stage 4 deleted that namespace, then
     * 0 until this page existed.  What arrives now is not authority — the page
     * is mapped read-only and every capability it names is already installed
     * in this task's CSpace — it is the answer to "what did the kernel give
     * me", which userboot previously had to guess from constants shared by
     * compile-time agreement (BOOT_CPTR_BOOTSTRAP_CAP, BOOT_CPTR_UNTYPED_START)
     * and discover by probing slots until one answered NOT_FOUND.
     *
     * A page that does not validate is fatal: a root task that cannot read its
     * own inventory has nothing to distribute, and guessing is the behaviour
     * being retired. */
    const struct iris_root_bootinfo *bi =
        (const struct iris_root_bootinfo *)(uintptr_t)bootinfo_va;

    handle_id_t init_proc_h = HANDLE_INVALID;
    handle_id_t init_boot_h = HANDLE_INVALID;
    uint64_t    bootstrap_cap_h;
    uint64_t    boot_untyped_c;
    uint64_t    ws_slot;      /* loader workspace CNode — first free slot */
    uint64_t    panic_slot;   /* serial KIoPort for a boot diagnostic — last */

    if (!bi || bi->magic != IRIS_ROOT_BOOTINFO_MAGIC ||
        bi->version != IRIS_ROOT_BOOTINFO_VERSION ||
        bi->header_bytes < sizeof(*bi)) {
        /* No trustworthy inventory, so the diagnostic uses the well-known slot
         * the kernel publishes the bootstrap cap into.  That is a guess, and
         * it is confined to printing why the boot is dead: if the guess is
         * wrong the KIoPort mint fails and the panic is silent, exactly as it
         * was before this page existed. */
        ub_boot_panic(BOOT_CPTR_BOOTSTRAP_CAP, UB_PANIC_IOPORT_SLOT,
                      "[USERBOOT] FATAL: BootInfo missing or unreadable; "
                      "halting boot\n");
        goto fail;
    }

    bootstrap_cap_h = bi->cap_bootstrap;
    if (bootstrap_cap_h == 0u) {
        ub_boot_panic(BOOT_CPTR_BOOTSTRAP_CAP, UB_PANIC_IOPORT_SLOT,
                      "[USERBOOT] FATAL: BootInfo grants no bootstrap "
                      "capability; halting boot\n");
        goto fail;
    }

    /* Stage 5: the slots userboot writes into come from the free range the
     * kernel declared, not from constants chosen by reading the boot code.
     * "Slot 40 is free" and "slot 3 is free in the reserved range" were true
     * only for as long as nobody moved anything, and a collision here does not
     * announce itself: a mint into an occupied slot fails, or worse, succeeds
     * over a capability that was in use.  Two slots are needed — the loader
     * workspace and the diagnostic KIoPort — and they are taken from opposite
     * ends so they cannot be the same slot. */
    if (bi->empty_slot_end < bi->empty_slot_first + 2u) {
        ub_boot_panic(bootstrap_cap_h, UB_PANIC_IOPORT_SLOT,
                      "[USERBOOT] FATAL: BootInfo leaves no free slots to "
                      "work in; halting boot\n");
        goto fail;
    }
    ws_slot    = bi->empty_slot_first;
    panic_slot = (uint64_t)bi->empty_slot_end - 1u;

    /* The untypeds are the boot memory budget: init gets one, and everything
     * any service ever retypes descends from them.  Zero of them is a dead
     * system with a confusing failure mode three services later. */
    if (bi->untyped_count == 0u) {
        ub_boot_panic(bootstrap_cap_h, panic_slot,
                      "[USERBOOT] FATAL: BootInfo describes no untyped "
                      "memory; halting boot\n");
        goto fail;
    }
    for (uint32_t i = 0; i < bi->untyped_count; i++) {
        if (!ub_untyped_matches(&bi->untyped[i])) {
            ub_boot_panic(bootstrap_cap_h, panic_slot,
                          "[USERBOOT] FATAL: BootInfo disagrees with the "
                          "CSpace it describes; halting boot\n");
            goto fail;
        }
    }
    boot_untyped_c = bi->untyped[0].cptr;

    /* Fase 28 boot-growth fix: the boot invariant is that the kernel initrd has
     * AT LEAST every image the ring-3 name→index catalog references (indices
     * 0..SL_CATALOG_COUNT-1 must resolve).  The initrd is allowed to hold MORE
     * images at higher indices (new services, backing blobs) — those are not
     * named here and are loaded by other means.  The old exact-equality check
     * turned any legitimate initrd growth into a silent dead boot (userboot
     * exited before loading init); it is now a >= check, and a genuine shortage
     * emits a bootstrap diagnostic instead of vanishing. */
    if (svc_initrd_count((handle_id_t)bootstrap_cap_h) < (long)SL_CATALOG_COUNT) {
        ub_boot_panic(bootstrap_cap_h, panic_slot,
                      "[USERBOOT] FATAL: initrd catalog too small "
                      "(kernel/ring-3 mismatch); halting boot\n");
        goto fail;
    }

    /* Stage 5: the Fase 3.4 liveness probe of BOOT_CPTR_UNTYPED_START is
     * RETIRED.  It invoked a slot named by a compile-time constant, ignored
     * the answer, and documented itself as something boot was not gated on —
     * a probe that cannot fail proves nothing.  The BootInfo validation above
     * replaces it with a check that has a verdict: every untyped the kernel
     * says it granted must answer from its slot with the physical region the
     * page claims, or the boot stops here with a diagnostic. */

    /* Fase 13 (Track I): deliver init's spawn/bootstrap cap as the
     * IRIS_CPTR_SPAWN_CAP (slot 6) pre-start mint instead of a post-spawn
     * KChannel SPAWN_CAP send — no SYS_CHAN.
     *
     * Stage 4: both mint sources are CPtrs into our own root CSpace, not
     * handles.  Beyond retiring the SYS_HANDLE_DUP / SYS_CSPACE_RESOLVE pair,
     * this buys real authority: SYS_CSPACE_MINT_INTO installs each cap as an
     * MDB CHILD of our slot, so init's founding capabilities are revocable by
     * userboot (and by the kernel bootstrap slot above it) instead of being
     * handed over forever. */
    {
        /* Fase 18: forward ONE boot KUntyped into init so it can be handed on
         * to iris_test for the ring-3 authority suite (T125–T131).  Full rights
         * so retype (WRITE) and onward mint (DUPLICATE) both work.  Non-fatal:
         * if the grant is absent the mint fails, the slot stays empty and the
         * authority tests FAIL loudly rather than silently skipping. */
        struct svc_mint init_mints[2] = { 0 };
        init_mints[0].slot     = IRIS_CPTR_SPAWN_CAP;
        init_mints[0].src_cptr = bootstrap_cap_h;
        init_mints[0].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        init_mints[0].badge    = 0;
        init_mints[1].slot     = IRIS_CPTR_INIT_UNTYPED;
        init_mints[1].src_cptr = boot_untyped_c;
        init_mints[1].rights   = RIGHT_READ | RIGHT_WRITE |
                                 RIGHT_DUPLICATE | RIGHT_TRANSFER;
        init_mints[1].badge    = 0;

        /* Both sources are the CPtrs the BootInfo named, not the constants
         * that happened to match them: userboot now delegates what the kernel
         * says it holds, and carves the loader workspace out of the same first
         * boot untyped into a slot the kernel declared free. */
        long lr = svc_load_minted_ws((handle_id_t)bootstrap_cap_h, "init",
                                     &init_proc_h, &init_boot_h, init_mints, 2u,
                                     SVC_LOADER_WS(boot_untyped_c, ws_slot));
        if (lr < 0)
            goto fail;
    }

    ub_close(init_boot_h);
    ub_close(init_proc_h);
    ub_park_root_bootstrap();

fail:
    ub_close(init_boot_h);
    ub_close(init_proc_h);
    (void)ub_sys1(SYS_EXIT, 1);
    for (;;) {}
}
