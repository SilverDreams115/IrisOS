#include <iris/svcmgr_proto.h>
#include <iris/kbd_proto.h>
#include "service_catalog.h"
#include <iris/syscall.h>
#include <iris/vfs_ep_proto.h>
#include <iris/nc/error.h>
#include <iris/nc/handle.h>
#include <iris/ipc_msg.h>
#include <iris/ipc_recv_slot.h>
#include <iris/endpoint_proto.h>
#include <stdint.h>
#include "../common/svc_loader.h"
#include "../common/console_client.h"

/*
 * Runtime-published services are bounded by the same per-process handle-table
 * ceiling as the rest of the system, not by an arbitrary local cap.
 */
#define SVCMGR_DYNAMIC_SERVICE_CAP 256u /* mirrors HANDLE_TABLE_MAX (A1.7).
                                         * CSpace-pool registrations (slots
                                         * 64..255) cover the first 192; the
                                         * legacy-handle overflow shares the
                                         * shrunken table with the working
                                         * set, which the A1.7 measurements
                                         * bound at ~33 entries. */
/* Indexed by IRQ number (0–15); mirrors KIRQCAP_POOL_SIZE in kirqcap.h. */
/* Fase S4 (Etapa 3 prep): device caps live in svcmgr's CSPACE, not its handle
 * table.  SYS_CAP_CREATE_IRQCAP/_IOPORT publish into these slots as MDB
 * children of the bootstrap-cap slot, so revoking the bootstrap cap revokes
 * every device cap issued under it.  Slots 16..47 are free in svcmgr's root
 * CNode (1..15 well-known, 62/63 scratch, 64..255 receive pool). */
/* Stage 5 Etapa 2: svcmgr's device-cap slots move up to 96..127 so that the
 * low, well-known part of every service's root CNode can hold the boot control
 * capabilities the kernel now publishes one per authority.  These slots are
 * svcmgr's own bookkeeping — nobody outside names them — so relocating them
 * costs nothing but has to stay clear of the registration pool below. */
#define SVCMGR_IRQCAP_SLOT_BASE    96u   /* + IRQ number   (96..111) */
#define SVCMGR_IOPORT_SLOT_BASE   112u   /* + service_id  (112..127) */

#define SVCMGR_IRQ_CAPS_TABLE_SIZE 16u
/* Indexed by service_id (0–2); mirrors IRIS_SERVICE_RUNTIME_SLOT_COUNT. */
#define SVCMGR_IOPORT_CAPS_TABLE_SIZE IRIS_SERVICE_RUNTIME_SLOT_COUNT

struct svcmgr_service_state {
    handle_id_t public_h;
    handle_id_t reply_h;
    handle_id_t proc_h;
    /* Service-owned KEndpoint master (Fase 7.1; manifest own_service_ep=1).
     * Created once at first boot and kept across restarts so client caps
     * stay valid; recv side goes to the child at bootstrap (kind 0x21) and
     * the send side is published as "<image_name>.ep". */
    uint32_t    ep_c;   /* CPtr slot, 0 = absent */
    /* IRQ KNotification master (Fase 7.6; manifest irq_notify=1). Created
     * once and kept across restarts; the kernel signals it per IRQ and the
     * WAIT side goes to the child at bootstrap (kind 0x23). */
    uint32_t    irq_notif_c;   /* CPtr slot, 0 = absent */
    /* Fase S1: per-service reply sub-untyped (4 KiB carved once from svcmgr's
     * pool).  Each (re)boot RESETs it (when the previous reply objects died
     * with the child) and retypes fresh reply object(s) that are minted into
     * the child at IRIS_CPTR_OWN_REPLY(2).  svcmgr NEVER retains a reply cap:
     * a retained copy would suppress the close-wakes-caller path on child
     * death. */
    uint32_t    reply_ut_c;   /* CPtr slot, 0 = absent */
    uint8_t restart_count;
    uint8_t degraded;       /* Fase 24: 1 = restart budget exhausted, not revived */
    uint8_t reserved[2];
    /* Fase 10: service generation — starts at 1 when first booted and bumps
     * on every restart (death→respawn) and on an explicit RESTART request.
     * A client that cached a generation can detect, via IRIS_SVCMGR_EP_STATUS,
     * that the service it holds a cap to has since been restarted. */
    uint32_t generation;
};

struct svcmgr_dynamic_service {
    uint32_t endpoint;
    handle_id_t public_h;
    /* A1.6: canonical CSpace storage for the registered master.  When the
     * REGISTER cap arrives through a declared receive-slot it lives in
     * svcmgr's root CNode at this CPtr (1..1023) and public_h stays
     * HANDLE_INVALID; when it arrives as a legacy handle (no slot available,
     * or the TOCTOU fallback) public_h holds it and public_cptr is 0.
     * Exactly one of the two is set while active. */
    uint32_t public_cptr;
    iris_rights_t client_rights;
    char name[SVCMGR_SERVICE_NAME_CAP];
    uint8_t active;
    /* Fase 10: badge of the client that registered this service (sender_badge
     * stamped by the kernel on the EP REGISTER call).  UNREGISTER over the EP
     * requires a matching owner badge; legacy KChannel registrations are
     * owner_badge 0 (unidentified). generation supports logical revocation. */
    uint64_t owner_badge;
    uint32_t generation;
};

struct svcmgr_state {
    /* Etapa 4: the spawn/authority capability as a CPtr slot, not a handle.
     * Starts at IRIS_CPTR_SPAWN_CAP and MOVES to SVCMGR_SPAWN_NARROW_SLOT once
     * bootstrap strips HW_ACCESS (see svcmgr_request_hardware_caps).  0 = absent. */
    uint64_t    spawn_cap_c;
    /* Etapa 4: both are CPtr slots, not handles.  Endpoint invocation resolves
     * either namespace, and as mint sources a CPtr installs each child's cap as
     * an MDB child of our slot — so the delegation stays revocable.  0 = absent. */
    uint64_t console_ep_c;     /* console KEndpoint send side (Fase 7.3):
                                * delivered by init at bootstrap (kind 0x22),
                                * published as "console.ep". */
    uint64_t ep_c;                                            /* svcmgr KEndpoint for EP-based discovery */
    uint32_t    death_notif_c;  /* Track B: one KNotification; bit (1<<service_id)
                                 * signalled by the kernel on each service exit. */
    /* Fase S1: svcmgr's delegated untyped pool (init carves a sub-untyped and
     * mints it at IRIS_CPTR_OWN_UNTYPED).  Every endpoint / notification /
     * reply svcmgr fabricates is retyped from here — the retired create
     * syscalls are never used.
     *
     * Etapa 4: held as the CPtr, not a handle.  retype2 only gives a created
     * capability a real MDB parent when its source untyped was named by CPtr;
     * a handle source has no CSpace ancestor and the result is a LEGACY_ROOT.
     * 0 = no pool. */
    uint64_t    untyped_c;
    /* Fase S4: CPtr slots (0 = absent), not handles. */
    uint32_t    irq_caps[SVCMGR_IRQ_CAPS_TABLE_SIZE];       /* indexed by IRQ number  */
    uint32_t    ioport_caps[SVCMGR_IOPORT_CAPS_TABLE_SIZE]; /* indexed by service_id  */
    struct svcmgr_service_state services[IRIS_SERVICE_RUNTIME_SLOT_COUNT];
    struct svcmgr_dynamic_service dynamic[SVCMGR_DYNAMIC_SERVICE_CAP];
};

static struct svcmgr_state g_svcmgr_state;

static const char sm_str_started[]      = "[SVCMGR] started\n";
static const char sm_str_ready[]        = "[SVCMGR] ready\n";
static const char sm_str_recverr[]      = "[SVCMGR] recv error, retrying\n";
static const char sm_str_spawnok[]      = "[SVCMGR] service spawned\n";
static const char sm_str_spawnfail[]    = "[SVCMGR] WARN: spawn failed\n";
static const char sm_str_bootok[]       = "[SVCMGR] child bootstrap OK\n";
static const char sm_str_bootfail[]     = "[SVCMGR] WARN: child bootstrap failed\n";
/* Fase 13 (Track C): sm_str_bootdupfail/bootsendfail retired with the
 * KChannel bootstrap senders — bootstrap caps are now pre-start CSpace mints. */
static const char sm_str_lookupfail[]   = "[SVCMGR] WARN: lookup failed\n";
/* sm_str_svc_exited replaced by inline format in svcmgr_release_service (logs name) */
static const char sm_str_irqfail[]      = "[SVCMGR] WARN: irq route xfer failed\n";
static const char sm_str_svc_unknown[]  = "[SVCMGR] WARN: unknown bootstrap service\n";
static const char sm_str_watchok[]      = "[SVCMGR] lifecycle watch armed\n";
static const char sm_str_bootcapfail[]  = "[SVCMGR] FATAL: missing spawn capability\n";
static const char sm_str_untypedfail[]  = "[SVCMGR] WARN: missing untyped pool\n";
static const char sm_str_stripfail[]    = "[SVCMGR] WARN: bootstrap strip FAILED\n";
static const char sm_str_restart[]           = "[SVCMGR] restarting ";
static const char sm_str_restart_exhausted[] = "[SVCMGR] WARN: restart budget exhausted: ";
static const char sm_str_ep_ready[]          = "[SVCMGR] ep ready\n";
static const char sm_str_lookup_name_ok[]    = "[SVCMGR] lookup-name reply OK\n";

static inline int64_t svcmgr_syscall4(uint64_t num, uint64_t arg0, uint64_t arg1,
                                      uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    register uint64_t _a3 __asm__("r10") = arg3;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "d"(arg2), "r"(_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t svcmgr_syscall3(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "d"(arg2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t svcmgr_syscall0(uint64_t num) {
    return svcmgr_syscall3(num, 0, 0, 0);
}

static inline int64_t svcmgr_syscall1(uint64_t num, uint64_t arg0) {
    return svcmgr_syscall3(num, arg0, 0, 0);
}

static inline int64_t svcmgr_syscall2(uint64_t num, uint64_t arg0, uint64_t arg1) {
    return svcmgr_syscall3(num, arg0, arg1, 0);
}

/*
 * Fase S1: fabricate one kernel object from an untyped pool and return it as
 * a handle-table handle (mint source).  SYS_UNTYPED_RETYPE2 publishes the new
 * capability into a scratch slot of svcmgr's own root CNode (dest 0 = own
 * root); the slot is materialized to a handle and then deleted, so the handle
 * is the only remaining svcmgr reference.  Slot 62 sits below the dynamic
 * receive-slot pool (64..255) and outside every well-known mint slot.
 */
/* Etapa 4: narrowed spawn cap after the bootstrap HW_ACCESS strip (48..61 free). */
#define SVCMGR_SPAWN_NARROW_SLOT 48u
/* Etapa 4: the long-lived per-service masters live in svcmgr's own CSpace,
 * two slots per service in 49..56 (49..61 is free between the ioport-cap
 * table and the S1 scratch slot).  They are created once and kept across
 * restarts, so a fixed slot per service is the natural home — and a mint from
 * a slot makes each child's cap an MDB child of it, hence revocable. */
#define SVCMGR_MSLOT_EP(id)  (49u + (uint32_t)(id) * 2u)
#define SVCMGR_MSLOT_IRQ(id) (50u + (uint32_t)(id) * 2u)
/* Per-service reply sub-untyped (57..60), the single death notification (61),
 * and the two transient reply objects a booting child receives (64/65).  The
 * receive-slot pool starts above them. */
#define SVCMGR_MSLOT_REPLYUT(id) (57u + (uint32_t)(id))
#define SVCMGR_SLOT_DEATH_NOTIF  61u
#define SVCMGR_SLOT_REPLY1       64u
#define SVCMGR_SLOT_REPLY2       65u
/* Fase S4 (Etapa 2): outbound cap-transfer source slot.  Every cap svcmgr
 * hands to a client is minted here first and transferred BY CPtr; the kernel
 * consumes the slot on a committed delivery.  63 sits below the dynamic
 * receive-slot pool (64..255) and outside every well-known mint slot. */
#define SVCMGR_XFER_SLOT       63u
/* Etapa 4: retype straight into a CSpace slot — the capability IS the slot.
 * Used for the objects svcmgr keeps; the materialising variant below remains
 * only where a consumer still takes a handle. */
static int64_t svcmgr_retype_to_slot(uint64_t ut_cptr, uint32_t obj_type,
                                     uint32_t dest_slot, uint64_t obj_arg) {
    if (ut_cptr == 0u) return (int64_t)IRIS_ERR_NOT_FOUND;
    (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, (uint64_t)dest_slot);
    return svcmgr_syscall4(SYS_UNTYPED_RETYPE2, ut_cptr,
                           (uint64_t)obj_type | (1ULL << 32),
                           ((uint64_t)dest_slot << 32), obj_arg);
}


/* Fase 13: svcmgr logs over console.ep (CONSOLE_EP_OP_WRITE), not the legacy
 * KChannel console writer.  Synchronous per-write flush; if console.ep is not
 * yet wired the line is dropped (same as the old early-boot behaviour). */
static uint8_t g_svcmgr_log_buf[IRIS_IPC_BUF_SIZE];
static void svcmgr_log(const char *msg) {
    (void)console_ep_write(g_svcmgr_state.console_ep_c, g_svcmgr_log_buf, msg);
}

static void svcmgr_log_u32(uint32_t value) {
    char buf[12];
    uint32_t i = 11;
    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value != 0) {
            buf[--i] = (char)('0' + (value % 10u));
            value /= 10u;
        }
    }
    svcmgr_log(buf + i);
}

/* Release a capability, whichever namespace names it (Etapa 4).
 *
 * The loader hands back process capabilities that live in a CSpace slot now.
 * Closing one as a handle is a silent no-op, and the symptom is not local: the
 * slot keeps the capability alive, so the process never reaches zero
 * references and the live-process gauge drifts upward across restarts. */
static void svcmgr_close_handle_if_valid(handle_id_t *h) {
    if (!h || *h == HANDLE_INVALID) return;
    uint32_t v = (uint32_t)*h;
    if (v >= 256u)
        (void)svcmgr_syscall2(SYS_CNODE_DELETE, (uint64_t)(v & 0xFFu),
                              (uint64_t)(v >> 8));
    else
        (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, (uint64_t)v);
    *h = HANDLE_INVALID;
}

/*
 * Receive bootstrap messages from the bootstrap channel.
 * init sends one message before svcmgr starts:
 *   SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP — hardware/spawn authority cap.
 * (Fase 13/Track I: KIND_CONSOLE_CAP retired — svcmgr logs over console.ep.)
 *
 * Returns 1 once SPAWN_CAP has been received.
 */
/* int svcmgr_recv_bootstrap_caps retired — Fase 13/Track I (legacy KChannel LOOKUP / bootstrap recv). */

/*
 * Request hardware capabilities from the kernel using the spawn cap as authority.
 * Policy (which service gets which IRQ / port range) comes from the service catalog
 * here in svcmgr, not from the kernel.
 */
static void svcmgr_request_hardware_caps(struct svcmgr_state *state) {
    uint32_t ci;

    if (!state || state->spawn_cap_c == 0u) return;

    for (ci = 0; ci < iris_service_catalog_count(); ci++) {
        const struct iris_service_catalog_entry *e = iris_service_catalog_at(ci);
        if (!e) continue;

        if (e->irq_num != 0xFFu && e->irq_num < SVCMGR_IRQ_CAPS_TABLE_SIZE &&
            state->irq_caps[e->irq_num] == 0u) {
            uint32_t slot = SVCMGR_IRQCAP_SLOT_BASE + e->irq_num;
            /* Fase S4: authority BY CPtr (it becomes the MDB parent).
             * Stage 5 Etapa 2: the authority is the IRQ control capability. */
            int64_t r = svcmgr_syscall4(SYS_CAP_CREATE_IRQCAP,
                                        IRIS_CPTR_IRQ_CONTROL, e->irq_num,
                                        0, slot);
            if (r == 0) state->irq_caps[e->irq_num] = slot;
        }

        if (e->ioport_count > 0u && e->service_id < SVCMGR_IOPORT_CAPS_TABLE_SIZE &&
            state->ioport_caps[e->service_id] == 0u) {
            uint32_t slot = SVCMGR_IOPORT_SLOT_BASE + e->service_id;
            int64_t r = svcmgr_syscall4(SYS_CAP_CREATE_IOPORT,
                                        IRIS_CPTR_IOPORT_CONTROL,
                                        e->ioport_base, e->ioport_count, slot);
            if (r == 0) state->ioport_caps[e->service_id] = slot;
        }
    }

    /*
     * Bootstrap is over: drop the authority to claim MORE hardware.
     *
     * Stage 5 Etapa 2 turns this from a narrowing into a deletion.  It used to
     * be derive-then-delete over a permission mask — SYS_BOOTCAP_RESTRICT
     * cloned the monolith without IRIS_BOOTCAP_HW_ACCESS and svcmgr deleted
     * the wide original — because device authority was a BIT on the same
     * capability that carries spawn and debug authority, and the only way to
     * drop one bit was to rebuild the object.  Device authority is its own
     * capability now, so giving it up is deleting the slot that holds it.
     *
     * The device caps claimed above are MDB children of these slots, so
     * deleting the parents reparents them onto init's slots rather than
     * destroying them: svcmgr keeps the hardware it already claimed and loses
     * only the authority to claim more, which is precisely the intent.
     */
    (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, IRIS_CPTR_IRQ_CONTROL);
    (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, IRIS_CPTR_IOPORT_CONTROL);

    /* What is left of the monolith mixes SPAWN_SERVICE and FRAMEBUFFER;
     * svcmgr needs the first and never uses the second, so the mask narrowing
     * survives here until those two split as well.  Debug authority left the
     * mask in Etapa 2b and arrives as its own capability. */
    if (svcmgr_syscall3(SYS_BOOTCAP_RESTRICT, state->spawn_cap_c,
                        IRIS_BOOTCAP_SPAWN_SERVICE,
                        SVCMGR_SPAWN_NARROW_SLOT) == 0) {
        (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, IRIS_CPTR_SPAWN_CAP);
        state->spawn_cap_c = SVCMGR_SPAWN_NARROW_SLOT;
    } else {
        svcmgr_log(sm_str_stripfail);
    }
}

static struct svcmgr_service_state *svcmgr_service_state(struct svcmgr_state *state,
                                                         uint32_t service_id) {
    if (!state) return 0;
    if (service_id >= (uint32_t)(sizeof(state->services) / sizeof(state->services[0])))
        return 0;
    return &state->services[service_id];
}

/*
 * svcmgr_seal_handle_if_valid — seal a channel handle before closing it.
 *
 * Called when retiring old service endpoint handles prior to a restart.
 * Sealing wakes blocked receivers immediately with IRIS_ERR_CLOSED so that
 * stale client handles fail fast instead of silently queuing to a dead service.
 * The handle is then closed normally to drop the master reference.
 */
/* Fase 13 (Track I): svcmgr_seal_handle_if_valid retired — the only sealable
 * KChannels were the legacy service/reply pair, now gone (every service is
 * endpoint_only).  Dynamic masters are KEndpoint caps, which are just closed. */
static void svcmgr_clear_service_masters(struct svcmgr_state *state, uint32_t service_id) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, service_id);
    if (!svc) return;
    svcmgr_close_handle_if_valid(&svc->public_h);
    svcmgr_close_handle_if_valid(&svc->reply_h);
}

static int svcmgr_should_restart_service(struct svcmgr_state *state,
                                         const struct iris_service_catalog_entry *manifest) {
    struct svcmgr_service_state *svc;

    if (!state || !manifest) return 0;
    if (!manifest->autostart || !manifest->restart_on_exit) return 0;
    svc = svcmgr_service_state(state, manifest->service_id);
    if (!svc) return 0;
    return svc->restart_count < manifest->restart_limit;
}

static void svcmgr_copy_name(char *dst, const char *src) {
    uint32_t i = 0;
    for (; i + 1u < SVCMGR_SERVICE_NAME_CAP && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    for (i++; i < SVCMGR_SERVICE_NAME_CAP; i++) dst[i] = '\0';
}

static int svcmgr_name_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static int svcmgr_name_is_catalog(const char *name) {
    for (uint32_t i = 0; i < iris_service_catalog_count(); i++) {
        const struct iris_service_catalog_entry *manifest = iris_service_catalog_at(i);
        if (manifest && manifest->image_name && svcmgr_name_equal(manifest->image_name, name))
            return 1;
    }
    return 0;
}

static const struct iris_service_catalog_entry *svcmgr_catalog_find_name(const char *name) {
    for (uint32_t i = 0; i < iris_service_catalog_count(); i++) {
        const struct iris_service_catalog_entry *manifest = iris_service_catalog_at(i);
        if (manifest && manifest->image_name && svcmgr_name_equal(manifest->image_name, name))
            return manifest;
    }
    return 0;
}

/* *svcmgr_dynamic_find_endpoint retired — Fase 13/Track I */

static struct svcmgr_dynamic_service *svcmgr_dynamic_find_name(struct svcmgr_state *state,
                                                               const char *name) {
    if (!state || !name || name[0] == '\0') return 0;
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        if (state->dynamic[i].active && svcmgr_name_equal(state->dynamic[i].name, name))
            return &state->dynamic[i];
    }
    return 0;
}

/* True iff name ends in the reserved ".ep" suffix (Fase 7.1). */
static int svcmgr_name_has_ep_suffix(const char *name) {
    uint32_t len = 0;
    if (!name) return 0;
    while (len < SVCMGR_SERVICE_NAME_CAP && name[len]) len++;
    if (len < 4u || len >= SVCMGR_SERVICE_NAME_CAP) return 0;
    return name[len - 3u] == '.' && name[len - 2u] == 'e' && name[len - 1u] == 'p';
}

/*
 * Resolve reserved endpoint names (Fase 7.1):
 *   "svcmgr.ep"       → svcmgr's own discovery KEndpoint
 *   "<image_name>.ep" → the service's KEndpoint (own_service_ep catalog flag)
 * Returns 1 and fills master/allowed on success. These names take precedence
 * over (and are rejected from) the dynamic registry, so they cannot be
 * spoofed by runtime registration.
 */
static int svcmgr_resolve_ep_name(struct svcmgr_state *state, const char *name,
                                  handle_id_t *master_h, iris_rights_t *allowed,
                                  uint32_t *out_cptr) {
    if (!state || !master_h || !allowed || !out_cptr) return 0;
    if (!svcmgr_name_has_ep_suffix(name)) return 0;

    /* Etapa 4: our own and console's endpoints are CSpace slots, so they are
     * returned through out_cptr and minted with SYS_CSPACE_MINT.  They cannot
     * go through master_h: SYS_CNODE_MINT resolves its SOURCE in the handle
     * namespace only, so a CPtr source is not expressible there. */
    if (svcmgr_name_equal(name, "svcmgr.ep")) {
        if (state->ep_c == 0u) return 0;
        *out_cptr = (uint32_t)state->ep_c;
        /* Discovery cap: TRANSFER allows holders (e.g. init) to distribute
         * it to children — including by CSpace mint (Fase 8), which needs
         * DUPLICATE. It only grants EP_CALL on svcmgr, never recv. */
        *allowed  = RIGHT_WRITE | RIGHT_TRANSFER | RIGHT_DUPLICATE;
        return 1;
    }

    /* "console.ep" (Fase 7.3): the console is spawned by init, not from the
     * catalog; init delivers the send side at bootstrap (kind 0x22). Same
     * anti-spoof property as catalog ".ep" names: bootstrap-delivered,
     * never runtime-registered. */
    if (svcmgr_name_equal(name, "console.ep")) {
        if (state->console_ep_c == 0u) return 0;
        *out_cptr = (uint32_t)state->console_ep_c;
        /* DUPLICATE (Fase 8) lets holders re-mint the send cap into CSpace
         * slots; it adds no receive authority. */
        *allowed  = RIGHT_WRITE | RIGHT_DUPLICATE;
        return 1;
    }

    {
        char base[SVCMGR_SERVICE_NAME_CAP];
        const struct iris_service_catalog_entry *manifest;
        struct svcmgr_service_state *svc;
        uint32_t len = 0;
        while (len < SVCMGR_SERVICE_NAME_CAP && name[len]) len++;
        for (uint32_t i = 0; i < len - 3u; i++) base[i] = name[i];
        base[len - 3u] = '\0';

        manifest = svcmgr_catalog_find_name(base);
        if (!manifest || !manifest->own_service_ep) return 0;
        svc = svcmgr_service_state(state, manifest->service_id);
        if (!svc || svc->ep_c == 0u) return 0;
        *out_cptr = svc->ep_c;
        /* Send/call side only; DUPLICATE (Fase 8) allows CSpace re-minting
         * (e.g. init mints vfs.ep/kbd.ep into iris_test's fixtures). */
        *allowed  = RIGHT_WRITE | RIGHT_DUPLICATE;
        return 1;
    }
}

static struct svcmgr_dynamic_service *svcmgr_dynamic_alloc_slot(struct svcmgr_state *state) {
    if (!state) return 0;
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        if (!state->dynamic[i].active)
            return &state->dynamic[i];
    }
    return 0;
}

static uint32_t svcmgr_dynamic_ready_count(const struct svcmgr_state *state) {
    uint32_t ready = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        if (state->dynamic[i].active &&
            (state->dynamic[i].public_h != HANDLE_INVALID ||
             state->dynamic[i].public_cptr != 0u))
            ready++;
    }
    return ready;
}

/* ── A1.6: receive-slot support ─────────────────────────────────────────
 *
 * svcmgr declares a receive-slot on every discovery-endpoint recv so a
 * REGISTER cap lands directly in its root CNode (CSpace-canonical storage,
 * no handle materialized at delivery).  Slots SVCMGR_RSLOT_BASE..LIMIT-1
 * are the registration pool — one per live CSpace-backed registration.
 * When the pool is exhausted (or the root CNode handle was not found) the
 * declaration degrades to 0 = legacy handle delivery, which keeps working.
 * ──────────────────────────────────────────────────────────────────────── */

#define SVCMGR_RSLOT_BASE  128u  /* below: well-known bootstrap, master and
                                  * device-cap slots (Stage 5 Etapa 2) */
#define SVCMGR_RSLOT_LIMIT 256u  /* root CNode has KCNODE_DEFAULT_SLOTS = 256 */

/* Stage 4: the root-CNode handle probe is RETIRED.  It scanned svcmgr's own
 * handle table for the first CNode-typed generation-1 id, relying on the
 * kernel inserting every process's root CNode as its FIRST handle.  The root
 * is now a structural back-reference on KProcess and lives in no handle table
 * at all, so there is nothing to find — and nothing to guess.  Every operation
 * that needs svcmgr's own root passes arg0 == 0, the "my own root CNode"
 * convention shared by SYS_CNODE_MINT / SYS_CNODE_DELETE / SYS_UNTYPED_RETYPE2. */
#define SVCMGR_OWN_ROOT_CNODE 0u

/* Pick the receive-slot to declare for the next recv: the first pool slot
 * not owned by a live CSpace-backed registration.  0 = declare nothing. */
static uint32_t svcmgr_next_recv_slot(const struct svcmgr_state *state) {
    uint8_t used[(SVCMGR_RSLOT_LIMIT - SVCMGR_RSLOT_BASE + 7u) / 8u] = {0};
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        uint32_t c = state->dynamic[i].public_cptr;
        if (state->dynamic[i].active &&
            c >= SVCMGR_RSLOT_BASE && c < SVCMGR_RSLOT_LIMIT)
            used[(c - SVCMGR_RSLOT_BASE) / 8u] |=
                (uint8_t)(1u << ((c - SVCMGR_RSLOT_BASE) % 8u));
    }
    for (uint32_t s = SVCMGR_RSLOT_BASE; s < SVCMGR_RSLOT_LIMIT; s++) {
        if (!(used[(s - SVCMGR_RSLOT_BASE) / 8u] &
              (uint8_t)(1u << ((s - SVCMGR_RSLOT_BASE) % 8u))))
            return s;
    }
    return 0;
}

/* Type of a delivered cap without consuming it.
 *
 * Stage 4: this used to materialize the CPtr into a handle (SYS_CSPACE_RESOLVE)
 * purely to ask SYS_HANDLE_TYPE what it was, then close the handle again — the
 * last productive use of the CSpace→handle bridge in the tree.  SYS_CAP_IDENTIFY
 * answers the same question about the slot itself, so the round trip (and the
 * transient handle-table entry it consumed on every delivered cap) is gone.
 *
 * The legacy leg remains only for a delivery that landed in the handle table
 * because the receiver declared no receive slot; it dies with that path. */
static int64_t svcmgr_delivered_cap_type(uint32_t v) {
    /* Stage 4: a delivered cap is a CPtr or nothing — handle materialisation
     * on delivery is retired, so the legacy leg has no input left. */
    return svcmgr_syscall1(SYS_CAP_IDENTIFY, (uint64_t)v);
}

/* Discard a delivered cap svcmgr will not keep: CNODE_DELETE for a CPtr
 * (frees the pool slot), close for a legacy handle.  No-op on no-cap. */
static void svcmgr_discard_delivered_cap(struct svcmgr_state *state, uint32_t v) {
    (void)state;   /* Stage 4: the root CNode is addressed by convention, not by a
                    * per-state handle; the parameter stays for call-site symmetry. */
    if (v == (uint32_t)IRIS_MSG_NO_CAP) return;
    if (iris_msg_cap_is_cptr(v)) {
        (void)svcmgr_syscall2(SYS_CNODE_DELETE,
                              SVCMGR_OWN_ROOT_CNODE, (uint64_t)v);
    } else {
        handle_id_t h = (handle_id_t)v;
        svcmgr_close_handle_if_valid(&h);
    }
}

static void svcmgr_dynamic_clear(struct svcmgr_dynamic_service *svc, int seal) {
    if (!svc || !svc->active) return;
    (void)seal;  /* Track I: dynamic masters are KEndpoints — always closed. */
    if (svc->public_h != HANDLE_INVALID)
        svcmgr_close_handle_if_valid(&svc->public_h);
    /* A1.6: release the CSpace-held master — the CNode slot owns its own
     * reference, so deleting the slot is the release; the pool slot becomes
     * declarable again on the next recv. */
    if (svc->public_cptr != 0u)
        (void)svcmgr_syscall2(SYS_CNODE_DELETE,
                              SVCMGR_OWN_ROOT_CNODE,
                              (uint64_t)svc->public_cptr);
    svc->public_cptr = 0u;
    svc->endpoint = 0;
    svc->client_rights = RIGHT_NONE;
    svc->active = 0;
    for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++) svc->name[i] = '\0';
}

/* svcmgr_reduce_lookup_rights retired — Fase 13/Track I */


/* Fase 13 (Track C): svcmgr_send_spawn_cap retired — the initrd spawn cap is
 * now delivered as the IRIS_CPTR_SPAWN_CAP pre-start mint. */

/* ── EP-based service discovery path ────────────────────────────────────
 *
 * svcmgr creates one KEndpoint and distributes it to all catalog services
 * during bootstrap.  Services call EP_CALL with IRIS_SVCMGR_EP_LOOKUP_NAME
 * to look up a service by name without the KChannel round-trip.
 *
 * The drain loop in the main body calls EP_NB_RECV periodically (after each
 * CHAN_RECV_TIMEOUT wakeup) to handle queued endpoint requests.
 * ──────────────────────────────────────────────────────────────────────── */

/* Receive buffer for bulk kbuf (service name) in EP_NB_RECV drain */
static uint8_t g_ep_recv_buf[IRIS_EP_SVCNAME_MAX];

/* Fase 10: dynamic (runtime-registered) service ids are reported as
 * SVCMGR_DYNAMIC_ID_BASE + slot_index so they never collide with the small
 * catalog ids (0..3) used by STATUS/RESTART. */
#define SVCMGR_DYNAMIC_ID_BASE 0x40u

/* Defined later; used by the EP DIAG handler (Fase 12). */
static uint32_t svcmgr_ready_service_count(const struct svcmgr_state *state);
static uint32_t svcmgr_active_slot_count(const struct svcmgr_state *state);

/* Liveness of a catalog service per the kernel (Fase 10 STATUS oracle). */
static int svcmgr_service_alive(struct svcmgr_state *state, uint32_t service_id) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, service_id);
    if (!svc || svc->proc_h == HANDLE_INVALID) return 0;
    return svcmgr_syscall1(SYS_PROCESS_STATUS, (uint64_t)svc->proc_h) == 1;
}

/* Resolve a name to its current {alive, generation} (Fase 10 STATUS).
 * Returns 1 on found. Handles ".ep"/catalog names and dynamic registrations. */
static int svcmgr_name_status(struct svcmgr_state *state, const char *name,
                              uint32_t *alive_out, uint32_t *gen_out) {
    char base[SVCMGR_SERVICE_NAME_CAP];
    const struct iris_service_catalog_entry *cat;
    struct svcmgr_dynamic_service *dyn;
    uint32_t len = 0;

    if (!state || !name) return 0;

    /* "<image>.ep" or bare catalog name → the catalog service behind it. */
    while (len < SVCMGR_SERVICE_NAME_CAP && name[len]) len++;
    if (svcmgr_name_has_ep_suffix(name) && len > 3u) {
        for (uint32_t i = 0; i < len - 3u; i++) base[i] = name[i];
        base[len - 3u] = '\0';
        cat = svcmgr_catalog_find_name(base);
    } else {
        cat = svcmgr_catalog_find_name(name);
    }
    if (cat) {
        struct svcmgr_service_state *svc = svcmgr_service_state(state, cat->service_id);
        if (gen_out)   *gen_out   = svc ? svc->generation : 0u;
        if (alive_out) *alive_out = (uint32_t)svcmgr_service_alive(state, cat->service_id);
        return 1;
    }

    dyn = svcmgr_dynamic_find_name(state, name);
    if (dyn && dyn->active) {
        if (gen_out)   *gen_out   = dyn->generation;
        if (alive_out) *alive_out = 1u;     /* registered ⇒ owner-alive by contract */
        return 1;
    }
    return 0;
}

static void svcmgr_handle_ep_request(struct svcmgr_state *state, struct IrisMsg *msg) {
    struct IrisMsg reply;
    uint32_t i;
    handle_id_t reply_h;

    if (!msg || msg->attached_handle == (uint32_t)IRIS_MSG_NO_CAP) return;
    reply_h = (handle_id_t)msg->attached_handle;

    /* A1.6: only REGISTER consumes a transferred cap.  A cap attached to any
     * other opcode used to leak into svcmgr's handle table (the delivered
     * handle was silently ignored); with receive-slots it would leak a pool
     * slot instead.  Discard it up front in both landing modes. */
    if (msg->label != IRIS_SVCMGR_EP_REGISTER &&
        msg->attached_cap != (uint32_t)IRIS_MSG_NO_CAP)
        svcmgr_discard_delivered_cap(state, msg->attached_cap);

    for (i = 0; i < (uint32_t)sizeof(reply); i++) ((uint8_t *)&reply)[i] = 0;

    switch (msg->label) {
    case IRIS_SVCMGR_EP_LOOKUP_NAME: {
        /* Name is in g_ep_recv_buf (set up before EP_NB_RECV call); NUL-terminate. */
        uint32_t namelen = msg->buf_len < IRIS_EP_SVCNAME_MAX
                           ? msg->buf_len : IRIS_EP_SVCNAME_MAX - 1u;
        g_ep_recv_buf[namelen] = '\0';

        handle_id_t master_h  = HANDLE_INVALID;
        iris_rights_t granted = RIGHT_NONE;
        /* Fase S4 (Etapa 2): when the master already lives in a CSpace slot we
         * mint straight from it — no handle is materialized at any point. */
        uint32_t    src_cptr  = 0u;

        /* Reserved "<name>.ep" endpoint names resolve first (Fase 7.1). */
        if (!svcmgr_resolve_ep_name(state, (const char *)g_ep_recv_buf,
                                    &master_h, &granted, &src_cptr)) {
            struct svcmgr_dynamic_service *dyn =
                svcmgr_dynamic_find_name(state, (const char *)g_ep_recv_buf);
            const struct iris_service_catalog_entry *cat =
                (!dyn) ? svcmgr_catalog_find_name((const char *)g_ep_recv_buf) : 0;

            if (dyn && dyn->public_cptr != 0u) {
                /* Fase S4: CSpace-backed registration — the source IS a slot;
                 * the A1.6 CSPACE_RESOLVE bridge is no longer needed here. */
                src_cptr = dyn->public_cptr;
                granted  = dyn->client_rights;
            } else if (dyn && dyn->public_h != HANDLE_INVALID) {
                master_h = dyn->public_h;
                granted  = dyn->client_rights;
            } else if (cat) {
                struct svcmgr_service_state *svc =
                    svcmgr_service_state(state, cat->service_id);
                if (svc && svc->public_h != HANDLE_INVALID) {
                    master_h = svc->public_h;
                    granted  = cat->client_service_rights;
                }
            }
        }

        if ((master_h != HANDLE_INVALID || src_cptr != 0u) && granted != RIGHT_NONE) {
            /* Fase 10 grant tightening: an ordinary client receives a
             * call-only cap (RIGHT_WRITE).  RIGHT_DUPLICATE/RIGHT_TRANSFER —
             * the authority to re-mint or hand the cap onward — is granted
             * ONLY to supervisor badges (init/svcmgr/unbadged bootstrap).
             * The legacy KChannel lookup path keeps the old wide grant for
             * bootstrap re-minting (T046). */
            iris_rights_t client_rights = granted;
            if (!iris_badge_is_supervisor(msg->sender_badge))
                client_rights &= ~(iris_rights_t)(RIGHT_DUPLICATE | RIGHT_TRANSFER);
            /* Fase S4 (Etapa 2): the transfer SOURCE is a CSpace slot, never a
             * handle.  Mint the master into svcmgr's scratch slot and hand the
             * CPtr to the kernel; the delivered cap becomes an MDB child of
             * that slot, so this grant is revocable from svcmgr.  The kernel
             * consumes (deletes) the scratch slot on a committed delivery. */
            (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, SVCMGR_XFER_SLOT);
            /* Stage 4: the registry master is a CSpace slot, so serving a
             * lookup is a slot-to-slot mint.  The SYS_CNODE_MINT branch for a
             * handle master is gone with the namespace. */
            int64_t mr = svcmgr_syscall3(SYS_CSPACE_MINT, (uint64_t)src_cptr,
                                         ((uint64_t)SVCMGR_XFER_SLOT << 32),
                                         (uint64_t)(client_rights | RIGHT_TRANSFER));
            if (mr == 0) {
                reply.label              = IRIS_EP_REPLY_OK;
                reply.words[0]           = 0u;
                reply.attached_handle    = SVCMGR_XFER_SLOT;
                reply.attached_rights    = (uint32_t)client_rights;
                svcmgr_log(sm_str_lookup_name_ok);
            } else {
                reply.label    = IRIS_EP_REPLY_ERR;
                reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_NO_MEMORY;
            }
        } else {
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND;
            svcmgr_log(sm_str_lookupfail);
        }
        break;
    }
    case IRIS_EP_OP_PING:
        /* Health check (Fase 8: also the CPtr-first discovery probe).
         * Fase 9 PING convention: echo the kernel-stamped sender badge. */
        reply.label      = IRIS_EP_REPLY_OK;
        reply.words[0]   = 0u;
        reply.words[1]   = msg->sender_badge;
        reply.word_count = 2u;
        break;
    case IRIS_SVCMGR_EP_STATUS: {
        /* Fase 10: read-only liveness/generation oracle. Name in kbuf.
         * Open to any caller — it is how a client polls a restart without
         * blocking on a possibly-dead endpoint. */
        uint32_t nl = msg->buf_len < IRIS_EP_SVCNAME_MAX
                      ? msg->buf_len : IRIS_EP_SVCNAME_MAX - 1u;
        uint32_t alive = 0u, gen = 0u;
        g_ep_recv_buf[nl] = '\0';
        if (svcmgr_name_status(state, (const char *)g_ep_recv_buf, &alive, &gen)) {
            reply.label      = IRIS_EP_REPLY_OK;
            reply.words[0]   = alive;
            reply.words[1]   = gen;
            reply.word_count = 2u;
            /* Fase 24 (additive): for a catalog service, expose the explicit
             * supervision policy so a supervisor/test can audit it without
             * reading svcmgr internals.  The IPC message carries only 4 words
             * (IRIS_MSG_WORDS), so the policy is packed:
             *   words[2] = supervision class
             *   words[3] = restart_count | (restart_limit<<8) | (degraded<<16)
             * Legacy callers reading only words[0..1] are unaffected. */
            {
                char base2[SVCMGR_SERVICE_NAME_CAP];
                const struct iris_service_catalog_entry *pc = 0;
                uint32_t l2 = 0;
                while (l2 < SVCMGR_SERVICE_NAME_CAP && g_ep_recv_buf[l2]) l2++;
                if (svcmgr_name_has_ep_suffix((const char *)g_ep_recv_buf) && l2 > 3u) {
                    for (uint32_t k = 0; k < l2 - 3u; k++) base2[k] = g_ep_recv_buf[k];
                    base2[l2 - 3u] = '\0';
                    pc = svcmgr_catalog_find_name(base2);
                } else {
                    pc = svcmgr_catalog_find_name((const char *)g_ep_recv_buf);
                }
                if (pc) {
                    struct svcmgr_service_state *ps = svcmgr_service_state(state, pc->service_id);
                    uint32_t rc  = ps ? (uint32_t)ps->restart_count : 0u;
                    uint32_t deg = ps ? (uint32_t)ps->degraded : 0u;
                    reply.words[2]   = (uint64_t)pc->supervision;
                    reply.words[3]   = (uint64_t)((rc & 0xFFu) |
                                       (((uint32_t)pc->restart_limit & 0xFFu) << 8) |
                                       ((deg & 0x1u) << 16));
                    reply.word_count = 4u;
                }
            }
        } else {
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND;
        }
        break;
    }
    case IRIS_SVCMGR_EP_DIAG: {
        /* Fase 12: endpoint-native snapshot — the productive diagnostics path
         * (replaces legacy KChannel SVCMGR_MSG_DIAG). No KChannel round-trip. */
        reply.label      = IRIS_EP_REPLY_OK;
        reply.words[0]   = (uint64_t)iris_service_catalog_count();
        reply.words[1]   = (uint64_t)svcmgr_ready_service_count(state);
        reply.words[2]   = (uint64_t)svcmgr_active_slot_count(state);
        reply.words[3]   = (uint64_t)IRIS_SERVICE_CATALOG_VERSION;
        reply.word_count = 4u;
        break;
    }
    case IRIS_SVCMGR_EP_RESTART: {
        /* Fase 10 PRIVILEGED: supervisor badges only. Kills the service; the
         * existing SYS_PROCESS_WATCH path respawns it and bumps generation. */
        uint32_t sid = (uint32_t)msg->words[0];
        const struct iris_service_catalog_entry *m =
            iris_service_catalog_find_by_service_id(sid);
        struct svcmgr_service_state *svc = svcmgr_service_state(state, sid);
        if (!iris_badge_is_supervisor(msg->sender_badge)) {
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_ACCESS_DENIED;
        } else if (!m || !svc || svc->proc_h == HANDLE_INVALID) {
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND;
        } else {
            reply.label      = IRIS_EP_REPLY_OK;
            reply.words[0]   = svc->generation;   /* caller polls STATUS for +1 */
            reply.word_count = 1u;
            (void)svcmgr_syscall1(SYS_PROCESS_KILL, (uint64_t)svc->proc_h);
        }
        break;
    }
    case IRIS_SVCMGR_EP_REGISTER: {
        /* Fase 11 cap-backed registration: the caller transfers its service
         * endpoint in attached_cap (kernel-delivered, never a forgeable
         * number) and svcmgr stores it so LOOKUP returns a usable cap.
         * A1.6: the drain loop declares a receive-slot, so the cap normally
         * lands in svcmgr's root CNode (attached_cap < 1024 = the CPtr) and
         * is stored CSpace-canonically; a legacy-handle landing (>= 1024:
         * pool exhausted, no root CNode, or the TOCTOU fallback) keeps the
         * old handle storage.  Badge-authenticated (sender_badge →
         * owner_badge); reserved names are rejected first (catalog + ".ep");
         * a cap is required. */
        uint32_t nl = msg->buf_len < IRIS_EP_SVCNAME_MAX
                      ? msg->buf_len : IRIS_EP_SVCNAME_MAX - 1u;
        const char *nm   = (const char *)g_ep_recv_buf;
        uint32_t cap_v   = msg->attached_cap;
        iris_error_t rej  = IRIS_OK;
        g_ep_recv_buf[nl] = '\0';

        if (nl == 0u || svcmgr_name_has_ep_suffix(nm) || svcmgr_name_is_catalog(nm))
            rej = IRIS_ERR_ACCESS_DENIED;            /* reserved name (T061) */
        else if (cap_v == (uint32_t)IRIS_MSG_NO_CAP)
            rej = IRIS_ERR_INVALID_ARG;              /* a cap is required */
        else if (svcmgr_delivered_cap_type(cap_v) !=
                 (int64_t)IRIS_HANDLE_TYPE_ENDPOINT)
            rej = IRIS_ERR_INVALID_ARG;              /* must be an endpoint */
        else if (svcmgr_dynamic_find_name(state, nm))
            rej = IRIS_ERR_BUSY;

        struct svcmgr_dynamic_service *slot =
            (rej == IRIS_OK) ? svcmgr_dynamic_alloc_slot(state) : 0;
        if (rej == IRIS_OK && !slot) rej = IRIS_ERR_NO_MEMORY;

        if (rej != IRIS_OK) {
            /* No leak on reject: frees the CSpace pool slot or closes the
             * legacy handle. */
            svcmgr_discard_delivered_cap(state, cap_v);
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)rej;
        } else {
            svcmgr_copy_name(slot->name, nm);
            slot->endpoint      = SVCMGR_DYNAMIC_ID_BASE +
                                  (uint32_t)(slot - state->dynamic);
            if (iris_msg_cap_is_cptr(cap_v)) {
                slot->public_cptr = cap_v;           /* CSpace-canonical master */
                slot->public_h    = HANDLE_INVALID;
            } else {
                slot->public_h    = (handle_id_t)cap_v; /* legacy handle master */
                slot->public_cptr = 0u;
            }
            slot->client_rights = RIGHT_WRITE;
            slot->owner_badge   = msg->sender_badge;
            slot->generation    = 1u;
            slot->active        = 1u;
            reply.label      = IRIS_EP_REPLY_OK;
            reply.words[0]   = slot->endpoint;       /* dynamic service id */
            reply.word_count = 1u;
        }
        break;
    }
    case IRIS_SVCMGR_EP_UNREGISTER: {
        /* Fase 10 badge-authenticated: only the owner badge (or a supervisor)
         * may unregister. words[0] = dynamic id from REGISTER. */
        uint32_t did = (uint32_t)msg->words[0];
        struct svcmgr_dynamic_service *slot = 0;
        if (did >= SVCMGR_DYNAMIC_ID_BASE &&
            (did - SVCMGR_DYNAMIC_ID_BASE) < SVCMGR_DYNAMIC_SERVICE_CAP)
            slot = &state->dynamic[did - SVCMGR_DYNAMIC_ID_BASE];
        if (!slot || !slot->active) {
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_NOT_FOUND;
        } else if (msg->sender_badge != slot->owner_badge &&
                   !iris_badge_is_supervisor(msg->sender_badge)) {
            reply.label    = IRIS_EP_REPLY_ERR;
            reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_ACCESS_DENIED;
        } else {
            svcmgr_dynamic_clear(slot, 0);
            reply.label = IRIS_EP_REPLY_OK;
        }
        break;
    }
    default:
        reply.label    = IRIS_EP_REPLY_ERR;
        reply.words[0] = (uint64_t)(uint32_t)IRIS_ERR_INVALID_ARG;
        break;
    }

    {
        int64_t rr = svcmgr_syscall2(SYS_REPLY, (uint64_t)reply_h,
                                     (uint64_t)(uintptr_t)&reply);
        /* Reply-cap contract (SYS_REPLY, Fase 7.1): on success the attached
         * dup is consumed; on IRIS_ERR_NOT_FOUND it was staged and destroyed.
         * Any other error happens before staging — close the dup here so a
         * failed reply does not leak the looked-up cap into svcmgr's table. */
        if (rr != IRIS_OK && rr != (int64_t)IRIS_ERR_NOT_FOUND &&
            reply.attached_handle != (uint32_t)IRIS_MSG_NO_CAP) {
            handle_id_t orphan = (handle_id_t)reply.attached_handle;
            svcmgr_close_handle_if_valid(&orphan);
        }
    }
    /* A1.6: the CSpace slot keeps the authority; the resolved master was a
     * per-request working handle only.  Fase S1: reply_h is svcmgr's OWN
     * reusable reply-object CPtr — never closed. */
}

static int64_t svcmgr_bootstrap_child(struct svcmgr_state *state,
                                      const struct iris_service_catalog_entry *manifest,
                                      handle_id_t child_boot_h) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, manifest->service_id);

    if (!svc) return IRIS_ERR_INVALID_ARG;

    /* Fase 13 (Track C): every bootstrap cap is now a pre-start CSpace mint
     * (svcmgr_build_core_mints) — the well-known endpoints (Fase 8), the vfs
     * spawn cap (C1) and the kbd service/reply KChannels + KIoPort/KIrqCap (C2).
     * Nothing is sent over the bootstrap KChannel anymore; svcmgr just drops its
     * end.  The channel itself is retired in a later increment (Track C3). */
    svcmgr_close_handle_if_valid(&child_boot_h);
    return IRIS_OK;
}

/*
 * Fase 8: build the well-known CPtr mint table for a catalog child (see
 * endpoint_proto.h for the layout).  Slots 1..4 carry the client side of
 * the core service endpoints (RIGHT_WRITE); slot 5 the child's OWN
 * endpoint recv side; slot 7 the IRQ KNotification WAIT side.  The table
 * is consumed by svc_load_minted, which mints BEFORE the child's first
 * thread starts — no bootstrap-message barrier is needed.
 */
#define SVCMGR_CORE_MINT_MAX 12u  /* Fase S1: +2 reply-object mints (slots 13/14) */
static uint32_t svcmgr_build_core_mints(struct svcmgr_state *state,
                                        const struct iris_service_catalog_entry *manifest,
                                        struct svc_mint *mints) {
    struct svcmgr_service_state *svc =
        svcmgr_service_state(state, manifest->service_id);
    uint32_t vfs_ep =
        (SVCMGR_SERVICE_VFS < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])))
        ? state->services[SVCMGR_SERVICE_VFS].ep_c : 0u;
    uint32_t kbd_ep =
        (SVCMGR_SERVICE_KBD < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])))
        ? state->services[SVCMGR_SERVICE_KBD].ep_c : 0u;
    uint32_t n = 0;

    /* Fase 9: the client-side slots (1..4) carry the CHILD's identity badge
     * — every message the child sends through them is kernel-stamped with
     * IRIS_BADGE_SVC(service_id).  Server-side caps (own EP recv, IRQ
     * notification) stay unbadged.
     *
     * Fase 22 (least authority): a slot is minted ONLY if the service's
     * client_eps manifest declares it needed.  Previously all four were minted
     * unconditionally, so kbd (a pure IRQ/endpoint driver) and vfs (which only
     * logs to console) each held WRITE caps to peers they never call — a
     * compromised driver could spoof requests to them.  Now kbd receives none,
     * vfs only console, sh all four; the delivery is verified by T156/T162. */
    uint64_t child_badge = IRIS_BADGE_SVC(manifest->service_id);

    if (manifest->client_eps & IRIS_SVC_CLIENT_EP_SVCMGR) {
        mints[n].slot = IRIS_CPTR_SVCMGR_EP;
        mints[n].src_cptr = state->ep_c;
        mints[n].rights = RIGHT_WRITE;
        mints[n].badge = child_badge;
        n++;
    }
    if (manifest->client_eps & IRIS_SVC_CLIENT_EP_VFS) {
        mints[n].slot = IRIS_CPTR_VFS_EP;
        mints[n].src_cptr = vfs_ep;
        mints[n].rights = RIGHT_WRITE;
        mints[n].badge = child_badge;
        n++;
    }
    if (manifest->client_eps & IRIS_SVC_CLIENT_EP_CONSOLE) {
        mints[n].slot = IRIS_CPTR_CONSOLE_EP;
        mints[n].src_cptr = state->console_ep_c;
        mints[n].rights = RIGHT_WRITE;
        mints[n].badge = child_badge;
        n++;
    }
    if (manifest->client_eps & IRIS_SVC_CLIENT_EP_KBD) {
        mints[n].slot = IRIS_CPTR_KBD_EP;
        mints[n].src_cptr = kbd_ep;
        mints[n].rights = RIGHT_WRITE;
        mints[n].badge = child_badge;
        n++;
    }
    if (manifest->own_service_ep && svc) {
        mints[n].slot = IRIS_CPTR_OWN_EP;
        mints[n].src_cptr = svc->ep_c;
        mints[n].rights = RIGHT_READ;
        mints[n].badge = 0;
        n++;
    }
    if (manifest->irq_notify && svc) {
        mints[n].slot = IRIS_CPTR_IRQ_NOTIFY;
        mints[n].src_cptr = svc->irq_notif_c;
        mints[n].rights = RIGHT_WAIT;
        mints[n].badge = 0;
        n++;
    }
    /* Fase 13 (Track C): the initrd-access spawn cap (vfs) arrives as a
     * pre-start CSpace mint instead of a post-spawn KChannel INITRD_CAP
     * message.  RIGHT_READ matches the legacy delivery; unbadged. */
    if (manifest->give_spawn_cap && state->spawn_cap_c != 0u) {
        mints[n].slot = IRIS_CPTR_SPAWN_CAP;
        mints[n].src_cptr = state->spawn_cap_c;
        mints[n].rights = RIGHT_READ;
        mints[n].badge = 0;
        n++;
    }
    /* Fase 13 (Track I): the legacy service/reply KChannel pair (IRIS_CPTR_SVC_CHAN
     * / SVC_REPLY) is retired — every catalog service is endpoint_only, so no
     * service-channel mint is emitted.  Device caps (KIoPort/KIrqCap) below. */
    if (manifest->ioport_count > 0u &&
        manifest->service_id < SVCMGR_IOPORT_CAPS_TABLE_SIZE &&
        state->ioport_caps[manifest->service_id] != 0u) {
        mints[n].slot = IRIS_CPTR_IOPORT;
        /* Fase S4: CSpace source — the child's device cap is an MDB child of
         * svcmgr's slot, so the delegation is revocable. */
        mints[n].src_cptr = state->ioport_caps[manifest->service_id];
        mints[n].rights = RIGHT_READ;
        mints[n].badge = 0;
        n++;
    }
    if (manifest->give_irqcap && manifest->irq_num != 0xFFu &&
        manifest->irq_num < SVCMGR_IRQ_CAPS_TABLE_SIZE &&
        state->irq_caps[manifest->irq_num] != 0u) {
        mints[n].slot = IRIS_CPTR_IRQ_CAP;
        mints[n].src_cptr = state->irq_caps[manifest->irq_num];
        mints[n].rights = RIGHT_ROUTE;
        mints[n].badge = 0;
        n++;
    }
    return n;
}

/* int64_t svcmgr_send_lookup_reply retired — Fase 13/Track I (legacy KChannel LOOKUP / bootstrap recv). */


static uint32_t svcmgr_ready_service_count(const struct svcmgr_state *state) {
    uint32_t ready = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])); i++) {
        const struct iris_service_catalog_entry *manifest =
            iris_service_catalog_find_by_service_id(i);
        if (manifest && manifest->endpoint_only) {
            /* Fase 7.5: endpoint_only services have no legacy pair; their
             * KEndpoint is the readiness entry point.  Fase 8: pure-client
             * services (endpoint_only without an own endpoint, e.g. sh) are
             * ready when their process is alive. */
            if (manifest->own_service_ep) {
                if (state->services[i].ep_c != 0u) ready++;
            } else {
                if (state->services[i].proc_h != HANDLE_INVALID) ready++;
            }
            continue;
        }
        if (state->services[i].public_h != HANDLE_INVALID &&
            state->services[i].reply_h != HANDLE_INVALID) ready++;
    }
    return ready + svcmgr_dynamic_ready_count(state);
}

static uint32_t svcmgr_active_slot_count(const struct svcmgr_state *state) {
    uint32_t active = 0;
    if (!state) return 0;
    for (uint32_t i = 0;
         i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0]));
         i++) {
        if (state->services[i].proc_h != HANDLE_INVALID) active++;
    }
    return active;
}

/* Fase 7.5: VFS health is queried over the stateless endpoint
 * (VFS_EP_OP_STATUS on the master ep cap svcmgr already holds). The
 * stateless protocol has no open-file table, so opens/capacity report 0. */
static int svcmgr_track_spawn(struct svcmgr_state *state,
                              const struct iris_service_catalog_entry *manifest,
                              handle_id_t proc_h, handle_id_t public_h) {
    struct svcmgr_service_state *svc;
    const char *service_name = manifest ? manifest->image_name : 0;

    if (!service_name) {
        svcmgr_log(sm_str_svc_unknown);
        svcmgr_close_handle_if_valid(&proc_h);
        return 0;
    }
    svc = svcmgr_service_state(state, manifest->service_id);
    if (!svc) {
        svcmgr_log(sm_str_spawnfail);
        svcmgr_close_handle_if_valid(&proc_h);
        return 0;
    }
    svc->proc_h = proc_h;

    if (manifest->irq_num != 0xFFu) {
        /* Fase S4: the IRQ cap is a CPtr slot; SYS_IRQ_ROUTE_REGISTER resolves
         * it through the CSpace leg of its dual resolver. */
        uint32_t irqcap_c = (manifest->irq_num < SVCMGR_IRQ_CAPS_TABLE_SIZE)
                                ? state->irq_caps[manifest->irq_num]
                                : 0u;
        handle_id_t route_h = public_h;
        if (manifest->irq_notify) {
            /* Fase 7.6: IRQ → KNotification. Created once, reused across
             * restarts so the kernel route only needs re-registering. */
            if (svc->irq_notif_c == 0u) {
                uint32_t sl = SVCMGR_MSLOT_IRQ(manifest->service_id);
                int64_t nr = svcmgr_retype_to_slot(state->untyped_c,
                                                   IRIS_KOBJ_NOTIFICATION, sl, 0);
                svc->irq_notif_c = (nr >= 0) ? sl : 0u;
            }
            route_h = (handle_id_t)svc->irq_notif_c;
        }
        if (irqcap_c == 0u || route_h == HANDLE_INVALID ||
            svcmgr_syscall3(SYS_IRQ_ROUTE_REGISTER, irqcap_c, route_h, proc_h) < 0) {
            svc->proc_h = HANDLE_INVALID;
            svcmgr_close_handle_if_valid(&proc_h);
            svcmgr_log(sm_str_irqfail);
            return 0;
        }
    }

    if (svcmgr_syscall3(SYS_PROCESS_WATCH, proc_h, state->death_notif_c,
                        (uint64_t)1u << manifest->service_id) != IRIS_OK) {
        svc->proc_h = HANDLE_INVALID;
        svcmgr_close_handle_if_valid(&proc_h);
        svcmgr_log(sm_str_spawnfail);
        return 0;
    }

    /* Fase 10: first successful boot establishes generation 1; restarts bump
     * it in svcmgr_handle_service_death before re-entering svcmgr_boot_service. */
    if (svc->generation == 0u)
        svc->generation = 1u;

    svcmgr_log(sm_str_watchok);
    svcmgr_log(sm_str_spawnok);
    return 1;
}

static void svcmgr_boot_service(struct svcmgr_state *state,
                                const struct iris_service_catalog_entry *manifest) {
    struct svcmgr_service_state *svc;
    handle_id_t child_boot_h = HANDLE_INVALID;
    int64_t proc_h;

    if (!manifest) {
        svcmgr_log(sm_str_svc_unknown);
        return;
    }

    svc = svcmgr_service_state(state, manifest->service_id);
    if (!svc) {
        svcmgr_log(sm_str_spawnfail);
        return;
    }

    svcmgr_clear_service_masters(state, manifest->service_id);

    /* Fase 7.1: create the service's KEndpoint once; it survives restarts
     * (clear_service_masters does not touch ep_h) so client caps obtained
     * via "<name>.ep" lookup stay valid across a respawn. Non-fatal. */
    if (manifest->own_service_ep && svc->ep_c == 0u) {
        uint32_t sl = SVCMGR_MSLOT_EP(manifest->service_id);
        int64_t ep_r = svcmgr_retype_to_slot(state->untyped_c,
                                             IRIS_KOBJ_ENDPOINT, sl, 0);
        svc->ep_c = (ep_r >= 0) ? sl : 0u;
    }

    /* Fase 7.6: the IRQ KNotification must exist BEFORE bootstrap caps are
     * sent (the WAIT side ships with them); the kernel route is registered
     * later in track_spawn. Created once, survives restarts. */
    if (manifest->irq_notify && svc->irq_notif_c == 0u) {
        uint32_t sl = SVCMGR_MSLOT_IRQ(manifest->service_id);
        int64_t nr = svcmgr_retype_to_slot(state->untyped_c,
                                           IRIS_KOBJ_NOTIFICATION, sl, 0);
        svc->irq_notif_c = (nr >= 0) ? sl : 0u;
    }

    /* Fase 13 (Track I): every catalog service is endpoint_only now — the
     * legacy service/reply KChannel pair is fully retired (no SYS_CHAN_CREATE).
     * Each service's KEndpoint (+ IRQ notification for kbd) is its whole
     * surface. */

    if (!manifest->image_name) {
        svcmgr_clear_service_masters(state, manifest->service_id);
        svcmgr_log(sm_str_svc_unknown);
        return;
    }

    {
        handle_id_t loaded_proc_h = HANDLE_INVALID;
        handle_id_t loaded_chan_h = HANDLE_INVALID;
        /* Fase 8: CPtr-first handoff — the well-known slots (discovery +
         * core service eps + own ep + irq notify) are minted into the
         * child's root CNode BEFORE its first thread starts, so even a
         * bag-less child (sh) finds them populated deterministically. */
        struct svc_mint mints[SVCMGR_CORE_MINT_MAX] = { 0 };
        uint32_t mint_count = svcmgr_build_core_mints(state, manifest, mints);

        /* Fase S1: fresh explicit reply object(s) for a serving child, funded
         * by the per-service reply sub-untyped.  On a restart the previous
         * reply objects died with the child, so the RESET reclaims the whole
         * 4 KiB region before retyping (BUSY = something still lives there —
         * fall through and carve from the remaining space).  kbd parks one
         * reply while continuing to serve, so it receives TWO (slots 13/14).
         * svcmgr closes its own handles right after the mint: a retained
         * reply cap would suppress close-wakes-caller on child death. */
        uint32_t reply1_c = 0u;
        uint32_t reply2_c = 0u;
        if (manifest->own_service_ep && state->untyped_c != 0u) {
            if (svc->reply_ut_c == 0u) {
                uint32_t sl = SVCMGR_MSLOT_REPLYUT(manifest->service_id);
                int64_t ur = svcmgr_retype_to_slot(state->untyped_c,
                                                   IRIS_KOBJ_UNTYPED, sl, 4096u);
                svc->reply_ut_c = (ur >= 0) ? sl : 0u;
            }
            if (svc->reply_ut_c != 0u) {
                (void)svcmgr_syscall1(SYS_UNTYPED_RESET, (uint64_t)svc->reply_ut_c);
                int64_t r1 = svcmgr_retype_to_slot(svc->reply_ut_c, IRIS_KOBJ_REPLY,
                                                   SVCMGR_SLOT_REPLY1, 0);
                reply1_c = (r1 >= 0) ? SVCMGR_SLOT_REPLY1 : 0u;
                if (manifest->irq_notify) { /* kbd: parked-reply second slot */
                    int64_t r2 = svcmgr_retype_to_slot(svc->reply_ut_c, IRIS_KOBJ_REPLY,
                                                       SVCMGR_SLOT_REPLY2, 0);
                    reply2_c = (r2 >= 0) ? SVCMGR_SLOT_REPLY2 : 0u;
                }
            }
            if (reply1_c != 0u && mint_count < SVCMGR_CORE_MINT_MAX) {
                mints[mint_count].slot   = (uint32_t)IRIS_CPTR_OWN_REPLY;
                mints[mint_count].src_cptr = reply1_c;
                mints[mint_count].rights = RIGHT_READ | RIGHT_WRITE;
                mints[mint_count].badge  = 0;
                mint_count++;
            }
            if (reply2_c != 0u && mint_count < SVCMGR_CORE_MINT_MAX) {
                mints[mint_count].slot   = (uint32_t)IRIS_CPTR_OWN_REPLY2;
                mints[mint_count].src_cptr = reply2_c;
                mints[mint_count].rights = RIGHT_READ | RIGHT_WRITE;
                mints[mint_count].badge  = 0;
                mint_count++;
            }
        }

        long r = svc_load_minted_ws((handle_id_t)state->spawn_cap_c, manifest->image_name,
                                 &loaded_proc_h, &loaded_chan_h,
                                 mints, mint_count,
                               SVC_LOADER_WS(state->untyped_c, 66u));
        /* Drop svcmgr's reply handles regardless of the load result — the
         * child's CSpace slots (if minted) are now the only reply caps. */
        /* svcmgr NEVER retains a reply cap: a retained copy would suppress
         * close-wakes-caller on child death.  The child's mint is the only one. */
        (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, SVCMGR_SLOT_REPLY1);
        (void)svcmgr_syscall2(SYS_CNODE_DELETE, 0, SVCMGR_SLOT_REPLY2);
        if (r < 0) {
            svcmgr_clear_service_masters(state, manifest->service_id);
            svcmgr_log(sm_str_spawnfail);
            return;
        }
        proc_h       = (int64_t)loaded_proc_h;
        child_boot_h = loaded_chan_h;
    }

    if (svcmgr_bootstrap_child(state, manifest, child_boot_h) == IRIS_OK) {
        svcmgr_log(sm_str_bootok);
    } else {
        svcmgr_close_handle_if_valid(&child_boot_h);
        svcmgr_log(sm_str_bootfail);
    }

    if (!svcmgr_track_spawn(state, manifest, (handle_id_t)proc_h, HANDLE_INVALID)) {
        svcmgr_clear_service_masters(state, manifest->service_id);
    }
}

static void svcmgr_autostart_services(struct svcmgr_state *state) {
    for (uint32_t i = 0; i < iris_service_catalog_count(); i++) {
        const struct iris_service_catalog_entry *desc = iris_service_catalog_at(i);
        if (!desc || !desc->autostart) continue;
        svcmgr_boot_service(state, desc);
    }
}

/* void svcmgr_handle_lookup retired — Fase 13/Track I (legacy KChannel LOOKUP / bootstrap recv). */

/* void svcmgr_handle_lookup_name retired — Fase 13/Track I (legacy KChannel LOOKUP / bootstrap recv). */

static void svcmgr_release_service(struct svcmgr_state *state,
                                   uint32_t service_id,
                                   struct svcmgr_service_state *svc) {
    const struct iris_service_catalog_entry *manifest;
    if (!svc || svc->proc_h == HANDLE_INVALID) return;

    manifest = iris_service_catalog_find_by_service_id(service_id);
    svcmgr_log("[SVCMGR] service exited: ");
    if (manifest && manifest->image_name) svcmgr_log(manifest->image_name);
    svcmgr_log("\n");
    svcmgr_clear_service_masters(state, service_id);
    svcmgr_close_handle_if_valid(&svc->proc_h);
}

/* Track B: the dead service is named directly by the signalled bit index. */
static void svcmgr_handle_service_death(struct svcmgr_state *state, uint32_t service_id) {
    const struct iris_service_catalog_entry *manifest;
    struct svcmgr_service_state *svc;

    manifest = iris_service_catalog_find_by_service_id(service_id);
    svc      = svcmgr_service_state(state, service_id);

    /* Only act once per death: a still-armed watch has a live proc_h. A bit
     * for an already-released (or never-booted) slot is ignored. */
    if (!svc || svc->proc_h == HANDLE_INVALID) return;

    svcmgr_release_service(state, service_id, svc);

    if (!svcmgr_should_restart_service(state, manifest)) {
        if (manifest && manifest->restart_on_exit) {
            /* Fase 24: restart budget exhausted — the service stays down and is
             * marked degraded (observable via STATUS words[5]).  A restartable
             * service never revives past its limit without explicit action. */
            svc->degraded = 1u;
            svcmgr_log(sm_str_restart_exhausted);
            if (manifest->image_name) svcmgr_log(manifest->image_name);
            svcmgr_log("\n");
        }
        return;
    }

    svc = svcmgr_service_state(state, service_id);
    if (!svc) return;
    svc->restart_count++;
    /* Fase 10: death→respawn bumps the generation so any client holding a cap
     * to the previous instance can detect the change via STATUS and relookup. */
    svc->generation++;

    svcmgr_log(sm_str_restart);
    if (manifest->image_name) svcmgr_log(manifest->image_name);
    svcmgr_log(" (");
    svcmgr_log_u32(svc->restart_count);
    svcmgr_log("/");
    svcmgr_log_u32((uint32_t)manifest->restart_limit);
    svcmgr_log(")\n");

    svcmgr_boot_service(state, manifest);
}

void svcmgr_main_c(handle_id_t rbx_unused) {
    struct svcmgr_state *state = &g_svcmgr_state;

    /* Fase 13 (Track I): the entry bootstrap KChannel is gone — every cap is a
     * pre-start CSpace mint, and svc_loader passes RBX = 0.  This argument has
     * not been a handle since; closing it was closing handle 0. */
    (void)rbx_unused;

    for (uint32_t i = 0; i < (uint32_t)sizeof(*state); i++) ((uint8_t *)state)[i] = 0;

    state->spawn_cap_c = 0u;
    state->console_ep_c = 0u;
    state->ep_c        = 0u;
    state->death_notif_c = 0u;
    for (uint32_t i = 0; i < SVCMGR_IRQ_CAPS_TABLE_SIZE; i++)
        state->irq_caps[i] = 0u;
    for (uint32_t i = 0; i < SVCMGR_IOPORT_CAPS_TABLE_SIZE; i++)
        state->ioport_caps[i] = 0u;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])); i++) {
        state->services[i].public_h = HANDLE_INVALID;
        state->services[i].reply_h = HANDLE_INVALID;
        state->services[i].proc_h = HANDLE_INVALID;
        state->services[i].ep_c = 0u;
        state->services[i].irq_notif_c = 0u;
        state->services[i].reply_ut_c = 0u;
    }
    state->untyped_c = 0u;
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        state->dynamic[i].endpoint = 0;
        state->dynamic[i].public_h = HANDLE_INVALID;
        state->dynamic[i].public_cptr = 0u;
        state->dynamic[i].client_rights = RIGHT_NONE;
        state->dynamic[i].active = 0;
        for (uint32_t j = 0; j < SVCMGR_SERVICE_NAME_CAP; j++) state->dynamic[i].name[j] = '\0';
    }

    svcmgr_log(sm_str_started);

    /* Fase 13 (Track I): every bootstrap cap is a pre-start CSpace mint from
     * init — no bootstrap KChannel.  Resolve the well-known slots to handles:
     *   slot 3 (CONSOLE_EP) — console.ep send side (log + re-mint to children);
     *   slot 5 (OWN_EP)     — svcmgr's discovery endpoint recv+mint side,
     *                         published as "svcmgr.ep" and minted as
     *                         IRIS_CPTR_SVCMGR_EP into catalog children;
     *   slot 6 (SPAWN_CAP)  — spawn/authority cap (initrd, ioport/irq caps). */
    {
        state->console_ep_c = IRIS_CPTR_CONSOLE_EP;
        state->ep_c = IRIS_CPTR_OWN_EP;
        state->spawn_cap_c = IRIS_CPTR_SPAWN_CAP;
        /* Fase S1: the delegated untyped pool (init carved a sub-untyped and
         * minted it at slot 12).  Every EP/notification/reply svcmgr creates
         * is retyped from this pool.
         *
         * Etapa 4: confirmed with SYS_UNTYPED_INFO, which answers by CPtr and
         * materializes nothing — the pool stays a CPtr all the way into
         * retype2, which is what gives the fabricated objects a real MDB
         * ancestor instead of LEGACY_ROOT status. */
        int64_t ur = svcmgr_syscall3(SYS_UNTYPED_INFO, IRIS_CPTR_OWN_UNTYPED, 0, 0);
        state->untyped_c = (ur >= 0) ? (uint64_t)IRIS_CPTR_OWN_UNTYPED : 0u;

    }
    if (state->spawn_cap_c == 0u) {
        svcmgr_log(sm_str_bootcapfail);
        return;
    }
    if (state->untyped_c == 0u)
        svcmgr_log(sm_str_untypedfail);

    /* Fase S1: svcmgr's OWN reply object for the discovery endpoint, retyped
     * straight into root slot IRIS_CPTR_OWN_REPLY (dest 0 = own root). */
    if (state->untyped_c != 0u)
        (void)svcmgr_syscall4(SYS_UNTYPED_RETYPE2, state->untyped_c,
                              (uint64_t)IRIS_KOBJ_REPLY | (1ULL << 32),
                              ((uint64_t)IRIS_CPTR_OWN_REPLY << 32), 0);

    /* Drain kernel boot log to console over console.ep (Fase 13/Track I).
     * console_ep_write is a synchronous per-chunk flush barrier — every byte is
     * on the UART before EP_CALL returns — so no early kernel marker (e.g.
     * "boot vspace CSpace grants OK") can be dropped in an async send window.
     * Replaces the legacy console_h KChannel writer (no SYS_CHAN). */
    {
        static uint8_t klog_drain_buf[4097]; /* KLOG_BUF_SIZE + 1 for NUL */
        /* Etapa 4: name the capability that authorises the drain instead of
         * relying on the kernel finding a KDEBUG cap somewhere in our handle
         * table — which it cannot do now that our spawn cap is a CSpace slot.
         * Stage 5 Etapa 2: that capability is the debug control capability,
         * which authorises reading the kernel's log and nothing else. */
        int64_t n = svcmgr_syscall3(SYS_KLOG_DRAIN,
                                    (uint64_t)(uintptr_t)klog_drain_buf,
                                    4096u, IRIS_CPTR_DEBUG_CONTROL);
        if (n > 0 && state->console_ep_c != 0u) {
            klog_drain_buf[n] = 0u;
            (void)console_ep_write(state->console_ep_c, g_svcmgr_log_buf,
                                   (const char *)klog_drain_buf);
        }
    }

    svcmgr_request_hardware_caps(state);

    /* svcmgr's discovery endpoint (state->ep_c) is the IRIS_CPTR_OWN_EP mint
     * resolved above — no SYS_ENDPOINT_CREATE. */

    /* Fase 8: pre-create ALL service endpoint / IRQ-notification masters
     * before autostart, so the first child booted can already receive the
     * full well-known slot set (vfs.ep / kbd.ep exist before any spawn). */
    for (uint32_t ci = 0; ci < iris_service_catalog_count(); ci++) {
        const struct iris_service_catalog_entry *d = iris_service_catalog_at(ci);
        struct svcmgr_service_state *s;
        if (!d) continue;
        s = svcmgr_service_state(state, d->service_id);
        if (!s) continue;
        if (d->own_service_ep && s->ep_c == 0u) {
            uint32_t sl = SVCMGR_MSLOT_EP(d->service_id);
            int64_t r0 = svcmgr_retype_to_slot(state->untyped_c,
                                               IRIS_KOBJ_ENDPOINT, sl, 0);
            s->ep_c = (r0 >= 0) ? sl : 0u;
        }
        if (d->irq_notify && s->irq_notif_c == 0u) {
            uint32_t sl = SVCMGR_MSLOT_IRQ(d->service_id);
            int64_t r0 = svcmgr_retype_to_slot(state->untyped_c,
                                               IRIS_KOBJ_NOTIFICATION, sl, 0);
            s->irq_notif_c = (r0 >= 0) ? sl : 0u;
        }
    }

    /* Track B: the single death notification must exist before any service
     * boots (svcmgr_boot_service arms the watch against it). */
    {
        int64_t nr = svcmgr_retype_to_slot(state->untyped_c, IRIS_KOBJ_NOTIFICATION,
                                           SVCMGR_SLOT_DEATH_NOTIF, 0);
        state->death_notif_c = (nr >= 0) ? SVCMGR_SLOT_DEATH_NOTIF : 0u;
    }

    svcmgr_autostart_services(state);
    svcmgr_log(sm_str_ready);
    if (state->ep_c != 0u)
        svcmgr_log(sm_str_ep_ready);

    for (;;) {
        /* Drain all pending EP_CALL requests before blocking on KChannel. */
        while (state->ep_c != 0u) {
            struct IrisMsg ep_msg;
            int64_t ep_r;
            uint32_t k;
            for (k = 0; k < IRIS_EP_SVCNAME_MAX; k++) g_ep_recv_buf[k] = 0;
            {
                uint8_t *p = (uint8_t *)&ep_msg;
                for (k = 0; k < (uint32_t)sizeof(ep_msg); k++) p[k] = 0;
            }
            ep_msg.buf_uptr = (uint64_t)(uintptr_t)g_ep_recv_buf;
            /* A1.6: declare a registration receive-slot so a REGISTER cap
             * lands in the CSpace pool instead of the handle table.  0 (pool
             * exhausted / no root CNode) keeps legacy handle delivery. */
            iris_msg_declare_recv_slot(&ep_msg, svcmgr_next_recv_slot(state));
            /* Fase S1: our explicit reply object rides in recv arg2. */
            ep_r = svcmgr_syscall3(SYS_EP_NB_RECV, state->ep_c,
                                   (uint64_t)(uintptr_t)&ep_msg,
                                   IRIS_CPTR_OWN_REPLY);
            if (ep_r != IRIS_OK) break;
            svcmgr_handle_ep_request(state, &ep_msg);
        }

        /* Fase 13 (Track I): the legacy bootstrap KChannel is fully retired —
         * discovery is the cap-backed EP API (IRIS_SVCMGR_EP_LOOKUP_NAME, served
         * in svcmgr_handle_ep_request above) and death is a KNotification.
         * svcmgr has no productive SYS_CHAN. */

        /* Track B: block on the death notification (10ms) — this is the loop's
         * idle driver, replacing the old CHAN_RECV_TIMEOUT.  Each set bit is a
         * service exit; the bit index is the service_id. */
        {
            uint64_t bits = 0;
            int64_t nr = svcmgr_syscall3(SYS_NOTIFY_WAIT_TIMEOUT,
                                         state->death_notif_c,
                                         (uint64_t)(uintptr_t)&bits, 10000000ULL);
            if (nr == IRIS_OK) {
                for (uint32_t sid = 0; sid < 64u && bits; sid++) {
                    if (bits & ((uint64_t)1u << sid))
                        svcmgr_handle_service_death(state, sid);
                    bits &= ~((uint64_t)1u << sid);
                }
            } else if (nr != (int64_t)IRIS_ERR_TIMED_OUT) {
                svcmgr_log(sm_str_recverr);
            }
        }
    }
}
