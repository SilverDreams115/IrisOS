/*
 * init.h — shared primitives for the init service (Phase 14).
 *
 * init began as a single large main.c.  Phase 14 decomposed it into auditable
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
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)a3);
}

static inline long init_sys3(long nr, long a0, long a1, long a2) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)0);
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


/*
 * Phase S1: fabricate one kernel object from an untyped capability.
 *
 * SYS_UNTYPED_RETYPE2 publishes the new capability DIRECTLY into dest_slot of
 * init's own root CNode (dest 0 = own root).  The source must be a CPtr:
 * retype2 parents each created capability to the MDB slot of its source
 * untyped only when the source was named that way, so a handle source would
 * make every object an ancestorless LEGACY_ROOT.
 *
 * Step 4: this used to materialise the result into a handle and delete the
 * slot.  A retyped object living in a CSpace slot IS the seL4 shape — the
 * capability is the slot — and every consumer here (mint sources, notify
 * wait, exception handler, endpoint invocation) resolves a CPtr, so the
 * materialisation was pure overhead plus a handle-table entry.
 *
 * Returns 0 on success; the caller already knows the slot.
 */
static inline long init_retype_slot(uint64_t ut_cptr, uint32_t obj_type,
                                    uint32_t dest_slot, uint64_t obj_arg) {
    if (ut_cptr == 0u) return (long)IRIS_ERR_NOT_FOUND;
    (void)init_sys2(SYS_CNODE_DELETE, 0, (long)dest_slot);
    return init_sys4(SYS_UNTYPED_RETYPE2, (long)ut_cptr,
                     (long)((uint64_t)obj_type | (1ULL << 32)),
                     (long)((uint64_t)dest_slot << 32),
                     (long)obj_arg);
}

/* Step 4: init's own root-CNode slots for the objects it fabricates.  Kept
 * distinct and named so a collision is a compile-time-visible mistake, not a
 * bring-up failure. */
#define INIT_SLOT_CONSOLE_EP   43u
#define INIT_SLOT_CONSOLE_RPLY 44u
#define INIT_SLOT_SVCMGR_EP    45u
#define INIT_SLOT_SM_UNTYPED   46u
#define INIT_SLOT_FIX_WRONGTY  47u
#define INIT_SLOT_TEST_UNTYPED 49u
#define INIT_SLOT_WATCH_NOTIF  50u
#define INIT_SLOT_PROBE_NOTIF  51u
#define INIT_SLOT_S8_NOTIF     52u
#define INIT_SLOT_LOADER_WS    53u   /* loader's second-level CNode */
#define INIT_RSLOT_LK_SVCMGR   54u   /* receive slots for discovery lookups */
#define INIT_RSLOT_LK_VFS      55u
#define INIT_RSLOT_LK_KBD      56u
/* Stage 5 Step 4: init's own CSpace and VSpace, as capabilities, plus the
 * slot its one test thread is retyped into.  A thread is created by retyping a
 * TCB from init's Untyped and configuring it with these two — the static task
 * pool is no longer reachable from userland. */
#define INIT_SLOT_OWN_CSPACE   57u
#define INIT_SLOT_OWN_VSPACE   58u
#define INIT_SLOT_S8_TCB       59u
/* Stage 7 Step 7: where a fault delivers the faulting thread's capability.
 * init arms the handler for ITSELF, so its own root CNode is the mailbox and
 * this is just the slot in it. */
#define INIT_SLOT_S8_FAULT     60u

/* Phase S1: init's untyped pool — the boot untyped delegated by userboot at
 * IRIS_CPTR_INIT_UNTYPED.  Stage 4: held as the CPtr itself, checked once in
 * init_main with a non-materializing SYS_UNTYPED_INFO probe.  0 = no pool
 * (spawns that need to fabricate objects fail loudly). */
extern uint64_t g_init_untyped_c;

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
void init_early_serial_start(void);
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
void init_spawn_fb(void);
int  init_spawn_console(void);
handle_id_t init_spawn_svcmgr(void);
void init_spawn_iris_test(handle_id_t sm_h);

/* Runtime probes + S8 exception selftest (init_test.c). */
void init_runtime_probe_invalid_userptr(void);
void init_runtime_probe_timeout_overflow(void);
void init_selftest_exception(void);

#endif /* IRIS_INIT_H */
