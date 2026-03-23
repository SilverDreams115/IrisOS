#ifndef IRIS_VFS_PROTO_H
#define IRIS_VFS_PROTO_H

#ifndef __ASSEMBLER__
#include <iris/nc/kchannel.h>
#include <iris/nc/handle.h>
#include <iris/nc/rights.h>
#endif

/*
 * Userland VFS protocol.
 *
 * Phase 9/current:
 *   - Clients talk to the ring-3 vfs service over the public request channel.
 *   - The service owns the client-visible file_id namespace and per-client
 *     open-file state for the migrated read-only open/read/close path.
 *   - The service now also owns namespace/open decisions for the exported
 *     boot-file subset it seeds at startup from the kernel backend.
 *     After that one-time seed, normal OPEN/READ on that subset do not ask
 *     the kernel VFS to resolve names or per-open state.
 *   - OPEN carries an attached self proc_handle with RIGHT_READ so the
 *     service can reclaim dead-client state without taking ownership back
 *     into the kernel.
 *   - Kernel SYS_OPEN/SYS_READ/SYS_CLOSE remain a transitional backend only;
 *     clients do not receive kernel fd numbers on this path.
 */

#define VFS_MSG_OPEN        0x00010001u
#define VFS_MSG_READ        0x00010002u
#define VFS_MSG_CLOSE       0x00010003u
#define VFS_MSG_RECLAIM_PROBE 0x00010004u
#define VFS_MSG_STATUS      0x00010005u

#define VFS_MSG_OPEN_REPLY  0x80010001u
#define VFS_MSG_READ_REPLY  0x80010002u
#define VFS_MSG_CLOSE_REPLY 0x80010003u
#define VFS_MSG_RECLAIM_PROBE_REPLY 0x80010004u
#define VFS_MSG_STATUS_REPLY 0x80010005u

#define VFS_MSG_OFF_OPEN_FLAGS        0u
#define VFS_MSG_OFF_OPEN_PATH         4u
#define VFS_MSG_OPEN_PATH_MAX         60u

#define VFS_MSG_OFF_OPEN_REPLY_ERR     0u
#define VFS_MSG_OFF_OPEN_REPLY_FILE_ID 4u

#define VFS_MSG_OFF_READ_FILE_ID       0u
#define VFS_MSG_OFF_READ_LEN           4u

#define VFS_MSG_OFF_READ_REPLY_ERR      0u
#define VFS_MSG_OFF_READ_REPLY_FILE_ID  4u
#define VFS_MSG_OFF_READ_REPLY_LEN      8u
#define VFS_MSG_OFF_READ_REPLY_DATA    12u
#define VFS_MSG_READ_REPLY_DATA_MAX    52u

#define VFS_MSG_OFF_CLOSE_FILE_ID       0u
#define VFS_MSG_OFF_CLOSE_REPLY_ERR     0u
#define VFS_MSG_OFF_CLOSE_REPLY_FILE_ID 4u

#define VFS_MSG_OFF_STATUS_REPLY_ERR      0u
#define VFS_MSG_OFF_STATUS_REPLY_VERSION  4u
#define VFS_MSG_OFF_STATUS_REPLY_EXPORTS  8u
#define VFS_MSG_OFF_STATUS_REPLY_OPENS   12u
#define VFS_MSG_OFF_STATUS_REPLY_CAP     16u
#define VFS_MSG_OFF_STATUS_REPLY_BYTES   20u

#define VFS_MSG_OPEN_MIN_LEN        5u
#define VFS_MSG_OPEN_REPLY_LEN      8u
#define VFS_MSG_READ_LEN            8u
#define VFS_MSG_READ_REPLY_BASE_LEN 12u
#define VFS_MSG_CLOSE_LEN           4u
#define VFS_MSG_CLOSE_REPLY_LEN     8u
#define VFS_MSG_RECLAIM_PROBE_LEN   0u
#define VFS_MSG_RECLAIM_PROBE_REPLY_LEN 4u
#define VFS_MSG_STATUS_LEN          0u
#define VFS_MSG_STATUS_REPLY_LEN    24u

#define VFS_SERVICE_OPEN_FILES      16u
#define VFS_PROTO_VERSION           1u

#ifndef __ASSEMBLER__
static inline void vfs_proto_write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static inline uint32_t vfs_proto_read_u32(const uint8_t *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static inline int vfs_proto_open_valid(const struct KChanMsg *msg) {
    uint32_t path_len;
    if (!msg || msg->type != VFS_MSG_OPEN) return 0;
    if (msg->data_len < VFS_MSG_OPEN_MIN_LEN) return 0;
    path_len = msg->data_len - VFS_MSG_OFF_OPEN_PATH;
    if (path_len == 0 || path_len > VFS_MSG_OPEN_PATH_MAX) return 0;
    if (msg->data[VFS_MSG_OFF_OPEN_PATH + path_len - 1u] != 0) return 0;
    return 1;
}

static inline void vfs_proto_open_reply_init(struct KChanMsg *msg,
                                             int32_t err,
                                             uint32_t file_id) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = VFS_MSG_OPEN_REPLY;
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_OPEN_REPLY_ERR], (uint32_t)err);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_OPEN_REPLY_FILE_ID], file_id);
    msg->data_len = VFS_MSG_OPEN_REPLY_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

static inline void vfs_proto_read_reply_init(struct KChanMsg *msg,
                                             int32_t err,
                                             uint32_t file_id,
                                             const uint8_t *data,
                                             uint32_t len) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = VFS_MSG_READ_REPLY;
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_READ_REPLY_ERR], (uint32_t)err);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_READ_REPLY_FILE_ID], file_id);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_READ_REPLY_LEN], len);
    if (data && len) {
        for (uint32_t i = 0; i < len; i++) msg->data[VFS_MSG_OFF_READ_REPLY_DATA + i] = data[i];
    }
    msg->data_len = VFS_MSG_READ_REPLY_BASE_LEN + len;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

static inline void vfs_proto_close_reply_init(struct KChanMsg *msg,
                                              int32_t err,
                                              uint32_t file_id) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = VFS_MSG_CLOSE_REPLY;
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_CLOSE_REPLY_ERR], (uint32_t)err);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_CLOSE_REPLY_FILE_ID], file_id);
    msg->data_len = VFS_MSG_CLOSE_REPLY_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}

static inline int vfs_proto_status_valid(const struct KChanMsg *msg) {
    return msg && msg->type == VFS_MSG_STATUS && msg->data_len == VFS_MSG_STATUS_LEN &&
           msg->attached_handle == HANDLE_INVALID;
}

static inline void vfs_proto_status_reply_init(struct KChanMsg *msg,
                                               int32_t err,
                                               uint32_t exports_ready,
                                               uint32_t open_files,
                                               uint32_t open_capacity,
                                               uint32_t exported_bytes) {
    uint8_t *raw = (uint8_t *)msg;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*msg); i++) raw[i] = 0;
    msg->type = VFS_MSG_STATUS_REPLY;
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_STATUS_REPLY_ERR], (uint32_t)err);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_STATUS_REPLY_VERSION], VFS_PROTO_VERSION);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_STATUS_REPLY_EXPORTS], exports_ready);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_STATUS_REPLY_OPENS], open_files);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_STATUS_REPLY_CAP], open_capacity);
    vfs_proto_write_u32(&msg->data[VFS_MSG_OFF_STATUS_REPLY_BYTES], exported_bytes);
    msg->data_len = VFS_MSG_STATUS_REPLY_LEN;
    msg->attached_handle = HANDLE_INVALID;
    msg->attached_rights = RIGHT_NONE;
}
#endif

#endif
