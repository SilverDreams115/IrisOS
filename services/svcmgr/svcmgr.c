#include <iris/svcmgr_proto.h>
#include <iris/diag.h>
#include <iris/kbd_proto.h>
#include "service_catalog.h"
#include <iris/syscall.h>
#include <iris/vfs_proto.h>
#include <iris/nc/error.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <stdint.h>
#include "../common/svc_loader.h"
#include "../common/console_client.h"

/*
 * Runtime-published services are bounded by the same per-process handle-table
 * ceiling as the rest of the system, not by an arbitrary local cap.
 */
#define SVCMGR_DYNAMIC_SERVICE_CAP 1024u /* mirrors HANDLE_TABLE_MAX */
/* Indexed by IRQ number (0–15); mirrors KIRQCAP_POOL_SIZE in kirqcap.h. */
#define SVCMGR_IRQ_CAPS_TABLE_SIZE 16u
/* Indexed by service_id (0–2); mirrors IRIS_SERVICE_RUNTIME_SLOT_COUNT. */
#define SVCMGR_IOPORT_CAPS_TABLE_SIZE IRIS_SERVICE_RUNTIME_SLOT_COUNT

struct svcmgr_service_state {
    handle_id_t public_h;
    handle_id_t reply_h;
    handle_id_t proc_h;
    uint8_t restart_count;
    uint8_t reserved[3];
};

struct svcmgr_dynamic_service {
    uint32_t endpoint;
    handle_id_t public_h;
    iris_rights_t client_rights;
    char name[SVCMGR_SERVICE_NAME_CAP];
    uint8_t active;
};

struct svcmgr_state {
    handle_id_t bootstrap_h;
    handle_id_t spawn_cap_h;
    handle_id_t console_h;                                    /* write end of console channel */
    handle_id_t irq_caps[SVCMGR_IRQ_CAPS_TABLE_SIZE];       /* indexed by IRQ number  */
    handle_id_t ioport_caps[SVCMGR_IOPORT_CAPS_TABLE_SIZE]; /* indexed by service_id  */
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
static const char sm_str_bootdupfail[]  = "[SVCMGR] WARN: child bootstrap dup failed\n";
static const char sm_str_bootsendfail[] = "[SVCMGR] WARN: child bootstrap send failed\n";
static const char sm_str_lookupok[]     = "[SVCMGR] lookup reply OK\n";
static const char sm_str_lookupfail[]   = "[SVCMGR] WARN: lookup failed\n";
static const char sm_str_lookupsendfail[] = "[SVCMGR] WARN: lookup reply send failed\n";
static const char sm_str_diag_fail[]    = "[SVCMGR] WARN: diagnostics query failed\n";
static const char sm_str_svc_exited[]   = "[SVCMGR] service exited\n";
static const char sm_str_irqfail[]      = "[SVCMGR] WARN: irq route xfer failed\n";
static const char sm_str_svc_unknown[]  = "[SVCMGR] WARN: unknown bootstrap service\n";
static const char sm_str_watchok[]      = "[SVCMGR] lifecycle watch armed\n";
static const char sm_str_bootcapfail[]  = "[SVCMGR] FATAL: missing spawn capability\n";
static const char sm_str_restart[]           = "[SVCMGR] restarting ";
static const char sm_str_restart_exhausted[] = "[SVCMGR] WARN: restart budget exhausted: ";
static const char sm_str_register_ok[]       = "[SVCMGR] runtime publish OK\n";
static const char sm_str_register_fail[]     = "[SVCMGR] WARN: runtime publish failed\n";
static const char sm_str_register_full[]     = "[SVCMGR] WARN: runtime registry full\n";
static const char sm_str_lookup_name_ok[]    = "[SVCMGR] lookup-name reply OK\n";
static const char sm_str_unregister_ok[]     = "[SVCMGR] runtime withdraw OK\n";
static const char sm_str_unregister_fail[]   = "[SVCMGR] WARN: runtime withdraw failed\n";
static const char sm_str_diag_req[]          = "[SVCMGR][DIAG] request\n";
static const char sm_str_diag_snap[]         = "[SVCMGR][DIAG] kernel snapshot OK\n";
static const char sm_str_diag_vfs[]          = "[SVCMGR][DIAG] vfs status OK\n";
static const char sm_str_diag_kbd[]          = "[SVCMGR][DIAG] kbd status OK\n";
static const char sm_str_diag_done[]         = "[SVCMGR][DIAG] reply sent\n";

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

static void svcmgr_log(const char *msg) {
    console_write(g_svcmgr_state.console_h, msg);
}

static void svcmgr_log_u32(uint32_t value) {
    char buf[11];
    uint32_t i = 0;

    if (value == 0) {
        svcmgr_log("0");
        return;
    }

    while (value != 0 && i < (uint32_t)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (i != 0) {
        char out[2];
        out[0] = buf[--i];
        out[1] = '\0';
        svcmgr_log(out);
    }
}

static void svcmgr_log_u64(uint64_t value) {
    char buf[21];
    uint32_t i = 0;

    if (value == 0) {
        svcmgr_log("0");
        return;
    }

    while (value != 0 && i < (uint32_t)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (i != 0) {
        char out[2];
        out[0] = buf[--i];
        out[1] = '\0';
        svcmgr_log(out);
    }
}

static void svcmgr_log_hex8(uint32_t value) {
    char out[5];
    static const char digits[] = "0123456789ABCDEF";

    out[0] = '0';
    out[1] = 'x';
    out[2] = digits[(value >> 4) & 0xFu];
    out[3] = digits[value & 0xFu];
    out[4] = '\0';
    svcmgr_log(out);
}

static void svcmgr_close_handle_if_valid(handle_id_t *h) {
    if (!h || *h == HANDLE_INVALID) return;
    (void)svcmgr_syscall1(SYS_HANDLE_CLOSE, *h);
    *h = HANDLE_INVALID;
}

/*
 * Receive bootstrap messages from the bootstrap channel.
 * init sends two messages before svcmgr starts:
 *   1. SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP — write end of the console channel
 *   2. SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP   — hardware/spawn authority cap
 *
 * Returns 1 once SPAWN_CAP has been received (CONSOLE_CAP is optional;
 * if missing, console_h stays HANDLE_INVALID and svcmgr_log is silent).
 */
static int svcmgr_recv_bootstrap_caps(struct svcmgr_state *state) {
    uint32_t recv_count = 0;
    const uint32_t max_recv = 8u;

    if (!state || state->bootstrap_h == HANDLE_INVALID) return 0;

    while (recv_count < max_recv) {
        struct KChanMsg msg;
        uint32_t kind;

        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) ((uint8_t *)&msg)[i] = 0;
        if (svcmgr_syscall2(SYS_CHAN_RECV, state->bootstrap_h,
                            (uint64_t)(uintptr_t)&msg) != IRIS_OK)
            return 0;
        recv_count++;

        if (msg.type != SVCMGR_MSG_BOOTSTRAP_HANDLE ||
            msg.data_len < SVCMGR_BOOTSTRAP_MSG_LEN ||
            msg.attached_handle == HANDLE_INVALID) {
            if (msg.attached_handle != HANDLE_INVALID)
                (void)svcmgr_syscall1(SYS_HANDLE_CLOSE, msg.attached_handle);
            continue;
        }

        kind = svcmgr_proto_read_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND]);
        if (kind == SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP) {
            state->console_h = msg.attached_handle;
            continue; /* keep reading — SPAWN_CAP comes next */
        }
        if (kind == SVCMGR_BOOTSTRAP_KIND_SPAWN_CAP &&
            rights_check(msg.attached_rights, RIGHT_READ)) {
            state->spawn_cap_h = msg.attached_handle;
            return 1;
        }
        /* unexpected kind: discard and keep waiting */
        (void)svcmgr_syscall1(SYS_HANDLE_CLOSE, msg.attached_handle);
    }

    return 0;
}

/*
 * Request hardware capabilities from the kernel using the spawn cap as authority.
 * Policy (which service gets which IRQ / port range) comes from the service catalog
 * here in svcmgr, not from the kernel.
 */
static void svcmgr_request_hardware_caps(struct svcmgr_state *state) {
    uint32_t ci;

    if (!state || state->spawn_cap_h == HANDLE_INVALID) return;

    for (ci = 0; ci < iris_service_catalog_count(); ci++) {
        const struct iris_service_catalog_entry *e = iris_service_catalog_at(ci);
        if (!e) continue;

        if (e->irq_num != 0xFFu && e->irq_num < SVCMGR_IRQ_CAPS_TABLE_SIZE &&
            state->irq_caps[e->irq_num] == HANDLE_INVALID) {
            int64_t h = svcmgr_syscall2(SYS_CAP_CREATE_IRQCAP,
                                        state->spawn_cap_h, e->irq_num);
            if (h >= 0)
                state->irq_caps[e->irq_num] = (handle_id_t)h;
        }

        if (e->ioport_count > 0u && e->service_id < SVCMGR_IOPORT_CAPS_TABLE_SIZE &&
            state->ioport_caps[e->service_id] == HANDLE_INVALID) {
            int64_t h = svcmgr_syscall3(SYS_CAP_CREATE_IOPORT,
                                        state->spawn_cap_h,
                                        e->ioport_base, e->ioport_count);
            if (h >= 0)
                state->ioport_caps[e->service_id] = (handle_id_t)h;
        }
    }

    /* B3: strip HW_ACCESS (and FRAMEBUFFER) once bootstrap is done.
     * Preserve SPAWN_SERVICE (needed for SYS_INITRD_VMO) and KDEBUG (for SYS_POWEROFF).
     * BOOTCAP_RESTRICT creates a new cap object; init's original cap is unaffected. */
    (void)svcmgr_syscall2(SYS_BOOTCAP_RESTRICT,
                          state->spawn_cap_h,
                          IRIS_BOOTCAP_SPAWN_SERVICE | IRIS_BOOTCAP_KDEBUG);
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
static void svcmgr_seal_handle_if_valid(handle_id_t *h) {
    if (!h || *h == HANDLE_INVALID) return;
    (void)svcmgr_syscall1(SYS_CHAN_SEAL, *h);
    svcmgr_close_handle_if_valid(h);
}

static void svcmgr_clear_service_masters(struct svcmgr_state *state, uint32_t service_id) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, service_id);
    if (!svc) return;
    svcmgr_seal_handle_if_valid(&svc->public_h);
    svcmgr_seal_handle_if_valid(&svc->reply_h);
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

static struct svcmgr_dynamic_service *svcmgr_dynamic_find_endpoint(struct svcmgr_state *state,
                                                                   uint32_t endpoint) {
    if (!state || endpoint == 0u) return 0;
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        if (state->dynamic[i].active && state->dynamic[i].endpoint == endpoint)
            return &state->dynamic[i];
    }
    return 0;
}

static struct svcmgr_dynamic_service *svcmgr_dynamic_find_name(struct svcmgr_state *state,
                                                               const char *name) {
    if (!state || !name || name[0] == '\0') return 0;
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        if (state->dynamic[i].active && svcmgr_name_equal(state->dynamic[i].name, name))
            return &state->dynamic[i];
    }
    return 0;
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
        if (state->dynamic[i].active && state->dynamic[i].public_h != HANDLE_INVALID)
            ready++;
    }
    return ready;
}

static void svcmgr_dynamic_clear(struct svcmgr_dynamic_service *svc, int seal) {
    if (!svc || !svc->active) return;
    if (svc->public_h != HANDLE_INVALID) {
        if (seal)
            svcmgr_seal_handle_if_valid(&svc->public_h);
        else
            svcmgr_close_handle_if_valid(&svc->public_h);
    }
    svc->endpoint = 0;
    svc->client_rights = RIGHT_NONE;
    svc->active = 0;
    for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++) svc->name[i] = '\0';
}

static iris_rights_t svcmgr_reduce_lookup_rights(iris_rights_t requested, iris_rights_t allowed) {
    if (requested & RIGHT_SAME_RIGHTS) return allowed;
    return requested & allowed;
}

static int64_t svcmgr_send_bootstrap_endpoint(handle_id_t child_boot_h,
                                              handle_id_t master_h,
                                              iris_rights_t child_rights,
                                              uint32_t endpoint_id) {
    struct KChanMsg msg;
    int64_t dup_h;

    if (master_h == HANDLE_INVALID) return (int64_t)IRIS_ERR_INVALID_ARG;

    dup_h = svcmgr_syscall2(SYS_HANDLE_DUP, master_h, child_rights | RIGHT_TRANSFER);
    if (dup_h < 0) {
        svcmgr_log(sm_str_bootdupfail);
        return dup_h;
    }

    {
        uint8_t *raw = (uint8_t *)&msg;
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
    }
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND], endpoint_id);
    msg.data_len = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = (handle_id_t)dup_h;
    msg.attached_rights = child_rights;

    if (svcmgr_syscall2(SYS_CHAN_SEND, child_boot_h, (uint64_t)(uintptr_t)&msg) < 0) {
        handle_id_t tmp = (handle_id_t)dup_h;
        svcmgr_close_handle_if_valid(&tmp);
        svcmgr_log(sm_str_bootsendfail);
        return (int64_t)IRIS_ERR_WOULD_BLOCK;
    }

    return IRIS_OK;
}

static int64_t svcmgr_send_console_cap(handle_id_t child_boot_h, handle_id_t console_h) {
    struct KChanMsg msg;
    int64_t dup_h;

    if (console_h == HANDLE_INVALID) return (int64_t)IRIS_ERR_INVALID_ARG;

    dup_h = svcmgr_syscall2(SYS_HANDLE_DUP, console_h,
                            RIGHT_WRITE | RIGHT_TRANSFER);
    if (dup_h < 0) {
        svcmgr_log(sm_str_bootdupfail);
        return dup_h;
    }

    {
        uint8_t *raw = (uint8_t *)&msg;
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
    }
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP);
    msg.data_len = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = (handle_id_t)dup_h;
    msg.attached_rights = RIGHT_WRITE;

    if (svcmgr_syscall2(SYS_CHAN_SEND, child_boot_h, (uint64_t)(uintptr_t)&msg) < 0) {
        handle_id_t tmp = (handle_id_t)dup_h;
        svcmgr_close_handle_if_valid(&tmp);
        svcmgr_log(sm_str_bootsendfail);
        return (int64_t)IRIS_ERR_WOULD_BLOCK;
    }

    return IRIS_OK;
}

static int64_t svcmgr_send_ioport_cap(handle_id_t child_boot_h, handle_id_t master_h) {
    struct KChanMsg msg;
    int64_t dup_h;

    if (master_h == HANDLE_INVALID) return (int64_t)IRIS_ERR_INVALID_ARG;

    /* Duplicate with RIGHT_READ|RIGHT_TRANSFER so the child gets RIGHT_READ only. */
    dup_h = svcmgr_syscall2(SYS_HANDLE_DUP, master_h, RIGHT_READ | RIGHT_TRANSFER);
    if (dup_h < 0) {
        svcmgr_log(sm_str_bootdupfail);
        return dup_h;
    }

    {
        uint8_t *raw = (uint8_t *)&msg;
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
    }
    msg.type = SVCMGR_MSG_BOOTSTRAP_HANDLE;
    svcmgr_proto_write_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND],
                           SVCMGR_BOOTSTRAP_KIND_IOPORT_CAP);
    msg.data_len = SVCMGR_BOOTSTRAP_MSG_LEN;
    msg.attached_handle = (handle_id_t)dup_h;
    msg.attached_rights = RIGHT_READ;

    if (svcmgr_syscall2(SYS_CHAN_SEND, child_boot_h, (uint64_t)(uintptr_t)&msg) < 0) {
        handle_id_t tmp = (handle_id_t)dup_h;
        svcmgr_close_handle_if_valid(&tmp);
        svcmgr_log(sm_str_bootsendfail);
        return (int64_t)IRIS_ERR_WOULD_BLOCK;
    }

    return IRIS_OK;
}

static int64_t svcmgr_bootstrap_child(struct svcmgr_state *state,
                                      const struct iris_service_catalog_entry *manifest,
                                      handle_id_t child_boot_h) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, manifest->service_id);
    int64_t r;

    if (!svc) return IRIS_ERR_INVALID_ARG;

    /* Send console channel FIRST so the child can store it before SERVICE/REPLY. */
    if (manifest->give_console && state->console_h != HANDLE_INVALID) {
        r = svcmgr_send_console_cap(child_boot_h, state->console_h);
        if (r != IRIS_OK) return r;
    }

    r = svcmgr_send_bootstrap_endpoint(child_boot_h, svc->public_h,
                                       manifest->child_service_rights,
                                       manifest->service_endpoint);
    if (r != IRIS_OK) return r;

    r = svcmgr_send_bootstrap_endpoint(child_boot_h, svc->reply_h,
                                       manifest->child_reply_rights,
                                       manifest->reply_endpoint);
    if (r != IRIS_OK) return r;

    /* Forward I/O port capability if this service requires hardware I/O. */
    if (manifest->ioport_count > 0u) {
        handle_id_t ioport_master_h =
            (manifest->service_id < SVCMGR_IOPORT_CAPS_TABLE_SIZE)
            ? state->ioport_caps[manifest->service_id]
            : HANDLE_INVALID;
        if (ioport_master_h != HANDLE_INVALID) {
            r = svcmgr_send_ioport_cap(child_boot_h, ioport_master_h);
            if (r != IRIS_OK) {
                svcmgr_close_handle_if_valid(&child_boot_h);
                return r;
            }
        }
    }

    /* Forward kbd channel to services that request it (e.g. sh). */
    if (manifest->give_kbd) {
        struct svcmgr_service_state *kbd_svc = svcmgr_service_state(state, SVCMGR_SERVICE_KBD);
        if (kbd_svc && kbd_svc->public_h != HANDLE_INVALID) {
            r = svcmgr_send_bootstrap_endpoint(child_boot_h, kbd_svc->public_h,
                                               RIGHT_WRITE,
                                               SVCMGR_BOOTSTRAP_KIND_KBD_CAP);
            if (r != IRIS_OK) {
                svcmgr_close_handle_if_valid(&child_boot_h);
                return r;
            }
        }
    }

    /* Forward vfs service+reply channels to services that request them (e.g. sh). */
    if (manifest->give_vfs) {
        struct svcmgr_service_state *vfs_svc = svcmgr_service_state(state, SVCMGR_SERVICE_VFS);
        if (vfs_svc) {
            if (vfs_svc->public_h != HANDLE_INVALID) {
                r = svcmgr_send_bootstrap_endpoint(child_boot_h, vfs_svc->public_h,
                                                   RIGHT_WRITE,
                                                   SVCMGR_BOOTSTRAP_KIND_VFS_CAP);
                if (r != IRIS_OK) {
                    svcmgr_close_handle_if_valid(&child_boot_h);
                    return r;
                }
            }
            if (vfs_svc->reply_h != HANDLE_INVALID) {
                r = svcmgr_send_bootstrap_endpoint(child_boot_h, vfs_svc->reply_h,
                                                   RIGHT_READ,
                                                   SVCMGR_BOOTSTRAP_KIND_VFS_REPLY_CAP);
                if (r != IRIS_OK) {
                    svcmgr_close_handle_if_valid(&child_boot_h);
                    return r;
                }
            }
        }
    }

    svcmgr_close_handle_if_valid(&child_boot_h);
    return IRIS_OK;
}

static int64_t svcmgr_send_lookup_reply(handle_id_t reply_h, uint32_t endpoint,
                                        int32_t err, handle_id_t attached_h,
                                        iris_rights_t attached_rights) {
    struct KChanMsg msg;
    svcmgr_proto_lookup_reply_init(&msg, endpoint, err, attached_h, attached_rights);
    return svcmgr_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void svcmgr_send_status_reply(handle_id_t reply_h,
                                     uint32_t manifest_count,
                                     uint32_t ready_services,
                                     uint32_t active_slots,
                                     uint32_t catalog_version) {
    struct KChanMsg msg;
    svcmgr_proto_status_reply_init(&msg, IRIS_OK, manifest_count, ready_services,
                                   active_slots, catalog_version);
    (void)svcmgr_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void svcmgr_send_diag_reply(handle_id_t reply_h,
                                   int32_t err,
                                   uint32_t tasks_live,
                                   uint32_t kproc_live,
                                   uint32_t irq_routes_active,
                                   uint32_t ticks_lo,
                                   uint32_t ticks_hi,
                                   uint32_t manifest_count,
                                   uint32_t ready_services,
                                   uint32_t active_slots,
                                   uint32_t catalog_version,
                                   uint32_t vfs_exports_ready,
                                   uint32_t vfs_open_files,
                                   uint32_t vfs_open_capacity,
                                   uint32_t vfs_exported_bytes,
                                   uint32_t kbd_flags) {
    struct KChanMsg msg;
    svcmgr_proto_diag_reply_init(&msg, err, tasks_live, kproc_live, irq_routes_active,
                                 ticks_lo, ticks_hi, manifest_count, ready_services,
                                 active_slots, catalog_version, vfs_exports_ready,
                                 vfs_open_files, vfs_open_capacity, vfs_exported_bytes,
                                 kbd_flags);
    (void)svcmgr_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static uint32_t svcmgr_ready_service_count(const struct svcmgr_state *state) {
    uint32_t ready = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])); i++) {
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

static int64_t svcmgr_make_status_reply_pair(handle_id_t *recv_h,
                                             handle_id_t *xfer_h) {
    int64_t base_h;
    int64_t local_recv_h;
    int64_t local_xfer_h;

    if (!recv_h || !xfer_h) return IRIS_ERR_INVALID_ARG;
    *recv_h = HANDLE_INVALID;
    *xfer_h = HANDLE_INVALID;

    base_h = svcmgr_syscall0(SYS_CHAN_CREATE);
    if (base_h < 0) return base_h;

    local_recv_h = svcmgr_syscall2(SYS_HANDLE_DUP, (handle_id_t)base_h, RIGHT_READ);
    if (local_recv_h < 0) {
        handle_id_t tmp = (handle_id_t)base_h;
        svcmgr_close_handle_if_valid(&tmp);
        return local_recv_h;
    }

    local_xfer_h = svcmgr_syscall2(SYS_HANDLE_DUP, (handle_id_t)base_h,
                                   RIGHT_WRITE | RIGHT_TRANSFER);
    {
        handle_id_t tmp = (handle_id_t)base_h;
        svcmgr_close_handle_if_valid(&tmp);
    }
    if (local_xfer_h < 0) {
        handle_id_t tmp = (handle_id_t)local_recv_h;
        svcmgr_close_handle_if_valid(&tmp);
        return local_xfer_h;
    }

    *recv_h = (handle_id_t)local_recv_h;
    *xfer_h = (handle_id_t)local_xfer_h;
    return IRIS_OK;
}

static int64_t svcmgr_query_kbd_status(const struct svcmgr_state *state,
                                       uint32_t *out_flags) {
    struct KChanMsg req;
    struct KChanMsg reply;
    const struct svcmgr_service_state *svc;
    int64_t rc;

    if (!state || !out_flags) return IRIS_ERR_INVALID_ARG;
    *out_flags = 0;
    svc = svcmgr_service_state((struct svcmgr_state *)state, SVCMGR_SERVICE_KBD);
    if (!svc || svc->public_h == HANDLE_INVALID || svc->reply_h == HANDLE_INVALID)
        return IRIS_ERR_NOT_FOUND;

    {
        uint8_t *raw = (uint8_t *)&req;
        for (uint32_t i = 0; i < (uint32_t)sizeof(req); i++) raw[i] = 0;
    }
    req.type = KBD_MSG_STATUS;
    req.data_len = KBD_MSG_STATUS_LEN;
    req.attached_handle = HANDLE_INVALID;
    req.attached_rights = RIGHT_NONE;

    rc = svcmgr_syscall2(SYS_CHAN_SEND, svc->public_h, (uint64_t)(uintptr_t)&req);
    if (rc < 0) return rc;

    {
        uint8_t *raw = (uint8_t *)&reply;
        for (uint32_t i = 0; i < (uint32_t)sizeof(reply); i++) raw[i] = 0;
    }
    rc = svcmgr_syscall2(SYS_CHAN_RECV, svc->reply_h, (uint64_t)(uintptr_t)&reply);
    if (rc < 0) return rc;
    if (reply.type != KBD_MSG_STATUS_REPLY || reply.data_len != KBD_MSG_STATUS_REPLY_LEN) {
        return IRIS_ERR_INTERNAL;
    }

    rc = (int32_t)kbd_proto_read_u32(&reply.data[KBD_MSG_OFF_STATUS_REPLY_ERR]);
    if (rc != IRIS_OK) return rc;
    *out_flags = kbd_proto_read_u32(&reply.data[KBD_MSG_OFF_STATUS_REPLY_FLAGS]);
    return IRIS_OK;
}

static int64_t svcmgr_query_vfs_status(const struct svcmgr_state *state,
                                       uint32_t *out_exports_ready,
                                       uint32_t *out_open_files,
                                       uint32_t *out_open_capacity,
                                       uint32_t *out_exported_bytes) {
    struct KChanMsg req;
    struct KChanMsg reply;
    const struct svcmgr_service_state *svc;
    handle_id_t recv_h = HANDLE_INVALID;
    handle_id_t xfer_h = HANDLE_INVALID;
    int64_t rc;

    if (!state || !out_exports_ready || !out_open_files ||
        !out_open_capacity || !out_exported_bytes)
        return IRIS_ERR_INVALID_ARG;
    *out_exports_ready = 0;
    *out_open_files = 0;
    *out_open_capacity = 0;
    *out_exported_bytes = 0;

    svc = svcmgr_service_state((struct svcmgr_state *)state, SVCMGR_SERVICE_VFS);
    if (!svc || svc->public_h == HANDLE_INVALID) return IRIS_ERR_NOT_FOUND;

    rc = svcmgr_make_status_reply_pair(&recv_h, &xfer_h);
    if (rc != IRIS_OK) return rc;

    {
        uint8_t *raw = (uint8_t *)&req;
        for (uint32_t i = 0; i < (uint32_t)sizeof(req); i++) raw[i] = 0;
    }
    req.type = VFS_MSG_STATUS;
    req.data_len = VFS_MSG_STATUS_LEN;
    req.attached_handle = xfer_h;
    req.attached_rights = RIGHT_WRITE;

    rc = svcmgr_syscall2(SYS_CHAN_SEND, svc->public_h, (uint64_t)(uintptr_t)&req);
    if (rc < 0) goto out;
    xfer_h = HANDLE_INVALID;

    {
        uint8_t *raw = (uint8_t *)&reply;
        for (uint32_t i = 0; i < (uint32_t)sizeof(reply); i++) raw[i] = 0;
    }
    rc = svcmgr_syscall2(SYS_CHAN_RECV, recv_h, (uint64_t)(uintptr_t)&reply);
    if (rc < 0) goto out;
    if (reply.type != VFS_MSG_STATUS_REPLY || reply.data_len != VFS_MSG_STATUS_REPLY_LEN) {
        rc = IRIS_ERR_INTERNAL;
        goto out;
    }

    rc = (int32_t)vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_STATUS_REPLY_ERR]);
    if (rc != IRIS_OK) goto out;
    *out_exports_ready = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_STATUS_REPLY_EXPORTS]);
    *out_open_files = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_STATUS_REPLY_OPENS]);
    *out_open_capacity = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_STATUS_REPLY_CAP]);
    *out_exported_bytes = vfs_proto_read_u32(&reply.data[VFS_MSG_OFF_STATUS_REPLY_BYTES]);
    rc = IRIS_OK;

out:
    svcmgr_close_handle_if_valid(&xfer_h);
    svcmgr_close_handle_if_valid(&recv_h);
    return rc;
}

static int64_t svcmgr_collect_diag(const struct svcmgr_state *state,
                                   uint32_t *out_tasks_live,
                                   uint32_t *out_kproc_live,
                                   uint32_t *out_irq_routes_active,
                                   uint32_t *out_ticks_lo,
                                   uint32_t *out_ticks_hi,
                                   uint32_t *out_manifest_count,
                                   uint32_t *out_ready_services,
                                   uint32_t *out_active_slots,
                                   uint32_t *out_catalog_version,
                                   uint32_t *out_vfs_exports_ready,
                                   uint32_t *out_vfs_open_files,
                                   uint32_t *out_vfs_open_capacity,
                                   uint32_t *out_vfs_exported_bytes,
                                   uint32_t *out_kbd_flags) {
    struct iris_diag_snapshot snap;
    int64_t rc;

    if (!state || !out_tasks_live || !out_kproc_live || !out_irq_routes_active ||
        !out_ticks_lo || !out_ticks_hi || !out_manifest_count || !out_ready_services ||
        !out_active_slots || !out_catalog_version || !out_vfs_exports_ready ||
        !out_vfs_open_files || !out_vfs_open_capacity || !out_vfs_exported_bytes ||
        !out_kbd_flags)
        return IRIS_ERR_INVALID_ARG;

    rc = svcmgr_syscall1(SYS_DIAG_SNAPSHOT, (uint64_t)(uintptr_t)&snap);
    if (rc != IRIS_OK) return rc;
    if (snap.magic != IRIS_DIAG_MAGIC || snap.version != IRIS_DIAG_VERSION)
        return IRIS_ERR_INTERNAL;
    svcmgr_log(sm_str_diag_snap);

    rc = svcmgr_query_vfs_status(state, out_vfs_exports_ready, out_vfs_open_files,
                                 out_vfs_open_capacity, out_vfs_exported_bytes);
    if (rc != IRIS_OK) return rc;
    svcmgr_log(sm_str_diag_vfs);
    rc = svcmgr_query_kbd_status(state, out_kbd_flags);
    if (rc != IRIS_OK) return rc;
    svcmgr_log(sm_str_diag_kbd);

    *out_tasks_live = snap.tasks_live;
    *out_kproc_live = snap.kproc_live;
    *out_irq_routes_active = snap.irq_routes_active;
    *out_ticks_lo = snap.ticks_lo;
    *out_ticks_hi = snap.ticks_hi;
    *out_manifest_count = iris_service_catalog_count();
    *out_ready_services = svcmgr_ready_service_count(state);
    *out_active_slots = svcmgr_active_slot_count(state);
    *out_catalog_version = IRIS_SERVICE_CATALOG_VERSION;
    return IRIS_OK;
}

static void svcmgr_log_diag_summary(uint32_t tasks_live,
                                    uint32_t kproc_live,
                                    uint32_t irq_routes_active,
                                    uint32_t ticks_lo,
                                    uint32_t ticks_hi,
                                    uint32_t ready_services,
                                    uint32_t manifest_count,
                                    uint32_t active_slots,
                                    uint32_t catalog_version,
                                    uint32_t vfs_exports_ready,
                                    uint32_t vfs_open_files,
                                    uint32_t vfs_open_capacity,
                                    uint32_t vfs_exported_bytes,
                                    uint32_t kbd_flags) {
    uint64_t ticks = ((uint64_t)ticks_hi << 32) | ticks_lo;

    svcmgr_log("[DIAG] core t=");
    svcmgr_log_u32(tasks_live);
    svcmgr_log(" kp=");
    svcmgr_log_u32(kproc_live);
    svcmgr_log(" irq=");
    svcmgr_log_u32(irq_routes_active);
    svcmgr_log(" ticks=");
    svcmgr_log_u64(ticks);
    svcmgr_log(" | svcmgr ");
    svcmgr_log_u32(ready_services);
    svcmgr_log("/");
    svcmgr_log_u32(manifest_count);
    svcmgr_log(" slots=");
    svcmgr_log_u32(active_slots);
    svcmgr_log(" cat=");
    svcmgr_log_u32(catalog_version);
    svcmgr_log(" | vfs exp=");
    svcmgr_log_u32(vfs_exports_ready);
    svcmgr_log(" open=");
    svcmgr_log_u32(vfs_open_files);
    svcmgr_log("/");
    svcmgr_log_u32(vfs_open_capacity);
    svcmgr_log(" bytes=");
    svcmgr_log_u32(vfs_exported_bytes);
    svcmgr_log(" | kbd=");
    svcmgr_log_hex8(kbd_flags & 0xFFu);
    svcmgr_log("\n");
}

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
        handle_id_t irqcap_h = (manifest->irq_num < SVCMGR_IRQ_CAPS_TABLE_SIZE)
                                ? state->irq_caps[manifest->irq_num]
                                : HANDLE_INVALID;
        if (irqcap_h == HANDLE_INVALID ||
            svcmgr_syscall3(SYS_IRQ_ROUTE_REGISTER, irqcap_h, public_h, proc_h) < 0) {
            svc->proc_h = HANDLE_INVALID;
            svcmgr_close_handle_if_valid(&proc_h);
            svcmgr_log(sm_str_irqfail);
            return 0;
        }
    }

    if (svcmgr_syscall3(SYS_PROCESS_WATCH, proc_h, state->bootstrap_h,
                        (uint64_t)manifest->service_id) != IRIS_OK) {
        svc->proc_h = HANDLE_INVALID;
        svcmgr_close_handle_if_valid(&proc_h);
        svcmgr_log(sm_str_spawnfail);
        return 0;
    }

    svcmgr_log(sm_str_watchok);
    svcmgr_log(sm_str_spawnok);
    return 1;
}

static void svcmgr_boot_service(struct svcmgr_state *state,
                                const struct iris_service_catalog_entry *manifest) {
    struct svcmgr_service_state *svc;
    handle_id_t child_boot_h = HANDLE_INVALID;
    int64_t public_h;
    int64_t reply_h;
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

    public_h = svcmgr_syscall0(SYS_CHAN_CREATE);
    if (public_h < 0) {
        svcmgr_log(sm_str_spawnfail);
        return;
    }
    svc->public_h = (handle_id_t)public_h;

    if (!manifest->image_name) {
        svcmgr_clear_service_masters(state, manifest->service_id);
        svcmgr_log(sm_str_svc_unknown);
        return;
    }

    reply_h = svcmgr_syscall0(SYS_CHAN_CREATE);
    if (reply_h < 0) {
        svcmgr_clear_service_masters(state, manifest->service_id);
        svcmgr_log(sm_str_spawnfail);
        return;
    }
    svc->reply_h = (handle_id_t)reply_h;

    {
        handle_id_t loaded_proc_h = HANDLE_INVALID;
        handle_id_t loaded_chan_h = HANDLE_INVALID;
        long r = svc_load(state->spawn_cap_h, manifest->image_name,
                          &loaded_proc_h, &loaded_chan_h);
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

    if (!svcmgr_track_spawn(state, manifest, (handle_id_t)proc_h, (handle_id_t)public_h)) {
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

static void svcmgr_handle_lookup(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t reply_h = msg->attached_handle;
    uint32_t endpoint = 0;
    iris_rights_t requested = RIGHT_NONE;
    int is_reply = 0;
    const struct iris_service_catalog_entry *manifest;
    struct svcmgr_dynamic_service *dynamic = 0;
    struct svcmgr_service_state *svc;
    handle_id_t master_h = HANDLE_INVALID;
    iris_rights_t allowed = RIGHT_NONE;
    iris_rights_t granted;
    int64_t xfer_h;

    if (!svcmgr_proto_lookup_valid(msg)) {
        svcmgr_log(sm_str_lookupfail);
        return;
    }
    svcmgr_proto_lookup_decode(msg, &endpoint, &requested);
    manifest = iris_service_catalog_find_by_endpoint(endpoint, &is_reply);
    if (!manifest)
        dynamic = svcmgr_dynamic_find_endpoint(state, endpoint);

    if (!manifest && !dynamic) {
        if (svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_NOT_FOUND, HANDLE_INVALID, RIGHT_NONE) < 0)
            svcmgr_log(sm_str_lookupsendfail);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    if (dynamic) {
        master_h = dynamic->public_h;
        allowed = dynamic->client_rights;
    } else {
        svc = svcmgr_service_state(state, manifest->service_id);
        if (!svc) {
            if (svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_INVALID_ARG, HANDLE_INVALID, RIGHT_NONE) < 0)
                svcmgr_log(sm_str_lookupsendfail);
            svcmgr_close_handle_if_valid(&reply_h);
            svcmgr_log(sm_str_lookupfail);
            return;
        }

        if (is_reply) {
            master_h = svc->reply_h;
            allowed = manifest->client_reply_rights;
        } else {
            master_h = svc->public_h;
            allowed = manifest->client_service_rights;
        }
    }

    granted = svcmgr_reduce_lookup_rights(requested, allowed);
    if (master_h == HANDLE_INVALID || granted == RIGHT_NONE) {
        if (svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_INVALID_ARG, HANDLE_INVALID, RIGHT_NONE) < 0)
            svcmgr_log(sm_str_lookupsendfail);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    xfer_h = svcmgr_syscall2(SYS_HANDLE_DUP, master_h, granted | RIGHT_TRANSFER);
    if (xfer_h < 0) {
        if (svcmgr_send_lookup_reply(reply_h, endpoint, (int32_t)xfer_h, HANDLE_INVALID, RIGHT_NONE) < 0)
            svcmgr_log(sm_str_lookupsendfail);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    if (svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_OK, (handle_id_t)xfer_h, granted) < 0)
        svcmgr_log(sm_str_lookupsendfail);
    svcmgr_close_handle_if_valid(&reply_h);
    svcmgr_log(sm_str_lookupok);
}

static void svcmgr_handle_lookup_name(struct svcmgr_state *state, const struct KChanMsg *msg) {
    char name[SVCMGR_SERVICE_NAME_CAP];
    handle_id_t reply_h = msg->attached_handle;
    const struct iris_service_catalog_entry *manifest;
    struct svcmgr_dynamic_service *dynamic = 0;
    struct svcmgr_service_state *svc;
    handle_id_t master_h = HANDLE_INVALID;
    iris_rights_t requested = RIGHT_NONE;
    iris_rights_t allowed = RIGHT_NONE;
    iris_rights_t granted;
    uint32_t endpoint = 0;
    int64_t xfer_h;

    if (!svcmgr_proto_lookup_name_valid(msg)) {
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    svcmgr_proto_lookup_name_decode(msg, name, &requested);
    manifest = svcmgr_catalog_find_name(name);
    if (!manifest)
        dynamic = svcmgr_dynamic_find_name(state, name);

    if (!manifest && !dynamic) {
        svcmgr_send_lookup_reply(reply_h, 0u, IRIS_ERR_NOT_FOUND, HANDLE_INVALID, RIGHT_NONE);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    if (dynamic) {
        endpoint = dynamic->endpoint;
        master_h = dynamic->public_h;
        allowed = dynamic->client_rights;
    } else {
        endpoint = manifest->service_endpoint;
        svc = svcmgr_service_state(state, manifest->service_id);
        if (!svc) {
            svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_INVALID_ARG, HANDLE_INVALID, RIGHT_NONE);
            svcmgr_close_handle_if_valid(&reply_h);
            svcmgr_log(sm_str_lookupfail);
            return;
        }
        master_h = svc->public_h;
        allowed = manifest->client_service_rights;
    }

    granted = svcmgr_reduce_lookup_rights(requested, allowed);
    if (master_h == HANDLE_INVALID || granted == RIGHT_NONE) {
        svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_INVALID_ARG, HANDLE_INVALID, RIGHT_NONE);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    xfer_h = svcmgr_syscall2(SYS_HANDLE_DUP, master_h, granted | RIGHT_TRANSFER);
    if (xfer_h < 0) {
        svcmgr_send_lookup_reply(reply_h, endpoint, (int32_t)xfer_h, HANDLE_INVALID, RIGHT_NONE);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_OK, (handle_id_t)xfer_h, granted);
    svcmgr_close_handle_if_valid(&reply_h);
    svcmgr_log(sm_str_lookup_name_ok);
}

static void svcmgr_handle_register(struct svcmgr_state *state, const struct KChanMsg *msg) {
    struct svcmgr_dynamic_service *slot;
    handle_id_t public_h = msg ? msg->attached_handle : HANDLE_INVALID;
    uint32_t endpoint = 0;
    iris_rights_t allowed = RIGHT_NONE;
    char name[SVCMGR_SERVICE_NAME_CAP];
    int64_t handle_type;
    int reject = 1;

    if (!state || !msg || !svcmgr_proto_register_valid(msg)) {
        svcmgr_log(sm_str_register_fail);
        return;
    }

    svcmgr_proto_register_decode(msg, &endpoint, &allowed, name);
    if (public_h == HANDLE_INVALID ||
        endpoint == 0u ||
        allowed == RIGHT_NONE ||
        !rights_check(msg->attached_rights, RIGHT_WRITE) ||
        !rights_check(msg->attached_rights, RIGHT_DUPLICATE))
        goto out;
    handle_type = svcmgr_syscall1(SYS_HANDLE_TYPE, public_h);
    if (handle_type < 0 || (uint64_t)handle_type != (uint64_t)IRIS_HANDLE_TYPE_CHANNEL)
        goto out;
    if (iris_service_catalog_find_by_endpoint(endpoint, 0) != 0)
        goto out;
    if (svcmgr_name_is_catalog(name))
        goto out;
    if (svcmgr_dynamic_find_endpoint(state, endpoint) != 0)
        goto out;
    if (svcmgr_dynamic_find_name(state, name) != 0)
        goto out;

    slot = svcmgr_dynamic_alloc_slot(state);
    if (!slot) {
        svcmgr_log(sm_str_register_full);
        goto out;
    }

    slot->endpoint = endpoint;
    slot->public_h = public_h;
    slot->client_rights = svcmgr_reduce_lookup_rights(allowed, msg->attached_rights);
    if (slot->client_rights == RIGHT_NONE)
        goto out;
    svcmgr_copy_name(slot->name, name);
    slot->active = 1u;
    reject = 0;
    svcmgr_log(sm_str_register_ok);

out:
    if (reject) {
        if (slot && slot->active == 0u && slot->public_h == public_h) {
            slot->endpoint = 0;
            slot->client_rights = RIGHT_NONE;
            for (uint32_t i = 0; i < SVCMGR_SERVICE_NAME_CAP; i++) slot->name[i] = '\0';
        }
        svcmgr_close_handle_if_valid(&public_h);
        svcmgr_log(sm_str_register_fail);
    }
}

static void svcmgr_handle_unregister(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t proof_h = msg ? msg->attached_handle : HANDLE_INVALID;
    struct svcmgr_dynamic_service *slot;
    uint32_t endpoint;
    int64_t same;

    if (!state || !msg || !svcmgr_proto_unregister_valid(msg)) {
        svcmgr_log(sm_str_unregister_fail);
        return;
    }

    endpoint = svcmgr_proto_unregister_endpoint(msg);
    slot = svcmgr_dynamic_find_endpoint(state, endpoint);
    if (!slot) {
        svcmgr_close_handle_if_valid(&proof_h);
        return;
    }

    same = svcmgr_syscall2(SYS_HANDLE_SAME_OBJECT, proof_h, slot->public_h);
    if (same != 1) {
        svcmgr_close_handle_if_valid(&proof_h);
        svcmgr_log(sm_str_unregister_fail);
        return;
    }

    svcmgr_close_handle_if_valid(&proof_h);
    svcmgr_dynamic_clear(slot, 1);
    svcmgr_log(sm_str_unregister_ok);
}

static void svcmgr_handle_status(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t reply_h = msg ? msg->attached_handle : HANDLE_INVALID;

    if (!svcmgr_proto_status_valid(msg)) {
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    svcmgr_send_status_reply(reply_h,
                             iris_service_catalog_count(),
                             svcmgr_ready_service_count(state),
                             svcmgr_active_slot_count(state),
                             IRIS_SERVICE_CATALOG_VERSION);
    svcmgr_close_handle_if_valid(&reply_h);
}

static void svcmgr_handle_diag(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t reply_h = msg ? msg->attached_handle : HANDLE_INVALID;
    uint32_t tasks_live;
    uint32_t kproc_live;
    uint32_t irq_routes_active;
    uint32_t ticks_lo;
    uint32_t ticks_hi;
    uint32_t manifest_count;
    uint32_t ready_services;
    uint32_t active_slots;
    uint32_t catalog_version;
    uint32_t vfs_exports_ready;
    uint32_t vfs_open_files;
    uint32_t vfs_open_capacity;
    uint32_t vfs_exported_bytes;
    uint32_t kbd_flags;
    int64_t rc;

    if (!svcmgr_proto_diag_valid(msg)) {
        svcmgr_log(sm_str_diag_fail);
        return;
    }
    svcmgr_log(sm_str_diag_req);

    rc = svcmgr_collect_diag(state, &tasks_live, &kproc_live, &irq_routes_active,
                             &ticks_lo, &ticks_hi, &manifest_count, &ready_services,
                             &active_slots, &catalog_version, &vfs_exports_ready,
                             &vfs_open_files, &vfs_open_capacity, &vfs_exported_bytes,
                             &kbd_flags);
    if (rc != IRIS_OK) {
        svcmgr_send_diag_reply(reply_h, (int32_t)rc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_diag_fail);
        return;
    }

    svcmgr_send_diag_reply(reply_h, IRIS_OK, tasks_live, kproc_live, irq_routes_active,
                           ticks_lo, ticks_hi, manifest_count, ready_services,
                           active_slots, catalog_version, vfs_exports_ready,
                           vfs_open_files, vfs_open_capacity, vfs_exported_bytes,
                           kbd_flags);
    svcmgr_log(sm_str_diag_done);
    svcmgr_close_handle_if_valid(&reply_h);
    svcmgr_log_diag_summary(tasks_live, kproc_live, irq_routes_active, ticks_lo, ticks_hi,
                            ready_services, manifest_count, active_slots, catalog_version,
                            vfs_exports_ready, vfs_open_files, vfs_open_capacity,
                            vfs_exported_bytes, kbd_flags);
}

static void svcmgr_release_service(struct svcmgr_state *state,
                                   uint32_t service_id,
                                   struct svcmgr_service_state *svc) {
    if (!svc || svc->proc_h == HANDLE_INVALID) return;

    svcmgr_log(sm_str_svc_exited);
    svcmgr_clear_service_masters(state, service_id);
    svcmgr_close_handle_if_valid(&svc->proc_h);
}

static void svcmgr_handle_proc_exit(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t watched_h = HANDLE_INVALID;
    uint32_t cookie = 0;
    uint32_t service_id;
    const struct iris_service_catalog_entry *manifest;
    struct svcmgr_service_state *svc;

    if (!svcmgr_proto_proc_exit_valid(msg)) return;
    svcmgr_proto_proc_exit_decode(msg, &watched_h, &cookie);

    /*
     * Fast path: cookie encodes the service_id set at watch-registration time.
     * Resolve the manifest in O(1) rather than after the slot scan.
     * Still verify proc_h to guard against spurious or replayed messages.
     */
    service_id = cookie;
    manifest   = iris_service_catalog_find_by_service_id(service_id);
    svc        = svcmgr_service_state(state, service_id);

    if (!manifest || !svc || svc->proc_h != watched_h) {
        manifest = 0;
        svc = 0;
        for (uint32_t i = 0;
             i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0]));
             i++) {
            if (state->services[i].proc_h != watched_h) continue;
            service_id = i;
            svc = &state->services[i];
            manifest = iris_service_catalog_find_by_service_id(service_id);
            break;
        }
    }

    if (!svc) return;  /* spurious exit event — no matching tracked service */

    svcmgr_release_service(state, service_id, svc);

    if (!svcmgr_should_restart_service(state, manifest)) {
        if (manifest && manifest->restart_on_exit) {
            svcmgr_log(sm_str_restart_exhausted);
            if (manifest->image_name) svcmgr_log(manifest->image_name);
            svcmgr_log("\n");
        }
        return;
    }

    svc = svcmgr_service_state(state, service_id);
    if (!svc) return;
    svc->restart_count++;

    svcmgr_log(sm_str_restart);
    if (manifest->image_name) svcmgr_log(manifest->image_name);
    svcmgr_log(" (");
    svcmgr_log_u32(svc->restart_count);
    svcmgr_log("/");
    svcmgr_log_u32((uint32_t)manifest->restart_limit);
    svcmgr_log(")\n");

    svcmgr_boot_service(state, manifest);
}

void svcmgr_main_c(handle_id_t bootstrap_h) {
    struct svcmgr_state *state = &g_svcmgr_state;
    struct KChanMsg msg;

    for (uint32_t i = 0; i < (uint32_t)sizeof(*state); i++) ((uint8_t *)state)[i] = 0;

    state->bootstrap_h = bootstrap_h;
    state->spawn_cap_h = HANDLE_INVALID;
    state->console_h   = HANDLE_INVALID;
    for (uint32_t i = 0; i < SVCMGR_IRQ_CAPS_TABLE_SIZE; i++)
        state->irq_caps[i] = HANDLE_INVALID;
    for (uint32_t i = 0; i < SVCMGR_IOPORT_CAPS_TABLE_SIZE; i++)
        state->ioport_caps[i] = HANDLE_INVALID;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])); i++) {
        state->services[i].public_h = HANDLE_INVALID;
        state->services[i].reply_h = HANDLE_INVALID;
        state->services[i].proc_h = HANDLE_INVALID;
    }
    for (uint32_t i = 0; i < SVCMGR_DYNAMIC_SERVICE_CAP; i++) {
        state->dynamic[i].endpoint = 0;
        state->dynamic[i].public_h = HANDLE_INVALID;
        state->dynamic[i].client_rights = RIGHT_NONE;
        state->dynamic[i].active = 0;
        for (uint32_t j = 0; j < SVCMGR_SERVICE_NAME_CAP; j++) state->dynamic[i].name[j] = '\0';
    }

    svcmgr_log(sm_str_started);
    if (!svcmgr_recv_bootstrap_caps(state)) {
        svcmgr_log(sm_str_bootcapfail);
        return;
    }
    svcmgr_request_hardware_caps(state);
    svcmgr_autostart_services(state);
    svcmgr_log(sm_str_ready);

    for (;;) {
        {
            uint8_t *raw = (uint8_t *)&msg;
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        }
        if (svcmgr_syscall2(SYS_CHAN_RECV, state->bootstrap_h, (uint64_t)(uintptr_t)&msg) != IRIS_OK) {
            svcmgr_log(sm_str_recverr);
            continue;
        }

        switch (msg.type) {
            case PROC_EVENT_MSG_EXIT:
                svcmgr_handle_proc_exit(state, &msg);
                break;
            case SVCMGR_MSG_LOOKUP:
                svcmgr_handle_lookup(state, &msg);
                break;
            case SVCMGR_MSG_LOOKUP_NAME:
                svcmgr_handle_lookup_name(state, &msg);
                break;
            case SVCMGR_MSG_REGISTER:
                svcmgr_handle_register(state, &msg);
                break;
            case SVCMGR_MSG_UNREGISTER:
                svcmgr_handle_unregister(state, &msg);
                break;
            case SVCMGR_MSG_STATUS:
                svcmgr_handle_status(state, &msg);
                break;
            case SVCMGR_MSG_DIAG:
                svcmgr_handle_diag(state, &msg);
                break;
            default:
                break;
        }
    }
}
