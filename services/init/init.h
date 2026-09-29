/* SPDX-License-Identifier: Apache-2.0 */
/*
 * init.h — shared primitives for the init service.
 *
 * init began as a single large main.c.  It was decomposed into auditable
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
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/endpoint_proto.h>

static inline long init_sys3(long nr, long a0, long a1, long a2) {
    return iris_syscall4((long)nr, (long)a0, (long)a1, (long)a2, (long)0);
}

static inline long init_sys1(long nr, long a0) {
    return init_sys3(nr, a0, 0, 0);
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
 * Fabricate one kernel object from an untyped capability.
 *
 * SYS_UNTYPED_RETYPE2 publishes the new capability DIRECTLY into dest_slot of
 * init's own root CNode (dest 0 = own root).  The source must be a CPtr:
 * retype2 parents each created capability to the MDB slot of its source
 * untyped only when the source was named that way, so a handle source would
 * make every object an ancestorless MDB_FLAG_UNPARENTED.
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
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)dest_slot);
    return iris_invoke((long)ut_cptr, INV_UNTYPED_RETYPE, (long)((uint64_t)obj_type | (1ULL << 32)), (long)((uint64_t)dest_slot << 32), (long)obj_arg);
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
#define INIT_SLOT_S8_FAULT_EP  52u  /* The S8 thread's fault endpoint */
#define INIT_SLOT_LOADER_WS    53u   /* loader's second-level CNode */
#define INIT_RSLOT_LK_SVCMGR   54u   /* receive slots for discovery lookups */
#define INIT_RSLOT_LK_VFS      55u
#define INIT_RSLOT_LK_KBD      56u
/* Init's own CSpace and VSpace, as capabilities, plus the
 * slot its one test thread is retyped into.  A thread is created by retyping a
 * TCB from init's Untyped and configuring it with these two — the static task
 * pool is no longer reachable from userland. */
#define INIT_SLOT_OWN_CSPACE   57u
#define INIT_SLOT_OWN_VSPACE   58u
/* The timer service's fixtures, held by init for its whole run. */
#define INIT_SLOT_TIMER_EP     63u   /* the control endpoint init serves it on */
/* 64/65 are IRIS_CPTR_DEVICE_UNTYPED / OWN_CSPACE in init's own root CNode —
 * the first draft put the IRQ capability on top of the device untyped and got
 * ALREADY_EXISTS, which is the slot map doing its job. */
#define INIT_SLOT_TIMER_IRQCAP 71u   /* the timer IRQ, claimed from IRQ control */
#define INIT_SLOT_TIMER_NOTIF  72u   /* the notification that IRQ routes into */
#define INIT_SLOT_TIMER_REPLY  73u   /* the service's reply object */
#define INIT_SLOT_TIMER_CN     74u   /* its CNode of client notifications */
#define INIT_SLOT_TIMER_UT     75u   /* its budget */
#define INIT_SLOT_TIMER_GIVE   76u   /* the derived copy init hands over per arm */
#define INIT_SLOT_IDLE_NOTIF   77u   /* What init stops on, forever */
/*
 * The PCI bus service's fixtures.
 *
 * 82 and up.  The first pick was 78, which is IRIS_CPTR_MMIO_UNTYPED — the
 * capability init has to HAND to this very service — so retyping an endpoint
 * into it destroyed the thing being handed over, and the service came up
 * holding an endpoint where its device region should have been.  It failed
 * with WRONG_TYPE four calls later, in a function that had no idea why.
 *
 * That was the third slot collision in this work, so it is the last one that
 * gets to be found by running: the static assertions at the bottom of this
 * file fail the BUILD for any init slot that collides with a service-wide
 * constant.  See IRIS_CPTR_MMIO_UNTYPED in endpoint_proto.h for the other two.
 */
#define INIT_SLOT_PCI_EP       82u   /* the endpoint init keeps and hands on  */
#define INIT_SLOT_PCI_IOPORT   83u   /* 0xCF8..0xCFF, claimed for the service */
#define INIT_SLOT_PCI_REPLY    84u   /* its reply object                      */
#define INIT_SLOT_PCI_UT       85u   /* its budget                            */
/* ...and the AHCI disk service's.  89 and up: 86 and 87 are IRIS_CPTR_ASID_POOL
 * and IRIS_CPTR_DOMAIN_CONTROL_TEST, 88 is IRIS_CPTR_ACPI_UNTYPED. */
#define INIT_SLOT_BLK_EP       89u
#define INIT_SLOT_BLK_REPLY    90u
#define INIT_SLOT_BLK_UT       91u
/* ...and the network service's. */
#define INIT_SLOT_NET_EP       92u
#define INIT_SLOT_NET_REPLY    93u
#define INIT_SLOT_NET_UT       94u
/* ...and the scratch the ARP boot check needs: a paging level and the two
 * buffer capabilities the network service hands back.  100 and up, because
 * 96..99 are IRIS_CPTR_IOSPACE_CONTROL / DOMAIN_CONTROL / SCHED_CONTROL /
 * FB_CONTROL and init holds all four. */
#define INIT_SLOT_NET_PT      100u
#define INIT_SLOT_NET_TX      101u
#define INIT_SLOT_NET_RX      102u
/* ...and the persistent filesystem's. */
#define INIT_SLOT_FS_EP       103u
#define INIT_SLOT_FS_REPLY    104u
#define INIT_SLOT_FS_UT       105u
#define INIT_SLOT_FS_BUF      106u
/* ...and the IP stack's. */
#define INIT_SLOT_IP_EP       107u
#define INIT_SLOT_IP_REPLY    108u
#define INIT_SLOT_IP_UT       109u
#define INIT_SLOT_IP_BUF      110u
/* ...and the PROGRAM spawner's.  `proc` is the first service init starts that
 * exists to start something else, so init keeps what a supervisor keeps: the
 * endpoint it talks to, and the thread of the program it asked for. */
#define INIT_SLOT_PROC_EP     111u
#define INIT_SLOT_PROC_REPLY  112u
#define INIT_SLOT_PROC_UT     113u
#define INIT_SLOT_PROC_CHILD  114u  /* the program's own thread               */
#define INIT_SLOT_PROC_NOTIF  115u  /* where its death is signalled           */
#define INIT_SLOT_PROC_GIVE   116u  /* the copy the timer service holds       */
/* ...and the shared-object registry's, plus what it takes to prove two live
 * programs share one library: an endpoint they both report on, a reply object
 * EACH (two callers blocked at once is two replies owed), and a thread each. */
#define INIT_SLOT_OBJREG_EP   117u
#define INIT_SLOT_OBJREG_RPLY 118u
#define INIT_SLOT_OBJREG_UT   119u
#define INIT_SLOT_SHARE_EP    120u
#define INIT_SLOT_SHARE_RA    121u
#define INIT_SLOT_SHARE_RB    122u
#define INIT_SLOT_SHARE_TA    123u
#define INIT_SLOT_SHARE_TB    124u
#define INIT_SLOT_SHARE_GIVE  125u  /* the copy handed to proc, for the child */
#define INIT_SLOT_S8_TCB       59u
/* Where a fault delivers the faulting thread's capability.
 * init arms the handler for ITSELF, so its own root CNode is the mailbox and
 * this is just the slot in it. */
#define INIT_SLOT_S8_REPLY     60u  /* Reply authority for that fault */
/* Iris_test's ROOT CSpace, kept from its spawn so init can
 * still mint into it afterwards — delegating into a child names the CSpace,
 * and there is no longer a way to reach one by naming the process. */
#define INIT_SLOT_TEST_CNODE   61u
/* Iris_test's first THREAD, kept so init waits on the
 * execution that ends rather than on a process capability. */
#define INIT_SLOT_TEST_TCB     62u

/* Init's untyped pool — the boot untyped delegated by userboot at
 * IRIS_CPTR_INIT_UNTYPED.  Held as the CPtr itself, checked once in
 * init_main with a non-materializing SYS_UNTYPED_INFO probe.  0 = no pool
 * (spawns that need to fabricate objects fail loudly). */
extern uint64_t g_init_untyped_c;

/* ── Cross-module contract ──────────────────────────────────────────────── */

/* Boot-supervisor log sink (main.c): console.ep once up, early-serial before. */
void init_log(const char *s);

/* Console KEndpoint master send side (defined in main.c next to the log
 * sink): created by init_spawn_console, read by init_log, re-minted into
 * children by the launch module. */
extern iris_cptr_t g_init_console_ep_h;
/* The timer service's control endpoint, init's own copy. */
extern iris_cptr_t g_init_timer_ep_h;
int init_spawn_timer(void);
int init_spawn_pci(void);
int init_spawn_blk(void);
int init_spawn_net(void);
int init_spawn_fs(void);
int init_spawn_ip(void);
/* The program spawner, and the first program.  Takes the vfs.ep session cap
 * init resolved through svcmgr, because `proc` reads its images through the
 * filesystem and there is nothing else to give it. */
int init_spawn_proc(iris_cptr_t vfs_ep_h);

/*
 * What this machine turned out to be.
 *
 * Each spawn already logs its findings, and on a machine with a serial port
 * that was enough.  On one without, the log goes to the screen -- and the
 * screen SCROLLS, so by the time a boot settles the findings have been pushed
 * off by whatever the supervisor said last.  A person looking at that machine
 * sees service-restart chatter and learns nothing about their disk.
 *
 * So the facts are recorded as they are discovered and printed once more, in a
 * block, as the last thing before the idle loop.  It is the same information;
 * what changes is that it is still there to be read.
 */
struct init_findings {
    uint32_t pci_functions, pci_windows, pci_carve;
    uint32_t blk_disks, blk_contained, blk_home;
    uint64_t blk_window;
    uint32_t fs_mounted, fs_generation, fs_formatted, fs_files, fs_foreign;
    uint32_t net_link, net_seen, net_vd;
    /* service step in the low byte, the backend's in the next */
    uint32_t net_step;
    uint64_t net_mac;
    uint32_t ip_ok, ip_bytes;
    /* The TFTP opcode that came back: 3 = data, 5 = the server refused
     * the file, 0 = nothing recognisable.  A round trip happened for
     * either of the first two. */
    uint32_t ip_reply;
    /* The first PROGRAM: the step `proc` reached, and what the program exited
     * with.  `prog_step == PROC_STEP_RUNNING` and `prog_exit == 42` is the
     * whole claim — a file was read by path, laid out, given a System V
     * initial stack, and READ ITS OWN STACK BACK correctly.  Any other exit is
     * `hello` naming the piece that was wrong. */
    uint32_t prog_step, prog_exit, prog_ran;
    /* ...and the second program, which is about MEMORY rather than about
     * starting: the status `alloc` exited with, and the region that came back
     * when its last capability went, in KiB.  `prog_reclaimed == prog_given` is
     * the claim; the two numbers are carried separately because "most of it" is
     * the answer a leak gives and it has to be readable as a number. */
    uint32_t prog_mem_exit, prog_given, prog_reclaimed;
    /* ...and the shared object: how many KiB of library text exist ONCE
     * instead of once per consumer, whether two live programs measured the
     * same physical frame for it, whether their data was their own, and
     * whether one revoke reached both. */
    uint32_t obj_text_kib, obj_shared, obj_private, obj_revoked, obj_exit;
};
extern struct init_findings g_init_found;
void init_report_findings(void);

/* Tiny process utilities (main.c). */
void init_exit(long code);
void init_close(iris_cptr_t *h);

/* Initial-authority wiring (init_bootstrap.c). */
void init_early_serial_start(void);
void init_early_serial_write(const char *s);
/* Register init's own IPC buffer frame (best-effort), and the buffer
 * itself — the log path marshals into it too, because a thread has one. */
void init_ipc_buffer_init(void);
extern uint8_t *g_init_buf;
void init_early_serial_stop(void);
void init_retry_pause(void);
iris_cptr_t init_ep_lookup_name(iris_cptr_t svcmgr_ep_h, const char *name);
iris_cptr_t init_ep_lookup_name_slot(iris_cptr_t svcmgr_ep_h, const char *name,
                                     uint32_t reply_slot);
int  init_wait_vfs_list_ep(iris_cptr_t vfs_ep_h);
int  init_wait_vfs_rw_ep(iris_cptr_t vfs_ep_h);

/* Service launch (init_launch.c): initrd loads via svc_load_minted with the
 * pre-start CSpace mint tables; init_spawn_svcmgr returns the svcmgr.ep send
 * side (init's discovery handle) or IRIS_CPTR_NULL; init_spawn_iris_test
 * consumes spawn_cap_h. */
void init_spawn_fb(void);
int  init_spawn_console(void);
iris_cptr_t init_spawn_svcmgr(void);
void init_spawn_iris_test(iris_cptr_t sm_h);

/* Runtime probes + S8 exception selftest (init_test.c). */
void init_runtime_probe_invalid_userptr(void);
void init_runtime_probe_timeout_overflow(void);
void init_selftest_exception(void);

/*
 * ── init's own slots never collide with a service-wide constant ────────────
 *
 * A mint into an occupied slot DELETES the occupant, so a collision here does
 * not fail — it succeeds, over something that was in use, and the damage shows
 * up somewhere else entirely.  Three of them were found by running the system
 * during the platform work, one of which destroyed init's loader workspace and made
 * every subsequent service load fail with nothing naming the slot.
 *
 * So the build checks.  Adding an init slot that shadows an IRIS_CPTR_* is now
 * a compile error that names the line, which is the difference between a rule
 * and a rule that holds.
 */
#define INIT_SLOT_FREE_OF(name, slot) \
    _Static_assert((uint64_t)(slot) != (name), \
                   "init slot collides with " #name)

#define INIT_SLOT_CHECK(slot)                                 \
    INIT_SLOT_FREE_OF(IRIS_CPTR_INIT_UNTYPED,   slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_INIT_UNTYPED2,  slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_PROC_CONTROL,   slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_IOPORT_CONTROL, slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_IRQ_CONTROL,    slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_DEVICE_UNTYPED, slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_MMIO_UNTYPED,   slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_OWN_CSPACE,     slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_OWN_VSPACE,     slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_OWN_TCB,        slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_FB_CONTROL,     slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_ASID_POOL,      slot);        \
    INIT_SLOT_FREE_OF(IRIS_CPTR_IOSPACE_CONTROL, slot)

INIT_SLOT_CHECK(INIT_SLOT_PCI_EP);
INIT_SLOT_CHECK(INIT_SLOT_PCI_IOPORT);
INIT_SLOT_CHECK(INIT_SLOT_PCI_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_PCI_UT);
INIT_SLOT_CHECK(INIT_SLOT_BLK_EP);
INIT_SLOT_CHECK(INIT_SLOT_BLK_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_BLK_UT);
INIT_SLOT_CHECK(INIT_SLOT_NET_EP);
INIT_SLOT_CHECK(INIT_SLOT_NET_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_NET_UT);
INIT_SLOT_CHECK(INIT_SLOT_NET_PT);
INIT_SLOT_CHECK(INIT_SLOT_NET_TX);
INIT_SLOT_CHECK(INIT_SLOT_NET_RX);
INIT_SLOT_CHECK(INIT_SLOT_FS_EP);
INIT_SLOT_CHECK(INIT_SLOT_FS_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_FS_UT);
INIT_SLOT_CHECK(INIT_SLOT_FS_BUF);
INIT_SLOT_CHECK(INIT_SLOT_IP_EP);
INIT_SLOT_CHECK(INIT_SLOT_IP_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_IP_UT);
INIT_SLOT_CHECK(INIT_SLOT_IP_BUF);
INIT_SLOT_CHECK(INIT_SLOT_PROC_EP);
INIT_SLOT_CHECK(INIT_SLOT_PROC_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_PROC_UT);
INIT_SLOT_CHECK(INIT_SLOT_PROC_CHILD);
INIT_SLOT_CHECK(INIT_SLOT_PROC_NOTIF);
INIT_SLOT_CHECK(INIT_SLOT_PROC_GIVE);
INIT_SLOT_CHECK(INIT_SLOT_OBJREG_EP);
INIT_SLOT_CHECK(INIT_SLOT_OBJREG_RPLY);
INIT_SLOT_CHECK(INIT_SLOT_OBJREG_UT);
INIT_SLOT_CHECK(INIT_SLOT_SHARE_EP);
INIT_SLOT_CHECK(INIT_SLOT_SHARE_RA);
INIT_SLOT_CHECK(INIT_SLOT_SHARE_RB);
INIT_SLOT_CHECK(INIT_SLOT_SHARE_TA);
INIT_SLOT_CHECK(INIT_SLOT_SHARE_TB);
INIT_SLOT_CHECK(INIT_SLOT_SHARE_GIVE);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_EP);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_IRQCAP);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_NOTIF);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_CN);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_UT);
INIT_SLOT_CHECK(INIT_SLOT_TIMER_GIVE);
INIT_SLOT_CHECK(INIT_SLOT_IDLE_NOTIF);
INIT_SLOT_CHECK(INIT_SLOT_LOADER_WS);
INIT_SLOT_CHECK(INIT_SLOT_TEST_CNODE);
INIT_SLOT_CHECK(INIT_SLOT_TEST_TCB);
INIT_SLOT_CHECK(INIT_SLOT_S8_FAULT_EP);
INIT_SLOT_CHECK(INIT_SLOT_S8_REPLY);
INIT_SLOT_CHECK(INIT_SLOT_S8_TCB);

#endif /* IRIS_INIT_H */
