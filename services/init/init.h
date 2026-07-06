/*
 * init.h — shared primitives for the init service (Fase 14).
 *
 * init began as a single large main.c.  Fase 14 decomposed it into auditable
 * modules that share this contract:
 *
 *   main.c            boot supervisor — orchestrates the healthy path + idle
 *                     loop; owns the log sink (console.ep / early-serial) and
 *                     the tiny process utilities
 *   init_bootstrap.c  initial-authority wiring — spawn-cap acquisition,
 *                     early-serial UART, svcmgr.ep discovery lookups, and the
 *                     S5/S6 VFS boot-health validation
 *   init_launch.c     service launch — the fb/console/svcmgr/iris_test spawns
 *                     with their pre-start CSpace mints and spawn error paths
 *   init_test.c       runtime probes / S8 exception selftest / smoke markers
 *
 * This header holds the raw syscall wrappers, the shared boot constants, and
 * the cross-module function contract.  No functional behavior lives here.
 */
#ifndef IRIS_INIT_H
#define IRIS_INIT_H

#include <stdint.h>
#include <iris/syscall.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>

/* ── Raw syscall helpers ────────────────────────────────────────────────── */

static inline long init_sys4(long nr, long a0, long a1, long a2, long a3) {
    long ret;
    register long _a3 __asm__("r10") = a3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long init_sys3(long nr, long a0, long a1, long a2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long init_sys2(long nr, long a0, long a1) {
    return init_sys3(nr, a0, a1, 0);
}

static inline long init_sys1(long nr, long a0) {
    return init_sys3(nr, a0, 0, 0);
}

static inline long init_sys0(long nr) {
    return init_sys3(nr, 0, 0, 0);
}

/* ── Shared boot constants ──────────────────────────────────────────────── */

#define INIT_RETRY_LIMIT 100
#define INIT_RETRY_SLEEP_TICKS 2

/* A1.6: init's own CSpace receive-slot for the productive vfs.ep session cap.
 * Init is the root spawner — nothing mints into its CSpace — so the 16..29
 * per-process pool (endpoint_proto.h layout) is free.  The slot is declared
 * once, holds the cap for init's whole lifetime, and is never deleted. */
#define INIT_RSLOT_VFS_EP 16u

/* ── Cross-module contract ──────────────────────────────────────────────── */

/* Boot-supervisor log sink (main.c): console.ep once up, early-serial before. */
void init_log(const char *s);

/* Console KEndpoint master send side (defined in main.c next to the log
 * sink): created by init_spawn_console, read by init_log, re-minted into
 * children by the launch module. */
extern handle_id_t g_init_console_ep_h;

/* Tiny process utilities (main.c). */
void init_exit(long code);
void init_close(handle_id_t *h);

/* Initial-authority wiring (init_bootstrap.c). */
handle_id_t init_recv_spawn_cap(handle_id_t bootstrap_ch_h);
void init_early_serial_start(handle_id_t spawn_cap_h);
void init_early_serial_write(const char *s);
void init_early_serial_stop(void);
void init_retry_pause(void);
handle_id_t init_ep_lookup_name(handle_id_t svcmgr_ep_h, const char *name);
handle_id_t init_ep_lookup_name_slot(handle_id_t svcmgr_ep_h, const char *name,
                                     uint32_t reply_slot);
int  init_wait_vfs_list_ep(handle_id_t vfs_ep_h);
int  init_wait_vfs_rw_ep(handle_id_t vfs_ep_h);

/* Service launch (init_launch.c): initrd loads via svc_load_minted with the
 * pre-start CSpace mint tables; init_spawn_svcmgr returns the svcmgr.ep send
 * side (init's discovery handle) or HANDLE_INVALID; init_spawn_iris_test
 * consumes spawn_cap_h. */
void init_spawn_fb(handle_id_t spawn_cap_h);
int  init_spawn_console(handle_id_t spawn_cap_h);
handle_id_t init_spawn_svcmgr(handle_id_t spawn_cap_h);
void init_spawn_iris_test(handle_id_t spawn_cap_h, handle_id_t sm_h);

/* Runtime probes + S8 exception selftest (init_test.c). */
void init_runtime_probe_invalid_userptr(void);
void init_runtime_probe_timeout_overflow(void);
void init_selftest_exception(void);

#endif /* IRIS_INIT_H */
