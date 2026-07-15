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
static void ub_boot_panic(handle_id_t bootstrap_cap_h, const char *msg) {
    long io = ub_sys3(SYS_CAP_CREATE_IOPORT, (long)bootstrap_cap_h, 0x3F8, 8);
    if (io >= 0) {
        for (const char *p = msg; *p; p++) {
            if (*p == '\n') (void)ub_sys3(SYS_IOPORT_OUT, io, 0, (long)'\r');
            (void)ub_sys3(SYS_IOPORT_OUT, io, 0, (long)(uint8_t)*p);
        }
        (void)ub_sys1(SYS_HANDLE_CLOSE, io);
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

    /* Fase 3.5: CPtr probe for KBootstrapCap in well-known slot 1.
     * SYS_CSPACE_RESOLVE materialises the CNode slot into a new handle-table
     * entry without altering the CNode.  If it returns >= 0 the slot is live;
     * the handle is closed immediately so the main flow is unchanged.
     * Boot is not gated on this probe. */
    {
        long bprobe_h = ub_sys1(SYS_CSPACE_RESOLVE,
                                (long)BOOT_CPTR_BOOTSTRAP_CAP);
        if (bprobe_h >= 0)
            ub_close((handle_id_t)bprobe_h);
    }

    /* Fase 4: CPtr probe for KVSpace in well-known slot 2 (BOOT_CPTR_VSPACE).
     * Non-destructive: materialises the slot, then closes the handle immediately.
     * Confirms that userland can traverse the CSpace grant inserted by kernel_main.
     * Boot is not gated on this probe. */
    {
        long vsprobe_h = ub_sys1(SYS_CSPACE_RESOLVE,
                                 (long)BOOT_CPTR_VSPACE);
        if (vsprobe_h >= 0)
            ub_close((handle_id_t)vsprobe_h);
    }

    /* Fase 13 (Track I): deliver init's spawn/bootstrap cap as the
     * IRIS_CPTR_SPAWN_CAP (slot 6) pre-start mint instead of a post-spawn
     * KChannel SPAWN_CAP send — no SYS_CHAN. */
    {
        long dup_h = ub_sys2(SYS_HANDLE_DUP, (long)bootstrap_cap_h,
                             (long)(RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER));
        if (dup_h < 0)
            goto fail;
        /* Fase 18: forward ONE boot KUntyped into init so it can be handed on
         * to iris_test for the ring-3 authority suite (T125–T131).  Resolve the
         * root-CNode slot into a handle (mint source); full rights so retype
         * (WRITE) and onward mint (DUPLICATE) both work.  Non-fatal: if the
         * grant is absent the slot stays empty and the authority tests FAIL
         * loudly rather than silently skipping. */
        long ut_h = ub_sys1(SYS_CSPACE_RESOLVE, (long)BOOT_CPTR_UNTYPED_START);
        uint32_t nmints = 1u;
        struct svc_mint init_mints[2];
        init_mints[0].slot   = IRIS_CPTR_SPAWN_CAP;
        init_mints[0].src_h  = (handle_id_t)dup_h;
        init_mints[0].rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
        init_mints[0].badge  = 0;
        if (ut_h >= 0) {
            init_mints[1].slot   = IRIS_CPTR_INIT_UNTYPED;
            init_mints[1].src_h  = (handle_id_t)ut_h;
            init_mints[1].rights = RIGHT_READ | RIGHT_WRITE |
                                   RIGHT_DUPLICATE | RIGHT_TRANSFER;
            init_mints[1].badge  = 0;
            nmints = 2u;
        }
        long lr = svc_load_minted(bootstrap_cap_h, "init",
                                  &init_proc_h, &init_boot_h, init_mints, nmints);
        ub_close((handle_id_t)dup_h);
        if (ut_h >= 0) ub_close((handle_id_t)ut_h);
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
