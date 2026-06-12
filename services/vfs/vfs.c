/*
 * vfs.c — VFS service, endpoint-only (Fase 7.5).
 *
 * The service speaks exactly one protocol: the stateless KEndpoint protocol
 * in iris/vfs_ep_proto.h (LIST / STAT / READ_AT / STATUS / PING), dispatched
 * by vfs_ep.c. The legacy stateful KChannel protocol (iris/vfs_proto.h:
 * OPEN/READ/CLOSE + server-side fd table + reply channel) was removed with
 * its last clients in this phase; svcmgr no longer creates the legacy
 * service/reply channels for VFS (catalog endpoint_only flag).
 *
 * KChannel remains only for the svcmgr bootstrap one-shot (handle delivery)
 * and console logging — neither is part of the VFS service protocol.
 */
#include <iris/svcmgr_proto.h>
#include <iris/console_proto.h>
#include <iris/syscall.h>
#include <iris/nc/error.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/rights.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <stdint.h>
#include "vfs_ep.h"
#include "../common/console_client.h"

#define VFS_SERVICE_EXPORTS 16u

struct vfs_state {
    handle_id_t bootstrap_h;
    handle_id_t console_h;
    handle_id_t spawn_cap_h;
    handle_id_t ep_h;          /* recv side of our KEndpoint (Fase 7.1) */
    struct vfs_export exports[VFS_SERVICE_EXPORTS];
};

static const char vfs_str_started[]   = "VFS start\n";
static const char vfs_str_ready[]     = "VFS ready\n";
static const char vfs_str_boot_ok[]   = "VFS bootstrap OK\n";
static const char vfs_str_boot_fail[] = "VFS bootstrap FAIL\n";
static const char vfs_str_ep_ready[]  = "[VFS] ep ready\n";
static const char vfs_str_ep_lost[]   = "[VFS] FATAL: ep recv failed\n";

static const char vfs_boot_export_iris[] = "iris.txt";
static const uint8_t vfs_boot_export_iris_data[] = "Hello from IrisOS VFS!\n";

static const char vfs_boot_export_services[] = "services.txt";
static const uint8_t vfs_boot_export_services_data[] =
    "kbd\nvfs\nconsole\nfb\nsh\n";

static const char vfs_boot_export_readme[] = "readme.txt";
static const uint8_t vfs_boot_export_readme_data[] =
    "IRIS — pure microkernel OS\n"
    "Phase 55 — ring-0/3 separation, IPC-only kernel interface.\n"
    "Kernel: PMM, paging, IPC, caps, IRQ routing, scheduler.\n"
    "Services: init svcmgr kbd vfs console fb sh\n";

static const char vfs_boot_export_catalog[] = "catalog.txt";
static const uint8_t vfs_boot_export_catalog_data[] =
    "0 userboot\n1 init\n2 svcmgr\n3 kbd\n4 vfs\n5 console\n6 fb\n7 sh\n";

/* initrd name→index table matching services/common/svc_loader.c */
static const char *const vfs_initrd_name_table[] = {
    "userboot", "init", "svcmgr", "kbd", "vfs", "console", "fb", "sh"
};
#define VFS_INITRD_NAME_COUNT 8u

/* Virtual address window for initrd VMO mappings: USER_VMO_BASE + slot * 8 MB */
#define VFS_INITRD_MAP_BASE  0x8050000000ULL
#define VFS_INITRD_MAP_SLOT  0x0000800000ULL


static inline int64_t vfs_syscall3(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "d"(arg2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t vfs_syscall2(uint64_t num, uint64_t arg0, uint64_t arg1) {
    return vfs_syscall3(num, arg0, arg1, 0);
}

static inline int64_t vfs_syscall1(uint64_t num, uint64_t arg0) {
    return vfs_syscall3(num, arg0, 0, 0);
}

static handle_id_t g_vfs_console_h = HANDLE_INVALID;

static void vfs_log(const char *msg) {
    console_write(g_vfs_console_h, msg);
}

static void vfs_copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

static void vfs_close_handle_if_valid(handle_id_t *h) {
    if (!h || *h == HANDLE_INVALID) return;
    (void)vfs_syscall1(SYS_HANDLE_CLOSE, *h);
    *h = HANDLE_INVALID;
}

static void vfs_copy_cstr(char *dst, const uint8_t *src, uint32_t len) {
    uint32_t i = 0;
    for (; i + 1u < VFS_EP_PATH_MAX && i < len && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
    for (i++; i < VFS_EP_PATH_MAX; i++) dst[i] = '\0';
}

static handle_id_t vfs_bootstrap_handle(struct KChanMsg *msg) {
    if (!msg) return HANDLE_INVALID;
    if (msg->type != SVCMGR_MSG_BOOTSTRAP_HANDLE) return HANDLE_INVALID;
    if (msg->attached_handle == HANDLE_INVALID) return HANDLE_INVALID;
    return msg->attached_handle;
}

static int vfs_seed_one_export(struct vfs_export *export_file,
                               const char *name,
                               const uint8_t *data,
                               uint32_t data_len) {
    if (!export_file || !name || !data) return 0;
    vfs_copy_cstr(export_file->name, (const uint8_t *)name, VFS_EP_PATH_MAX);
    if (data_len > (uint32_t)sizeof(export_file->data)) return 0;
    vfs_copy_bytes(export_file->data, data, data_len);
    export_file->size = data_len;
    export_file->ready = 1;
    return 1;
}

static int vfs_seed_exports(struct vfs_state *state) {
    uint32_t data_len;

    if (!state) return 0;
    data_len = (uint32_t)(sizeof(vfs_boot_export_iris_data) - 1u);
    if (!vfs_seed_one_export(&state->exports[0], vfs_boot_export_iris,
                             vfs_boot_export_iris_data, data_len)) return 0;
    data_len = (uint32_t)(sizeof(vfs_boot_export_services_data) - 1u);
    if (!vfs_seed_one_export(&state->exports[1], vfs_boot_export_services,
                             vfs_boot_export_services_data, data_len)) return 0;
    data_len = (uint32_t)(sizeof(vfs_boot_export_readme_data) - 1u);
    if (!vfs_seed_one_export(&state->exports[2], vfs_boot_export_readme,
                             vfs_boot_export_readme_data, data_len)) return 0;
    data_len = (uint32_t)(sizeof(vfs_boot_export_catalog_data) - 1u);
    if (!vfs_seed_one_export(&state->exports[3], vfs_boot_export_catalog,
                             vfs_boot_export_catalog_data, data_len)) return 0;
    return 1;
}

static void vfs_seed_initrd_exports(struct vfs_state *state) {
    int64_t  count_rc;
    uint32_t count;
    uint32_t i;

    if (!state || state->spawn_cap_h == HANDLE_INVALID) return;

    count_rc = vfs_syscall1(SYS_INITRD_COUNT, (uint64_t)state->spawn_cap_h);
    if (count_rc <= 0) return;
    count = (uint32_t)count_rc;
    if (count > VFS_INITRD_NAME_COUNT) count = VFS_INITRD_NAME_COUNT;

    for (i = 0; i < count; i++) {
        uint32_t slot;
        int64_t  vmo_h, sz_rc, map_rc;
        uint64_t virt;
        struct vfs_export *exp;

        /* Find a free export slot */
        for (slot = 0; slot < (uint32_t)(sizeof(state->exports)/sizeof(state->exports[0])); slot++) {
            if (!state->exports[slot].ready) break;
        }
        if (slot == (uint32_t)(sizeof(state->exports)/sizeof(state->exports[0]))) break;

        vmo_h = vfs_syscall2(SYS_INITRD_VMO, (uint64_t)state->spawn_cap_h, (uint64_t)i);
        if (vmo_h < 0) continue;

        sz_rc = vfs_syscall1(SYS_VMO_SIZE, (uint64_t)vmo_h);
        if (sz_rc <= 0) {
            vfs_syscall1(SYS_HANDLE_CLOSE, (uint64_t)vmo_h);
            continue;
        }

        virt = VFS_INITRD_MAP_BASE + (uint64_t)i * VFS_INITRD_MAP_SLOT;
        map_rc = vfs_syscall3(SYS_VMO_MAP, (uint64_t)vmo_h, virt, 0);
        vfs_syscall1(SYS_HANDLE_CLOSE, (uint64_t)vmo_h);
        if (map_rc != 0) continue;

        exp = &state->exports[slot];
        vfs_copy_cstr(exp->name, (const uint8_t *)vfs_initrd_name_table[i], VFS_EP_PATH_MAX);
        exp->size = (uint32_t)sz_rc;
        exp->is_mapped = 1;
        exp->virt_base = virt;
        exp->ready = 1;
    }
}

/* Single-threaded server: static IPC buffers, no stack pressure. */
static uint8_t g_vfs_ep_req_buf[VFS_EP_DATA_MAX];
static uint8_t g_vfs_ep_reply_buf[VFS_EP_DATA_MAX];

/*
 * Handle one received EP request: dispatch, then reply exactly once on the
 * attached KReply cap and close it. Requests without a reply cap (plain
 * EP_SEND) are served-and-dropped: there is no one to answer.
 * If the attached cap is not a KReply, SYS_REPLY fails and the close below
 * still releases it — no handle leak.
 */
static void vfs_ep_serve(struct vfs_state *state, struct IrisMsg *req) {
    struct IrisMsg reply;
    handle_id_t reply_h = (handle_id_t)req->attached_handle;
    const uint8_t *req_buf =
        (req->buf_len > 0u && req->buf_uptr != 0u) ? g_vfs_ep_req_buf : 0;

    vfs_ep_dispatch(state->exports,
                    (uint32_t)(sizeof(state->exports) / sizeof(state->exports[0])),
                    req, req_buf, &reply, g_vfs_ep_reply_buf);

    if (reply_h == HANDLE_INVALID) return;
    (void)vfs_syscall2(SYS_REPLY, reply_h, (uint64_t)(uintptr_t)&reply);
    vfs_close_handle_if_valid(&reply_h);
}

void vfs_server_main_c(handle_id_t bootstrap_h) {
    struct vfs_state state;
    struct KChanMsg msg;

    for (uint32_t i = 0; i < (uint32_t)sizeof(state); i++) ((uint8_t *)&state)[i] = 0;
    state.bootstrap_h = bootstrap_h;
    state.console_h = HANDLE_INVALID;
    state.spawn_cap_h = HANDLE_INVALID;
    state.ep_h = HANDLE_INVALID;

    vfs_log(vfs_str_started);

    /* Bootstrap one-shot: svcmgr delivers console cap, our KEndpoint recv
     * side, and the initrd spawn cap. The endpoint is mandatory — there is
     * no legacy fallback to hide a broken EP delivery. */
    while (state.ep_h == HANDLE_INVALID || state.spawn_cap_h == HANDLE_INVALID) {
        {
            uint8_t *raw = (uint8_t *)&msg;
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        }
        if (vfs_syscall2(SYS_CHAN_RECV, state.bootstrap_h, (uint64_t)(uintptr_t)&msg) != IRIS_OK)
            goto fail;

        if (vfs_bootstrap_handle(&msg) == HANDLE_INVALID)
            goto fail;

        switch (svcmgr_proto_read_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND])) {
            case SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP:
                state.console_h = msg.attached_handle;
                g_vfs_console_h = state.console_h;
                break;
            case SVCMGR_BOOTSTRAP_KIND_INITRD_CAP:
                state.spawn_cap_h = msg.attached_handle;
                break;
            case SVCMGR_BOOTSTRAP_KIND_SERVICE_EP:
                if (state.ep_h == HANDLE_INVALID)
                    state.ep_h = msg.attached_handle;
                else
                    vfs_close_handle_if_valid(&msg.attached_handle);
                break;
            default:
                /* Unknown kind: close attached handle and keep receiving. */
                vfs_close_handle_if_valid(&msg.attached_handle);
                break;
        }
    }

    /* Drain any bootstrap messages queued after the required set (e.g. the
     * svcmgr discovery EP, kind 0x20, which VFS does not use this phase) so
     * their attached handles are not leaked into the closed channel. */
    for (;;) {
        {
            uint8_t *raw = (uint8_t *)&msg;
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        }
        if (vfs_syscall2(SYS_CHAN_RECV_NB, state.bootstrap_h,
                         (uint64_t)(uintptr_t)&msg) != IRIS_OK)
            break;
        if (vfs_bootstrap_handle(&msg) == HANDLE_INVALID) continue;
        vfs_close_handle_if_valid(&msg.attached_handle);
    }

    if (!vfs_seed_exports(&state)) goto fail;
    vfs_seed_initrd_exports(&state);
    vfs_close_handle_if_valid(&state.spawn_cap_h);
    vfs_log(vfs_str_boot_ok);
    vfs_close_handle_if_valid(&state.bootstrap_h);
    vfs_log(vfs_str_ep_ready);
    vfs_log(vfs_str_ready);

    /* Endpoint-only main loop: block on the KEndpoint, serve, reply.
     * svcmgr holds the endpoint master forever, so a clean recv failure
     * here is a real fault — fail loudly instead of spinning. */
    for (;;) {
        struct IrisMsg req;
        int64_t r;

        {
            uint8_t *raw = (uint8_t *)&req;
            for (uint32_t i = 0; i < (uint32_t)sizeof(req); i++) raw[i] = 0;
        }
        req.buf_uptr = (uint64_t)(uintptr_t)g_vfs_ep_req_buf;

        r = vfs_syscall2(SYS_EP_RECV, state.ep_h, (uint64_t)(uintptr_t)&req);
        if (r != IRIS_OK) {
            vfs_log(vfs_str_ep_lost);
            goto fail;
        }
        vfs_ep_serve(&state, &req);
    }

fail:
    vfs_log(vfs_str_boot_fail);
    for (;;) __asm__ volatile ("hlt");
}
