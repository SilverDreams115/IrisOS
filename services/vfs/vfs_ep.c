/*
 * vfs_ep.c — VFS endpoint-protocol dispatcher (Fase 7.1).
 *
 * Pure request → reply logic for the IrisMsg-based VFS protocol
 * (iris/vfs_ep_proto.h). No syscalls, no globals: unit-testable on the host
 * (tests/kernel/test_vfs_ep.c) and wrapped by the drain loop in vfs.c.
 *
 * Invariants enforced here:
 *   - exactly one reply per request (caller sends it; we always fill it);
 *   - unknown opcode → IRIS_EP_REPLY_ERR / IRIS_ERR_NOT_SUPPORTED;
 *   - malformed payload (empty, oversized, non-NUL-terminated, undelivered)
 *     → IRIS_EP_REPLY_ERR / IRIS_ERR_INVALID_ARG;
 *   - reads are clamped to VFS_EP_DATA_MAX; offset >= size is EOF, not error.
 */

#include "vfs_ep.h"
#include <iris/endpoint_proto.h>
#include <iris/nc/error.h>

static void vfs_ep_msg_clear(struct IrisMsg *m) {
    uint8_t *p = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) p[i] = 0;
}

static void vfs_ep_reply_err(struct IrisMsg *reply, iris_error_t err) {
    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_ERR;
    reply->words[0]   = (uint64_t)(uint32_t)err;
    reply->word_count = 1u;
}

/*
 * Validate and extract the request path from the bulk payload.
 * Returns NULL if the payload is malformed (caller replies INVALID_ARG).
 */
static const char *vfs_ep_req_path(const struct IrisMsg *req,
                                   const uint8_t *req_buf) {
    if (!req_buf) return 0;                       /* payload not delivered */
    if (req->buf_len == 0u) return 0;             /* short payload */
    if (req->buf_len > VFS_EP_PATH_MAX) return 0; /* long payload */
    if (req_buf[req->buf_len - 1u] != 0u) return 0; /* not NUL-terminated */
    if (req_buf[0] == 0u) return 0;               /* empty name */
    return (const char *)req_buf;
}

static int vfs_ep_name_equal(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static const struct vfs_export *vfs_ep_find_export(const struct vfs_export *exports,
                                                   uint32_t export_count,
                                                   const char *path) {
    for (uint32_t i = 0; i < export_count; i++) {
        if (!exports[i].ready) continue;
        if (vfs_ep_name_equal(exports[i].name, path)) return &exports[i];
    }
    return 0;
}

static const struct vfs_export *vfs_ep_export_at_index(const struct vfs_export *exports,
                                                       uint32_t export_count,
                                                       uint64_t index) {
    uint64_t visible = 0;
    for (uint32_t i = 0; i < export_count; i++) {
        if (!exports[i].ready) continue;
        if (visible == index) return &exports[i];
        visible++;
    }
    return 0;
}

static const uint8_t *vfs_ep_export_bytes(const struct vfs_export *exp) {
    if (exp->is_mapped)
        return (const uint8_t *)(uintptr_t)exp->virt_base;
    return exp->data;
}

static void vfs_ep_handle_list(const struct vfs_export *exports,
                               uint32_t export_count,
                               const struct IrisMsg *req,
                               struct IrisMsg *reply, uint8_t *reply_buf) {
    if (req->word_count < 1u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const struct vfs_export *exp =
        vfs_ep_export_at_index(exports, export_count, req->words[0]);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }

    uint32_t name_len = 0;
    while (name_len + 1u < VFS_EP_PATH_MAX && exp->name[name_len]) name_len++;
    for (uint32_t i = 0; i < name_len; i++) reply_buf[i] = (uint8_t)exp->name[i];
    reply_buf[name_len] = 0u;

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = exp->size;
    reply->words[2]   = name_len;
    reply->word_count = 3u;
    reply->buf_len    = name_len + 1u;
    reply->buf_uptr   = (uint64_t)(uintptr_t)reply_buf;
}

static void vfs_ep_handle_stat(const struct vfs_export *exports,
                               uint32_t export_count,
                               const struct IrisMsg *req,
                               const uint8_t *req_buf,
                               struct IrisMsg *reply) {
    const char *path = vfs_ep_req_path(req, req_buf);
    if (!path) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const struct vfs_export *exp = vfs_ep_find_export(exports, export_count, path);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = exp->size;
    reply->word_count = 2u;
}

static void vfs_ep_handle_read_at(const struct vfs_export *exports,
                                  uint32_t export_count,
                                  const struct IrisMsg *req,
                                  const uint8_t *req_buf,
                                  struct IrisMsg *reply, uint8_t *reply_buf) {
    if (req->word_count < 2u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const char *path = vfs_ep_req_path(req, req_buf);
    if (!path) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    const struct vfs_export *exp = vfs_ep_find_export(exports, export_count, path);
    if (!exp) {
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_FOUND);
        return;
    }

    uint64_t offset = req->words[0];
    uint64_t len    = req->words[1];
    if (len > VFS_EP_DATA_MAX) len = VFS_EP_DATA_MAX;

    uint64_t bytes = 0;
    if (offset < exp->size) {
        uint64_t available = (uint64_t)exp->size - offset;
        bytes = (len < available) ? len : available;
        const uint8_t *src = vfs_ep_export_bytes(exp) + offset;
        for (uint64_t i = 0; i < bytes; i++) reply_buf[i] = src[i];
    }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = bytes;
    reply->words[2]   = exp->size;
    reply->word_count = 3u;
    if (bytes > 0) {
        reply->buf_len  = (uint32_t)bytes;
        reply->buf_uptr = (uint64_t)(uintptr_t)reply_buf;
    }
}

static void vfs_ep_handle_status(const struct vfs_export *exports,
                                 uint32_t export_count,
                                 const struct IrisMsg *req,
                                 struct IrisMsg *reply) {
    if (req->buf_len > 0u) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    uint64_t ready = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < export_count; i++) {
        if (!exports[i].ready) continue;
        ready++;
        bytes += exports[i].size;
    }

    vfs_ep_msg_clear(reply);
    reply->label      = IRIS_EP_REPLY_OK;
    reply->words[0]   = 0u;
    reply->words[1]   = ready;
    reply->words[2]   = bytes;
    reply->word_count = 3u;
}

void vfs_ep_dispatch(const struct vfs_export *exports, uint32_t export_count,
                     const struct IrisMsg *req, const uint8_t *req_buf,
                     struct IrisMsg *reply, uint8_t *reply_buf) {
    if (!reply) return;
    if (!req || !reply_buf || (!exports && export_count > 0u)) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    /* Payload announced but not delivered (receiver buffer copy failed). */
    if (req->buf_len > 0u && !req_buf) {
        vfs_ep_reply_err(reply, IRIS_ERR_INVALID_ARG);
        return;
    }

    switch (req->label) {
    case VFS_EP_OP_LIST:
        vfs_ep_handle_list(exports, export_count, req, reply, reply_buf);
        return;
    case VFS_EP_OP_STAT:
        vfs_ep_handle_stat(exports, export_count, req, req_buf, reply);
        return;
    case VFS_EP_OP_READ_AT:
        vfs_ep_handle_read_at(exports, export_count, req, req_buf, reply, reply_buf);
        return;
    case VFS_EP_OP_STATUS:
        vfs_ep_handle_status(exports, export_count, req, reply);
        return;
    case IRIS_EP_OP_PING:
        vfs_ep_msg_clear(reply);
        reply->label      = IRIS_EP_REPLY_OK;
        reply->words[0]   = 0u;
        /* Fase 9 PING convention: echo the kernel-stamped sender badge. */
        reply->words[1]   = req->sender_badge;
        reply->word_count = 2u;
        return;
    default:
        vfs_ep_reply_err(reply, IRIS_ERR_NOT_SUPPORTED);
        return;
    }
}
