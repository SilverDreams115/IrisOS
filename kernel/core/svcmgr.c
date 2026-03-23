#include <iris/svcmgr_proto.h>
#include <iris/syscall.h>
#include <iris/nc/error.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <stdint.h>

extern void kbd_server(void);
extern void vfs_server(void);

#define SVCMGR_MAX_SLOTS 4u
#define SVCMGR_NAME_CAP  16u

struct svcmgr_slot {
    handle_id_t proc_h;
    uint8_t irq_num;
    uint8_t service_id;
    uint8_t poll_fallback;
    char name[SVCMGR_NAME_CAP];
};

struct svcmgr_service_manifest {
    uint32_t service_id;
    uint32_t service_endpoint;
    uint32_t reply_endpoint;
    uint8_t irq_num;
    uint8_t autostart;
    iris_rights_t child_service_rights;
    iris_rights_t child_reply_rights;
    iris_rights_t client_service_rights;
    iris_rights_t client_reply_rights;
};

struct svcmgr_service_state {
    handle_id_t public_h;
    handle_id_t reply_h;
};

struct svcmgr_state {
    handle_id_t bootstrap_h;
    handle_id_t phase3_proc_h;
    struct svcmgr_service_state services[3];
    struct svcmgr_slot slots[SVCMGR_MAX_SLOTS];
};

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
static const char sm_str_statusok[]     = "[SVCMGR] status reply OK\n";
static const char sm_str_lookupfail[]   = "[SVCMGR] WARN: lookup failed\n";
static const char sm_str_svc_exited[]   = "[SVCMGR] service exited\n";
static const char sm_str_slot_full[]    = "[SVCMGR] WARN: service slot full, proc_h not tracked\n";
static const char sm_str_irqfail[]      = "[SVCMGR] WARN: irq route xfer failed\n";
static const char sm_str_svc_unknown[]  = "[SVCMGR] WARN: unknown bootstrap service\n";
static const char sm_str_p3_retired[]   = "[SVCMGR] p3 proc_h retired\n";
static const char sm_str_p3_slot_free[] = "[SVCMGR] p3 slot free\n";
static const char sm_str_p3_fail[]      = "[SVCMGR] WARN: p3 supervision failed\n";
static const char sm_str_watchfail[]    = "[SVCMGR] WARN: lifecycle watch fallback\n";
static const char sm_str_watchok[]      = "[SVCMGR] lifecycle watch armed\n";
static const char sm_name_kbd[]         = "kbd";
static const char sm_name_vfs[]         = "vfs";

static const struct svcmgr_service_manifest g_manifest[] = {
    {
        .service_id = SVCMGR_SERVICE_KBD,
        .service_endpoint = SVCMGR_ENDPOINT_KBD,
        .reply_endpoint = SVCMGR_ENDPOINT_KBD_REPLY,
        .irq_num = 1u,
        .autostart = 1u,
        .child_service_rights = RIGHT_READ,
        .child_reply_rights = RIGHT_WRITE,
        .client_service_rights = RIGHT_WRITE,
        .client_reply_rights = RIGHT_READ,
    },
    {
        .service_id = SVCMGR_SERVICE_VFS,
        .service_endpoint = SVCMGR_ENDPOINT_VFS,
        .reply_endpoint = SVCMGR_ENDPOINT_VFS_REPLY,
        .irq_num = 0xFFu,
        .autostart = 1u,
        .child_service_rights = RIGHT_READ,
        .child_reply_rights = RIGHT_WRITE,
        .client_service_rights = RIGHT_WRITE | RIGHT_DUPLICATE,
        .client_reply_rights = RIGHT_READ | RIGHT_DUPLICATE,
    },
};

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
    (void)svcmgr_syscall1(SYS_WRITE, (uint64_t)(uintptr_t)msg);
}

static void svcmgr_close_handle_if_valid(handle_id_t *h) {
    if (!h || *h == HANDLE_INVALID) return;
    (void)svcmgr_syscall1(SYS_HANDLE_CLOSE, *h);
    *h = HANDLE_INVALID;
}

static const struct svcmgr_service_manifest *svcmgr_find_by_endpoint(uint32_t endpoint, int *is_reply) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_manifest) / sizeof(g_manifest[0])); i++) {
        if (g_manifest[i].service_endpoint == endpoint) {
            if (is_reply) *is_reply = 0;
            return &g_manifest[i];
        }
        if (g_manifest[i].reply_endpoint == endpoint) {
            if (is_reply) *is_reply = 1;
            return &g_manifest[i];
        }
    }
    return 0;
}

static const char *svcmgr_service_name(uint32_t service_id) {
    switch (service_id) {
        case SVCMGR_SERVICE_KBD:
            return sm_name_kbd;
        case SVCMGR_SERVICE_VFS:
            return sm_name_vfs;
        default:
            return 0;
    }
}

static uint64_t svcmgr_service_entry(uint32_t service_id) {
    switch (service_id) {
        case SVCMGR_SERVICE_KBD:
            return (uint64_t)(uintptr_t)kbd_server;
        case SVCMGR_SERVICE_VFS:
            return (uint64_t)(uintptr_t)vfs_server;
        default:
            return 0;
    }
}

static struct svcmgr_service_state *svcmgr_service_state(struct svcmgr_state *state,
                                                         uint32_t service_id) {
    if (!state) return 0;
    if (service_id >= (uint32_t)(sizeof(state->services) / sizeof(state->services[0])))
        return 0;
    return &state->services[service_id];
}

static void svcmgr_clear_service_masters(struct svcmgr_state *state, uint32_t service_id) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, service_id);
    if (!svc) return;
    svcmgr_close_handle_if_valid(&svc->public_h);
    svcmgr_close_handle_if_valid(&svc->reply_h);
}

static void svcmgr_copy_name(char *dst, const char *src) {
    uint32_t i = 0;
    for (; i + 1u < SVCMGR_NAME_CAP && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    for (i++; i < SVCMGR_NAME_CAP; i++) dst[i] = '\0';
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

static int64_t svcmgr_bootstrap_child(struct svcmgr_state *state,
                                      const struct svcmgr_service_manifest *manifest,
                                      handle_id_t child_boot_h) {
    struct svcmgr_service_state *svc = svcmgr_service_state(state, manifest->service_id);
    int64_t r;

    if (!svc) return IRIS_ERR_INVALID_ARG;

    r = svcmgr_send_bootstrap_endpoint(child_boot_h, svc->public_h,
                                       manifest->child_service_rights,
                                       manifest->service_endpoint);
    if (r != IRIS_OK) return r;

    r = svcmgr_send_bootstrap_endpoint(child_boot_h, svc->reply_h,
                                       manifest->child_reply_rights,
                                       manifest->reply_endpoint);
    if (r != IRIS_OK) return r;

    svcmgr_close_handle_if_valid(&child_boot_h);
    return IRIS_OK;
}

static void svcmgr_send_lookup_reply(handle_id_t reply_h, uint32_t endpoint,
                                     int32_t err, handle_id_t attached_h,
                                     iris_rights_t attached_rights) {
    struct KChanMsg msg;
    svcmgr_proto_lookup_reply_init(&msg, endpoint, err, attached_h, attached_rights);
    (void)svcmgr_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void svcmgr_send_status_reply(handle_id_t reply_h,
                                     uint32_t manifest_count,
                                     uint32_t ready_services,
                                     uint32_t active_slots,
                                     uint32_t fallback_slots) {
    struct KChanMsg msg;
    svcmgr_proto_status_reply_init(&msg, IRIS_OK, manifest_count, ready_services,
                                   active_slots, fallback_slots);
    (void)svcmgr_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static uint32_t svcmgr_ready_service_count(const struct svcmgr_state *state) {
    uint32_t ready = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->services) / sizeof(state->services[0])); i++) {
        if (state->services[i].public_h != HANDLE_INVALID &&
            state->services[i].reply_h != HANDLE_INVALID) ready++;
    }
    return ready;
}

static uint32_t svcmgr_active_slot_count(const struct svcmgr_state *state) {
    uint32_t active = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < SVCMGR_MAX_SLOTS; i++) {
        if (state->slots[i].proc_h != HANDLE_INVALID) active++;
    }
    return active;
}

static uint32_t svcmgr_fallback_slot_count(const struct svcmgr_state *state) {
    uint32_t fallback = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < SVCMGR_MAX_SLOTS; i++) {
        if (state->slots[i].proc_h != HANDLE_INVALID && state->slots[i].poll_fallback) fallback++;
    }
    return fallback;
}

static void svcmgr_track_spawn(struct svcmgr_state *state,
                               const struct svcmgr_service_manifest *manifest,
                               handle_id_t proc_h, handle_id_t public_h) {
    struct svcmgr_slot *slot = 0;
    const char *service_name = svcmgr_service_name(manifest->service_id);

    if (!service_name) {
        svcmgr_log(sm_str_svc_unknown);
        svcmgr_close_handle_if_valid(&proc_h);
        return;
    }

    for (uint32_t i = 0; i < SVCMGR_MAX_SLOTS; i++) {
        if (state->slots[i].proc_h == HANDLE_INVALID) {
            slot = &state->slots[i];
            break;
        }
    }

    if (!slot) {
        svcmgr_log(sm_str_slot_full);
        svcmgr_close_handle_if_valid(&proc_h);
        svcmgr_log(sm_str_spawnok);
        return;
    }

    slot->proc_h = proc_h;
    slot->irq_num = manifest->irq_num;
    slot->service_id = (uint8_t)manifest->service_id;
    slot->poll_fallback = 0;
    svcmgr_copy_name(slot->name, service_name);

    if (manifest->irq_num != 0xFFu) {
        if (svcmgr_syscall3(SYS_IRQ_ROUTE_REGISTER, manifest->irq_num, public_h, proc_h) < 0)
            svcmgr_log(sm_str_irqfail);
    }

    if (svcmgr_syscall3(SYS_PROCESS_WATCH, proc_h, state->bootstrap_h,
                        (uint64_t)manifest->service_id) != IRIS_OK) {
        slot->poll_fallback = 1;
        svcmgr_log(sm_str_watchfail);
    } else {
        svcmgr_log(sm_str_watchok);
    }

    svcmgr_log(sm_str_spawnok);
}

static void svcmgr_boot_service(struct svcmgr_state *state,
                                const struct svcmgr_service_manifest *manifest) {
    struct svcmgr_service_state *svc;
    uint64_t entry;
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

    entry = svcmgr_service_entry(manifest->service_id);
    if (entry == 0) {
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

    proc_h = svcmgr_syscall2(SYS_SPAWN, entry, (uint64_t)(uintptr_t)&child_boot_h);
    if (proc_h < 0) {
        svcmgr_clear_service_masters(state, manifest->service_id);
        svcmgr_close_handle_if_valid(&child_boot_h);
        svcmgr_log(sm_str_spawnfail);
        return;
    }

    if (svcmgr_bootstrap_child(state, manifest, child_boot_h) == IRIS_OK) {
        svcmgr_log(sm_str_bootok);
    } else {
        svcmgr_close_handle_if_valid(&child_boot_h);
        svcmgr_log(sm_str_bootfail);
    }

    svcmgr_track_spawn(state, manifest, (handle_id_t)proc_h, (handle_id_t)public_h);
}

static void svcmgr_autostart_services(struct svcmgr_state *state) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_manifest) / sizeof(g_manifest[0])); i++) {
        if (!g_manifest[i].autostart) continue;
        svcmgr_boot_service(state, &g_manifest[i]);
    }
}

static void svcmgr_handle_lookup(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t reply_h = msg->attached_handle;
    uint32_t endpoint = 0;
    iris_rights_t requested = RIGHT_NONE;
    int is_reply = 0;
    const struct svcmgr_service_manifest *manifest;
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
    manifest = svcmgr_find_by_endpoint(endpoint, &is_reply);

    if (!manifest) {
        svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_NOT_FOUND, HANDLE_INVALID, RIGHT_NONE);
        svcmgr_close_handle_if_valid(&reply_h);
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    svc = svcmgr_service_state(state, manifest->service_id);
    if (!svc) {
        svcmgr_send_lookup_reply(reply_h, endpoint, IRIS_ERR_INVALID_ARG, HANDLE_INVALID, RIGHT_NONE);
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
    svcmgr_log(sm_str_lookupok);
}

static void svcmgr_handle_status(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t reply_h = msg ? msg->attached_handle : HANDLE_INVALID;

    if (!svcmgr_proto_status_valid(msg)) {
        svcmgr_log(sm_str_lookupfail);
        return;
    }

    svcmgr_send_status_reply(reply_h,
                             (uint32_t)(sizeof(g_manifest) / sizeof(g_manifest[0])),
                             svcmgr_ready_service_count(state),
                             svcmgr_active_slot_count(state),
                             svcmgr_fallback_slot_count(state));
    svcmgr_close_handle_if_valid(&reply_h);
    svcmgr_log(sm_str_statusok);
}

static void svcmgr_handle_phase3_probe(struct svcmgr_state *state, const struct KChanMsg *msg) {
    uint64_t entry = svcmgr_proto_read_u64(&msg->data[SVCMGR_P3_OFF_ENTRY]);
    uint64_t tries = 32;
    int64_t status;

    if (state->phase3_proc_h != HANDLE_INVALID) {
        svcmgr_log(sm_str_p3_fail);
        return;
    }

    status = svcmgr_syscall2(SYS_SPAWN, entry, 0);
    if (status < 0) {
        svcmgr_log(sm_str_p3_fail);
        return;
    }
    state->phase3_proc_h = (handle_id_t)status;

    while (tries--) {
        status = svcmgr_syscall1(SYS_PROCESS_STATUS, state->phase3_proc_h);
        if (status < 0) {
            svcmgr_log(sm_str_p3_fail);
            goto fail;
        }
        if (status == 0) break;
        (void)svcmgr_syscall1(SYS_SLEEP, 1);
    }
    if (status != 0) {
        svcmgr_log(sm_str_p3_fail);
        goto fail;
    }

    if (svcmgr_syscall1(SYS_HANDLE_CLOSE, state->phase3_proc_h) != IRIS_OK) {
        svcmgr_log(sm_str_p3_fail);
        goto fail;
    }

    if (svcmgr_syscall1(SYS_PROCESS_STATUS, state->phase3_proc_h) != IRIS_ERR_BAD_HANDLE) {
        svcmgr_log(sm_str_p3_fail);
        goto fail;
    }

    state->phase3_proc_h = HANDLE_INVALID;
    svcmgr_log(sm_str_p3_retired);
    svcmgr_log(sm_str_p3_slot_free);
    return;

fail:
    svcmgr_close_handle_if_valid(&state->phase3_proc_h);
}

static void svcmgr_release_slot(struct svcmgr_state *state, struct svcmgr_slot *slot) {
    if (!slot || slot->proc_h == HANDLE_INVALID) return;

    svcmgr_log(sm_str_svc_exited);
    svcmgr_clear_service_masters(state, slot->service_id);
    svcmgr_close_handle_if_valid(&slot->proc_h);
    slot->irq_num = 0;
    slot->service_id = 0;
    slot->poll_fallback = 0;
    for (uint32_t j = 0; j < SVCMGR_NAME_CAP; j++) slot->name[j] = '\0';
}

static void svcmgr_handle_proc_exit(struct svcmgr_state *state, const struct KChanMsg *msg) {
    handle_id_t watched_h = HANDLE_INVALID;
    uint32_t cookie = 0;

    if (!svcmgr_proto_proc_exit_valid(msg)) return;
    svcmgr_proto_proc_exit_decode(msg, &watched_h, &cookie);
    (void)cookie;

    for (uint32_t i = 0; i < SVCMGR_MAX_SLOTS; i++) {
        if (state->slots[i].proc_h != watched_h) continue;
        svcmgr_release_slot(state, &state->slots[i]);
        return;
    }
}

static void svcmgr_check_lifecycle_fallback(struct svcmgr_state *state) {
    for (uint32_t i = 0; i < SVCMGR_MAX_SLOTS; i++) {
        struct svcmgr_slot *slot = &state->slots[i];
        int64_t status;

        if (slot->proc_h == HANDLE_INVALID || !slot->poll_fallback) continue;
        status = svcmgr_syscall1(SYS_PROCESS_STATUS, slot->proc_h);
        if (status < 0 || status > 0) continue;
        svcmgr_release_slot(state, slot);
    }
}

void svcmgr_main_c(handle_id_t bootstrap_h) {
    struct svcmgr_state state;
    struct KChanMsg msg;

    for (uint32_t i = 0; i < (uint32_t)sizeof(state); i++) ((uint8_t *)&state)[i] = 0;

    state.bootstrap_h = bootstrap_h;
    state.phase3_proc_h = HANDLE_INVALID;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state.services) / sizeof(state.services[0])); i++) {
        state.services[i].public_h = HANDLE_INVALID;
        state.services[i].reply_h = HANDLE_INVALID;
    }
    for (uint32_t i = 0; i < SVCMGR_MAX_SLOTS; i++) {
        state.slots[i].proc_h = HANDLE_INVALID;
        state.slots[i].irq_num = 0;
        state.slots[i].service_id = 0;
        state.slots[i].poll_fallback = 0;
        for (uint32_t j = 0; j < SVCMGR_NAME_CAP; j++) state.slots[i].name[j] = '\0';
    }

    svcmgr_log(sm_str_started);
    svcmgr_autostart_services(&state);
    svcmgr_log(sm_str_ready);

    for (;;) {
        svcmgr_check_lifecycle_fallback(&state);
        {
            uint8_t *raw = (uint8_t *)&msg;
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        }
        if (svcmgr_syscall2(SYS_CHAN_RECV, state.bootstrap_h, (uint64_t)(uintptr_t)&msg) != IRIS_OK) {
            svcmgr_log(sm_str_recverr);
            continue;
        }

        switch (msg.type) {
            case PROC_EVENT_MSG_EXIT:
                svcmgr_handle_proc_exit(&state, &msg);
                break;
            case SVCMGR_MSG_LOOKUP:
                svcmgr_handle_lookup(&state, &msg);
                break;
            case SVCMGR_MSG_STATUS:
                svcmgr_handle_status(&state, &msg);
                break;
            case SVCMGR_MSG_PHASE3_PROBE:
                svcmgr_handle_phase3_probe(&state, &msg);
                break;
            default:
                break;
        }
    }
}
