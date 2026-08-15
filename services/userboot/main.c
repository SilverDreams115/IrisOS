#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/svcmgr_proto.h>
#include <iris/endpoint_proto.h>
#include <iris/boot_info.h>
#include "../../services/common/svc_loader.h"

static inline long ub_sys0(long nr) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys1(long nr, long a0) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long ub_sys2(long nr, long a0, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
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

static void ub_close(handle_id_t h) {
    if (h != HANDLE_INVALID)
        (void)ub_sys1(SYS_HANDLE_CLOSE, (long)h);
}

/* Fase 28: bootstrap diagnostic.  A bootstrap-fatal condition (a broken initrd
 * catalog) must never manifest as a SILENT dead system.  userboot holds the
 * root KBootstrapCap (HW_ACCESS), so it can mint a serial KIoPort and emit a
 * diagnostic line directly to COM1 before exiting — visible even though no
 * console/svcmgr service has come up yet.  Crude (no LSR polling), but a boot
 * that reaches this path is already fatal. */
/* Fase S4: the KIoPort is published into a CSpace slot as an MDB child of the
 * bootstrap-cap slot; the authority argument must be a CPtr.  Slot 40 is free
 * in userboot's root CNode (1..15 reserved, 16.. boot untypeds start well
 * above what this early path ever touches). */
#define UB_PANIC_IOPORT_SLOT 40u
static void ub_boot_panic(handle_id_t bootstrap_cap_h, const char *msg) {
    (void)bootstrap_cap_h;
    long r = ub_sys4(SYS_CAP_CREATE_IOPORT, (long)BOOT_CPTR_BOOTSTRAP_CAP,
                     0x3F8, 8, (long)UB_PANIC_IOPORT_SLOT);
    if (r == 0) {
        long io = (long)UB_PANIC_IOPORT_SLOT;
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

void iris_userboot_main(handle_id_t bootstrap_cap_h) {
    handle_id_t init_proc_h = HANDLE_INVALID;
    handle_id_t init_boot_h = HANDLE_INVALID;

    if (bootstrap_cap_h == HANDLE_INVALID)
        goto fail;

    /* Fase 28 boot-growth fix: the boot invariant is that the kernel initrd has
     * AT LEAST every image the ring-3 name→index catalog references (indices
     * 0..SL_CATALOG_COUNT-1 must resolve).  The initrd is allowed to hold MORE
     * images at higher indices (new services, backing blobs) — those are not
     * named here and are loaded by other means.  The old exact-equality check
     * turned any legitimate initrd growth into a silent dead boot (userboot
     * exited before loading init); it is now a >= check, and a genuine shortage
     * emits a bootstrap diagnostic instead of vanishing. */
    if (svc_initrd_count(bootstrap_cap_h) < (long)SL_CATALOG_COUNT) {
        ub_boot_panic(bootstrap_cap_h,
                      "[USERBOOT] FATAL: initrd catalog too small "
                      "(kernel/ring-3 mismatch); halting boot\n");
        goto fail;
    }

    /* Fase 3.4: CPtr probe — exercise the root CSpace path for boot KUntyped.
     * BOOT_CPTR_UNTYPED_START names the first boot KUntyped slot in the root
     * CNode that kernel_main populated during the Ph76 drain.  A successful
     * SYS_UNTYPED_INFO call (return >= 0) confirms the CSpace grant is live.
     * Boot is not gated on this: if the kernel ran without CSpace grants the
     * legacy handle path still works and all downstream services are unaffected. */
    (void)ub_sys3(SYS_UNTYPED_INFO, (long)BOOT_CPTR_UNTYPED_START, 0, 0);

    /* Fase 3.5/4: the CPtr probes for BOOT_CPTR_BOOTSTRAP_CAP and
     * BOOT_CPTR_VSPACE are RETIRED (Stage 4).  They resolved a slot into a
     * handle purely to prove the slot was live and closed it immediately —
     * boot was never gated on them, nothing read their result, and their only
     * lasting effect was to make userboot a SYS_CSPACE_RESOLVE consumer.  The
     * grants they probed are now exercised productively by the mints below,
     * which fail loudly if a slot is empty. */

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
        init_mints[0].src_cptr = BOOT_CPTR_BOOTSTRAP_CAP;
        init_mints[0].rights   = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        init_mints[0].badge    = 0;
        init_mints[1].slot     = IRIS_CPTR_INIT_UNTYPED;
        init_mints[1].src_cptr = BOOT_CPTR_UNTYPED_START;
        init_mints[1].rights   = RIGHT_READ | RIGHT_WRITE |
                                 RIGHT_DUPLICATE | RIGHT_TRANSFER;
        init_mints[1].badge    = 0;

        long lr = svc_load_minted(bootstrap_cap_h, "init",
                                  &init_proc_h, &init_boot_h, init_mints, 2u);
        if (lr < 0)
            goto fail;
    }

    ub_close(init_boot_h);
    ub_close(init_proc_h);
    ub_close(bootstrap_cap_h);
    ub_park_root_bootstrap();

fail:
    ub_close(init_boot_h);
    ub_close(init_proc_h);
    ub_close(bootstrap_cap_h);
    (void)ub_sys1(SYS_EXIT, 1);
    for (;;) {}
}
