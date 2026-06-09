#include <iris/vfs_proto.h>
#include <iris/svcmgr_proto.h>
#include <iris/console_proto.h>
#include <iris/syscall.h>
#include <iris/vfs.h>
#include <iris/nc/error.h>
#include <iris/nc/handle.h>
#include <iris/nc/kchannel.h>
#include <iris/nc/rights.h>
#include <stdint.h>
#include "../common/console_client.h"

struct vfs_open_file {
    uint32_t owner_task_id;
    uint32_t offset;
    uint16_t generation;
    uint8_t  active;
    uint8_t  flags;
    uint8_t  export_id;
    uint8_t  client_slot;
    uint8_t  reserved[2];
};

struct vfs_client {
    uint32_t owner_task_id;
    handle_id_t owner_proc_h;
    uint16_t open_refs;
    uint8_t active;
    uint8_t reserved;
};

struct vfs_export {
    char     name[VFS_MAX_NAME];
    uint8_t  data[512];
    uint32_t size;
    uint8_t  ready;
    uint8_t  is_mapped;
    uint64_t virt_base;
};

struct vfs_state {
    handle_id_t bootstrap_h;
    handle_id_t service_h;
    handle_id_t reply_h;
    handle_id_t console_h;
    handle_id_t spawn_cap_h;
    struct vfs_export exports[VFS_SERVICE_EXPORTS];
    struct vfs_client clients[VFS_SERVICE_OPEN_FILES];
    struct vfs_open_file files[VFS_SERVICE_OPEN_FILES];
};

static const char vfs_str_started[]   = "VFS start\n";
static const char vfs_str_ready[]     = "VFS ready\n";
static const char vfs_str_boot_ok[]   = "VFS bootstrap OK\n";
static const char vfs_str_boot_fail[] = "VFS bootstrap FAIL\n";
static const char vfs_str_reclaim[]   = "VFS reclaimed dead client\n";

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

static inline int64_t vfs_syscall0(uint64_t num) {
    return vfs_syscall3(num, 0, 0, 0);
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
    for (; i + 1u < VFS_MAX_NAME && i < len && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = '\0';
    for (i++; i < VFS_MAX_NAME; i++) dst[i] = '\0';
}

static int vfs_str_equal(const char *a, const char *b) {
    uint32_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static handle_id_t vfs_bootstrap_handle(struct KChanMsg *msg) {
    if (!msg) return HANDLE_INVALID;
    if (msg->type != SVCMGR_MSG_BOOTSTRAP_HANDLE) return HANDLE_INVALID;
    if (msg->attached_handle == HANDLE_INVALID) return HANDLE_INVALID;
    return msg->attached_handle;
}

static void vfs_reply_open(handle_id_t reply_h, int32_t err, uint32_t file_id) {
    struct KChanMsg msg;
    vfs_proto_open_reply_init(&msg, err, file_id);
    (void)vfs_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void vfs_reply_read(handle_id_t reply_h, int32_t err, uint32_t file_id,
                           const uint8_t *data, uint32_t len) {
    struct KChanMsg msg;
    vfs_proto_read_reply_init(&msg, err, file_id, data, len);
    (void)vfs_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void vfs_reply_close(handle_id_t reply_h, int32_t err, uint32_t file_id) {
    struct KChanMsg msg;
    vfs_proto_close_reply_init(&msg, err, file_id);
    (void)vfs_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void vfs_reply_probe(handle_id_t reply_h, int32_t err) {
    struct KChanMsg msg;
    {
        uint8_t *raw = (uint8_t *)&msg;
        for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
    }
    msg.type = VFS_MSG_RECLAIM_PROBE_REPLY;
    vfs_proto_write_u32(&msg.data[0], (uint32_t)err);
    msg.data_len = VFS_MSG_RECLAIM_PROBE_REPLY_LEN;
    (void)vfs_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void vfs_reply_status(handle_id_t reply_h, int32_t err,
                             uint32_t exports_ready, uint32_t open_files,
                             uint32_t exported_bytes) {
    struct KChanMsg msg;
    vfs_proto_status_reply_init(&msg, err, exports_ready, open_files,
                                VFS_SERVICE_OPEN_FILES, exported_bytes);
    (void)vfs_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static void vfs_reply_list(handle_id_t reply_h, int32_t err, uint32_t index,
                           const struct vfs_export *export_file) {
    struct KChanMsg msg;
    const char *name = 0;
    uint32_t name_len = 0;
    uint32_t export_size = 0;

    if (export_file && export_file->ready) {
        while (name_len < VFS_MAX_NAME && export_file->name[name_len]) name_len++;
        name = export_file->name;
        export_size = export_file->size;
    }
    vfs_proto_list_reply_init(&msg, err, index, export_size, name, name_len);
    (void)vfs_syscall2(SYS_CHAN_SEND, reply_h, (uint64_t)(uintptr_t)&msg);
}

static uint32_t vfs_encode_file_id(uint32_t slot, uint16_t generation) {
    return ((uint32_t)generation << 16) | (slot + 1u);
}

static uint32_t vfs_client_cookie(uint32_t slot) {
    return slot + 1u;
}

static int vfs_client_slot_from_cookie(uint32_t cookie, uint32_t *slot_out) {
    if (cookie == 0 || cookie > VFS_SERVICE_OPEN_FILES) return 0;
    if (slot_out) *slot_out = cookie - 1u;
    return 1;
}

static struct vfs_open_file *vfs_lookup_file(struct vfs_state *state,
                                             uint32_t sender_id,
                                             uint32_t file_id) {
    uint32_t slot;
    uint16_t generation;
    struct vfs_open_file *file;

    if (!state || file_id == 0) return 0;
    slot = (file_id & 0xFFFFu);
    if (slot == 0 || slot > VFS_SERVICE_OPEN_FILES) return 0;
    generation = (uint16_t)(file_id >> 16);
    file = &state->files[slot - 1u];
    if (!file->active) return 0;
    if (file->generation != generation || generation == 0) return 0;
    if (file->owner_task_id != sender_id) return 0;
    return file;
}

static struct vfs_client *vfs_find_client(struct vfs_state *state, uint32_t sender_id,
                                          uint32_t *slot_out) {
    if (!state) return 0;
    for (uint32_t i = 0; i < VFS_SERVICE_OPEN_FILES; i++) {
        if (!state->clients[i].active) continue;
        if (state->clients[i].owner_task_id != sender_id) continue;
        if (slot_out) *slot_out = i;
        return &state->clients[i];
    }
    return 0;
}

static void vfs_clear_client(struct vfs_client *client) {
    if (!client) return;
    vfs_close_handle_if_valid(&client->owner_proc_h);
    client->owner_task_id = 0;
    client->open_refs = 0;
    client->active = 0;
}

static int32_t vfs_attach_client(struct vfs_state *state, uint32_t sender_id,
                                 handle_id_t owner_proc_h, uint32_t *slot_out) {
    struct vfs_client *client;
    uint32_t slot = 0;
    int64_t watch_rc;

    if (!state || !slot_out || owner_proc_h == HANDLE_INVALID)
        return IRIS_ERR_INVALID_ARG;

    client = vfs_find_client(state, sender_id, &slot);
    if (client) {
        if (client->open_refs == UINT16_MAX) {
            vfs_close_handle_if_valid(&owner_proc_h);
            return IRIS_ERR_TABLE_FULL;
        }
        client->open_refs++;
        vfs_close_handle_if_valid(&owner_proc_h);
        *slot_out = slot;
        return IRIS_OK;
    }

    for (slot = 0; slot < VFS_SERVICE_OPEN_FILES; slot++) {
        if (!state->clients[slot].active) break;
    }
    if (slot == VFS_SERVICE_OPEN_FILES) {
        vfs_close_handle_if_valid(&owner_proc_h);
        return IRIS_ERR_TABLE_FULL;
    }

    client = &state->clients[slot];
    client->owner_task_id = sender_id;
    client->owner_proc_h = owner_proc_h;
    client->open_refs = 1;
    client->active = 1;

    watch_rc = vfs_syscall3(SYS_PROCESS_WATCH, owner_proc_h, state->service_h,
                            vfs_client_cookie(slot));
    if (watch_rc != IRIS_OK) {
        vfs_clear_client(client);
        return (int32_t)watch_rc;
    }

    *slot_out = slot;
    return IRIS_OK;
}

static void vfs_release_client_ref(struct vfs_state *state, uint32_t client_slot) {
    struct vfs_client *client;

    if (!state || client_slot >= VFS_SERVICE_OPEN_FILES) return;
    client = &state->clients[client_slot];
    if (!client->active || client->open_refs == 0) return;
    client->open_refs--;
    if (client->open_refs == 0) vfs_clear_client(client);
}

static uint32_t vfs_alloc_file(struct vfs_state *state, uint32_t sender_id,
                               uint32_t client_slot, uint32_t flags,
                               uint8_t export_id) {
    for (uint32_t i = 0; i < VFS_SERVICE_OPEN_FILES; i++) {
        struct vfs_open_file *file = &state->files[i];
        if (file->active) continue;
        file->generation = (uint16_t)(file->generation + 1u);
        if (file->generation == 0) file->generation = 1;
        file->active = 1;
        file->owner_task_id = sender_id;
        file->offset = 0;
        file->flags = (uint8_t)flags;
        file->export_id = export_id;
        file->client_slot = (uint8_t)client_slot;
        return vfs_encode_file_id(i, file->generation);
    }
    return 0;
}

static void vfs_retire_file(struct vfs_open_file *file) {
    if (!file) return;
    file->active = 0;
    file->owner_task_id = 0;
    file->offset = 0;
    file->flags = 0;
    file->export_id = 0;
    file->client_slot = 0;
}

static uint32_t vfs_active_open_count(const struct vfs_state *state) {
    uint32_t active = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < VFS_SERVICE_OPEN_FILES; i++) {
        if (state->files[i].active) active++;
    }
    return active;
}

static uint32_t vfs_ready_export_count(const struct vfs_state *state) {
    uint32_t ready = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->exports) / sizeof(state->exports[0])); i++) {
        if (state->exports[i].ready) ready++;
    }
    return ready;
}

static uint32_t vfs_exported_bytes(const struct vfs_state *state) {
    uint32_t total = 0;
    if (!state) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->exports) / sizeof(state->exports[0])); i++) {
        if (state->exports[i].ready) total += state->exports[i].size;
    }
    return total;
}

static struct vfs_export *vfs_export_at_visible_index(struct vfs_state *state, uint32_t index) {
    uint32_t visible = 0;

    if (!state) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->exports) / sizeof(state->exports[0])); i++) {
        if (!state->exports[i].ready) continue;
        if (visible == index) return &state->exports[i];
        visible++;
    }
    return 0;
}

static struct vfs_export *vfs_find_export(struct vfs_state *state, const char *path,
                                          uint8_t *out_id) {
    if (!state || !path) return 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(state->exports) / sizeof(state->exports[0])); i++) {
        if (!state->exports[i].ready) continue;
        if (!vfs_str_equal(state->exports[i].name, path)) continue;
        if (out_id) *out_id = (uint8_t)i;
        return &state->exports[i];
    }
    return 0;
}

static int vfs_seed_one_export(struct vfs_export *export_file,
                               const char *name,
                               const uint8_t *data,
                               uint32_t data_len) {
    if (!export_file || !name || !data) return 0;
    vfs_copy_cstr(export_file->name, (const uint8_t *)name, (uint32_t)VFS_MAX_NAME);
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
        vfs_copy_cstr(exp->name, (const uint8_t *)vfs_initrd_name_table[i], VFS_MAX_NAME);
        exp->size = (uint32_t)sz_rc;
        exp->is_mapped = 1;
        exp->virt_base = virt;
        exp->ready = 1;
    }
}

static void vfs_handle_open(struct vfs_state *state, const struct KChanMsg *req,
                            handle_id_t reply_h) {
    uint32_t flags;
    char path[VFS_MAX_NAME];
    struct vfs_export *export_file;
    uint32_t file_id;
    handle_id_t owner_proc_h;
    uint32_t client_slot = 0;
    int32_t client_rc;
    uint8_t export_id = 0;

    if (!state || !req) return;
    if (!vfs_proto_open_valid(req)) {
        if (req->attached_handle != HANDLE_INVALID) {
            owner_proc_h = req->attached_handle;
            vfs_close_handle_if_valid(&owner_proc_h);
        }
        vfs_reply_open(reply_h, IRIS_ERR_INVALID_ARG, 0);
        return;
    }

    if (req->attached_handle == HANDLE_INVALID ||
        !rights_check(req->attached_rights, RIGHT_READ)) {
        vfs_reply_open(reply_h, IRIS_ERR_INVALID_ARG, 0);
        return;
    }

    flags = vfs_proto_read_u32(&req->data[VFS_MSG_OFF_OPEN_FLAGS]);
    if (flags != VFS_O_READ) {
        owner_proc_h = req->attached_handle;
        vfs_close_handle_if_valid(&owner_proc_h);
        vfs_reply_open(reply_h, IRIS_ERR_NOT_SUPPORTED, 0);
        return;
    }

    owner_proc_h = req->attached_handle;
    vfs_copy_cstr(path, &req->data[VFS_MSG_OFF_OPEN_PATH],
                  req->data_len - VFS_MSG_OFF_OPEN_PATH);
    export_file = vfs_find_export(state, path, &export_id);
    if (!export_file) {
        vfs_close_handle_if_valid(&owner_proc_h);
        vfs_reply_open(reply_h, IRIS_ERR_NOT_FOUND, 0);
        return;
    }

    client_rc = vfs_attach_client(state, req->sender_id, owner_proc_h, &client_slot);
    if (client_rc != IRIS_OK) {
        vfs_reply_open(reply_h, client_rc, 0);
        return;
    }

    file_id = vfs_alloc_file(state, req->sender_id, client_slot, flags,
                             export_id);
    if (file_id == 0) {
        vfs_release_client_ref(state, client_slot);
        vfs_reply_open(reply_h, IRIS_ERR_TABLE_FULL, 0);
        return;
    }

    vfs_reply_open(reply_h, IRIS_OK, file_id);
}

static void vfs_handle_read(struct vfs_state *state, const struct KChanMsg *req,
                            handle_id_t reply_h) {
    uint32_t file_id;
    uint32_t len;
    struct vfs_open_file *file;
    struct vfs_export *export_file;
    uint8_t data[VFS_MSG_READ_REPLY_DATA_MAX];
    uint32_t available;

    if (!state || !req || req->data_len < VFS_MSG_READ_LEN) {
        if (req && req->attached_handle != HANDLE_INVALID) {
            handle_id_t unexpected = req->attached_handle;
            vfs_close_handle_if_valid(&unexpected);
        }
        vfs_reply_read(reply_h, IRIS_ERR_INVALID_ARG, 0, 0, 0);
        return;
    }
    if (req->attached_handle != HANDLE_INVALID) {
        handle_id_t unexpected = req->attached_handle;
        vfs_close_handle_if_valid(&unexpected);
        vfs_reply_read(reply_h, IRIS_ERR_INVALID_ARG, 0, 0, 0);
        return;
    }

    file_id = vfs_proto_read_u32(&req->data[VFS_MSG_OFF_READ_FILE_ID]);
    len = vfs_proto_read_u32(&req->data[VFS_MSG_OFF_READ_LEN]);
    file = vfs_lookup_file(state, req->sender_id, file_id);
    if (!file) {
        vfs_reply_read(reply_h, IRIS_ERR_BAD_HANDLE, file_id, 0, 0);
        return;
    }
    if (file->export_id >= (uint8_t)(sizeof(state->exports) / sizeof(state->exports[0]))) {
        vfs_reply_read(reply_h, IRIS_ERR_INTERNAL, file_id, 0, 0);
        return;
    }
    export_file = &state->exports[file->export_id];
    if (!export_file->ready) {
        vfs_reply_read(reply_h, IRIS_ERR_INTERNAL, file_id, 0, 0);
        return;
    }

    if (len > VFS_MSG_READ_REPLY_DATA_MAX) len = VFS_MSG_READ_REPLY_DATA_MAX;
    if (file->offset >= export_file->size) {
        vfs_reply_read(reply_h, IRIS_OK, file_id, data, 0);
        return;
    }
    available = export_file->size - file->offset;
    if (len > available) len = available;
    if (export_file->is_mapped) {
        const uint8_t *src = (const uint8_t *)(uintptr_t)(export_file->virt_base +
                                                            file->offset);
        vfs_copy_bytes(data, src, len);
    } else {
        vfs_copy_bytes(data, &export_file->data[file->offset], len);
    }
    file->offset += len;
    vfs_reply_read(reply_h, IRIS_OK, file_id, data, len);
}

static void vfs_handle_close(struct vfs_state *state, const struct KChanMsg *req,
                             handle_id_t reply_h) {
    uint32_t file_id;
    struct vfs_open_file *file;

    if (!state || !req || req->data_len < VFS_MSG_CLOSE_LEN) {
        if (req && req->attached_handle != HANDLE_INVALID) {
            handle_id_t unexpected = req->attached_handle;
            vfs_close_handle_if_valid(&unexpected);
        }
        vfs_reply_close(reply_h, IRIS_ERR_INVALID_ARG, 0);
        return;
    }
    if (req->attached_handle != HANDLE_INVALID) {
        handle_id_t unexpected = req->attached_handle;
        vfs_close_handle_if_valid(&unexpected);
        vfs_reply_close(reply_h, IRIS_ERR_INVALID_ARG, 0);
        return;
    }

    file_id = vfs_proto_read_u32(&req->data[VFS_MSG_OFF_CLOSE_FILE_ID]);
    file = vfs_lookup_file(state, req->sender_id, file_id);
    if (!file) {
        vfs_reply_close(reply_h, IRIS_ERR_BAD_HANDLE, file_id);
        return;
    }

    vfs_release_client_ref(state, file->client_slot);
    vfs_retire_file(file);
    vfs_reply_close(reply_h, IRIS_OK, file_id);
}

static void vfs_reclaim_client(struct vfs_state *state, uint32_t client_slot,
                               handle_id_t watched_h) {
    struct vfs_client *client;

    if (!state || client_slot >= VFS_SERVICE_OPEN_FILES) return;
    client = &state->clients[client_slot];
    if (!client->active) return;
    if (watched_h != HANDLE_INVALID && client->owner_proc_h != watched_h) return;

    for (uint32_t i = 0; i < VFS_SERVICE_OPEN_FILES; i++) {
        struct vfs_open_file *file = &state->files[i];
        if (!file->active) continue;
        if (file->client_slot != client_slot) continue;
        vfs_retire_file(file);
    }

    vfs_clear_client(client);
    vfs_log(vfs_str_reclaim);
}

static void vfs_handle_proc_exit(struct vfs_state *state, const struct KChanMsg *msg) {
    handle_id_t watched_h = HANDLE_INVALID;
    uint32_t cookie = 0;
    uint32_t client_slot = 0;

    if (!state || !msg) return;
    if (msg->type != PROC_EVENT_MSG_EXIT || msg->data_len != PROC_EVENT_MSG_LEN) return;
    watched_h = (handle_id_t)vfs_proto_read_u32(&msg->data[PROC_EVENT_OFF_HANDLE]);
    cookie = vfs_proto_read_u32(&msg->data[PROC_EVENT_OFF_COOKIE]);
    if (!vfs_client_slot_from_cookie(cookie, &client_slot)) return;
    vfs_reclaim_client(state, client_slot, watched_h);
}

static void vfs_handle_probe(struct vfs_state *state, const struct KChanMsg *req,
                             handle_id_t reply_h) {
    (void)state;
    if (req && req->attached_handle != HANDLE_INVALID) {
        handle_id_t unexpected = req->attached_handle;
        vfs_close_handle_if_valid(&unexpected);
    }
    vfs_reply_probe(reply_h, IRIS_ERR_NOT_SUPPORTED);
}

static void vfs_handle_status(struct vfs_state *state, const struct KChanMsg *req,
                              handle_id_t reply_h) {
    handle_id_t dest_h = reply_h;

    if (!state || !vfs_proto_status_valid(req)) {
        if (req && req->attached_handle != HANDLE_INVALID) {
            handle_id_t unexpected = req->attached_handle;
            vfs_close_handle_if_valid(&unexpected);
        }
        vfs_reply_status(reply_h, IRIS_ERR_INVALID_ARG, 0, 0, 0);
        return;
    }
    if (req->attached_handle != HANDLE_INVALID) dest_h = req->attached_handle;
    vfs_reply_status(dest_h, IRIS_OK, vfs_ready_export_count(state),
                     vfs_active_open_count(state), vfs_exported_bytes(state));
    if (req->attached_handle != HANDLE_INVALID) vfs_close_handle_if_valid(&dest_h);
}

static void vfs_handle_list(struct vfs_state *state, const struct KChanMsg *req,
                            handle_id_t reply_h) {
    uint32_t index;
    struct vfs_export *export_file;

    if (!state || !vfs_proto_list_valid(req)) {
        if (req && req->attached_handle != HANDLE_INVALID) {
            handle_id_t unexpected = req->attached_handle;
            vfs_close_handle_if_valid(&unexpected);
        }
        vfs_reply_list(reply_h, IRIS_ERR_INVALID_ARG, 0, 0);
        return;
    }

    index = vfs_proto_read_u32(&req->data[VFS_MSG_OFF_LIST_INDEX]);
    export_file = vfs_export_at_visible_index(state, index);
    if (!export_file) {
        vfs_reply_list(reply_h, IRIS_ERR_NOT_FOUND, index, 0);
        return;
    }
    vfs_reply_list(reply_h, IRIS_OK, index, export_file);
}

void vfs_server_main_c(handle_id_t bootstrap_h) {
    struct vfs_state state;
    struct KChanMsg msg;

    for (uint32_t i = 0; i < (uint32_t)sizeof(state); i++) ((uint8_t *)&state)[i] = 0;
    state.bootstrap_h = bootstrap_h;
    state.service_h = HANDLE_INVALID;
    state.reply_h = HANDLE_INVALID;
    state.console_h = HANDLE_INVALID;
    state.spawn_cap_h = HANDLE_INVALID;
    for (uint32_t i = 0; i < VFS_SERVICE_OPEN_FILES; i++) {
        state.clients[i].owner_proc_h = HANDLE_INVALID;
    }
    for (uint32_t i = 0; i < VFS_SERVICE_OPEN_FILES; i++) {
        state.files[i].generation = 1;
    }

    vfs_log(vfs_str_started);

    while (state.service_h == HANDLE_INVALID || state.reply_h == HANDLE_INVALID ||
           state.spawn_cap_h == HANDLE_INVALID) {
        {
            uint8_t *raw = (uint8_t *)&msg;
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        }
        if (vfs_syscall2(SYS_CHAN_RECV, state.bootstrap_h, (uint64_t)(uintptr_t)&msg) != IRIS_OK)
            goto fail;

        if (vfs_bootstrap_handle(&msg) == HANDLE_INVALID)
            goto fail;

        switch (vfs_proto_read_u32(&msg.data[SVCMGR_BOOTSTRAP_OFF_KIND])) {
            case SVCMGR_ENDPOINT_VFS:
                state.service_h = msg.attached_handle;
                break;
            case SVCMGR_ENDPOINT_VFS_REPLY:
                state.reply_h = msg.attached_handle;
                break;
            case SVCMGR_BOOTSTRAP_KIND_CONSOLE_CAP:
                state.console_h = msg.attached_handle;
                g_vfs_console_h = state.console_h;
                break;
            case SVCMGR_BOOTSTRAP_KIND_INITRD_CAP:
                state.spawn_cap_h = msg.attached_handle;
                break;
            default:
                /* Unknown kind: close attached handle and keep receiving. */
                vfs_close_handle_if_valid(&msg.attached_handle);
                break;
        }
    }

    if (!vfs_seed_exports(&state)) goto fail;
    vfs_seed_initrd_exports(&state);
    vfs_close_handle_if_valid(&state.spawn_cap_h);
    vfs_log(vfs_str_boot_ok);
    vfs_close_handle_if_valid(&state.bootstrap_h);
    vfs_log(vfs_str_ready);

    for (;;) {
        {
            uint8_t *raw = (uint8_t *)&msg;
            for (uint32_t i = 0; i < (uint32_t)sizeof(msg); i++) raw[i] = 0;
        }
        if (vfs_syscall2(SYS_CHAN_RECV, state.service_h, (uint64_t)(uintptr_t)&msg) != IRIS_OK)
            continue;

        switch (msg.type) {
            case PROC_EVENT_MSG_EXIT:
                vfs_handle_proc_exit(&state, &msg);
                break;
            case VFS_MSG_OPEN:
                vfs_handle_open(&state, &msg, state.reply_h);
                break;
            case VFS_MSG_READ:
                vfs_handle_read(&state, &msg, state.reply_h);
                break;
            case VFS_MSG_CLOSE:
                vfs_handle_close(&state, &msg, state.reply_h);
                break;
            case VFS_MSG_STATUS:
                vfs_handle_status(&state, &msg, state.reply_h);
                break;
            case VFS_MSG_LIST:
                vfs_handle_list(&state, &msg, state.reply_h);
                break;
            case VFS_MSG_RECLAIM_PROBE:
                vfs_handle_probe(&state, &msg, state.reply_h);
                break;
            default:
                break;
        }
    }

fail:
    vfs_log(vfs_str_boot_fail);
    for (;;) __asm__ volatile ("hlt");
}

