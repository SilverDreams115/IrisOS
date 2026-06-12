/*
 * vfs_ep.h — VFS endpoint-protocol dispatcher (Fase 7.1).
 *
 * The dispatcher is a pure function over (exports, request) → reply with no
 * syscalls inside, so the host unit-test harness (tests/kernel) can exercise
 * the full opcode/validation surface without a kernel. vfs.c wraps it with
 * the EP_NB_RECV / SYS_REPLY drain loop.
 */
#ifndef IRIS_SERVICES_VFS_EP_H
#define IRIS_SERVICES_VFS_EP_H

#include <stdint.h>
#include <iris/ipc_msg.h>
#include <iris/vfs_ep_proto.h>

/* One exported file. Shared by vfs.c (owner) and the dispatcher (reader).
 * is_mapped exports read through virt_base (initrd VMO mappings); inline
 * exports read from data[]. */
struct vfs_export {
    char     name[VFS_EP_PATH_MAX];
    uint8_t  data[512];
    uint32_t size;
    uint8_t  ready;
    uint8_t  is_mapped;
    uint64_t virt_base;
};

/*
 * vfs_ep_dispatch — handle one endpoint request.
 *
 *   exports/export_count: the export table (entries with ready=0 are skipped).
 *   req:       received IrisMsg (label, words, buf_len as delivered).
 *   req_buf:   bulk payload bytes for req (valid for req->buf_len bytes when
 *              the payload was delivered; pass NULL if it was not).
 *   reply:     fully written by the dispatcher (label/words/word_count/buf_*).
 *   reply_buf: VFS_EP_DATA_MAX-byte buffer for reply bulk data; on return
 *              reply->buf_uptr points at it iff reply->buf_len > 0.
 *
 * Exactly one reply is produced for every request, including malformed ones.
 */
void vfs_ep_dispatch(const struct vfs_export *exports, uint32_t export_count,
                     const struct IrisMsg *req, const uint8_t *req_buf,
                     struct IrisMsg *reply, uint8_t *reply_buf);

#endif /* IRIS_SERVICES_VFS_EP_H */
