/*
 * vfs_ep_proto.h — VFS service protocol over KEndpoint (Fase 7.1).
 *
 * Wire format: struct IrisMsg (iris/ipc_msg.h) following the conventions in
 * iris/endpoint_proto.h. All operations are EP_CALL + SYS_REPLY round trips.
 *
 * Design notes:
 *   - The endpoint protocol is STATELESS: there is no open-file table on the
 *     EP path. Reads carry an explicit (path, offset, len) triple, so a dead
 *     client leaves no server-side state behind and no sender identity is
 *     required (IrisMsg carries no kernel-stamped sender id / badge yet).
 *   - This is the ONLY VFS protocol (Fase 7.5): the legacy stateful KChannel
 *     open/read/close protocol (iris/vfs_proto.h) was removed with its last
 *     clients; VFS no longer owns a legacy service channel.
 *   - Requests never transfer capabilities (EP_CALL forbids request-side cap
 *     transfer); replies never transfer capabilities either.
 *
 * Reply convention (see endpoint_proto.h):
 *   reply.label == IRIS_EP_REPLY_OK  → words[0] = 0, payload in words[1..]/kbuf
 *   reply.label == IRIS_EP_REPLY_ERR → words[0] = (uint64_t)(uint32_t)iris_error_t
 *
 * Error semantics (all ops):
 *   IRIS_ERR_INVALID_ARG   — malformed request (missing/oversized/non-NUL
 *                            path, missing words, undeliverable payload)
 *   IRIS_ERR_NOT_FOUND     — no export with that name / index out of range
 *   IRIS_ERR_NOT_SUPPORTED — unknown opcode
 */

#ifndef IRIS_VFS_EP_PROTO_H
#define IRIS_VFS_EP_PROTO_H

#include <stdint.h>
#include <iris/ipc_msg.h>

/* Service opcode range 0x0100–0xEFFF (endpoint_proto.h); VFS owns 0x01xx. */

/*
 * VFS_EP_OP_LIST — enumerate exports by visible index.
 *   Request:  words[0] = index, word_count >= 1. No bulk payload.
 *   Reply OK: words[1] = export size in bytes
 *             words[2] = name length (excluding NUL)
 *             kbuf     = NUL-terminated export name (buf_len = name_len + 1)
 *   Reply ERR: IRIS_ERR_NOT_FOUND when index >= number of ready exports
 *              (this is the normal end-of-listing condition).
 */
#define VFS_EP_OP_LIST     UINT64_C(0x0101)

/*
 * VFS_EP_OP_STAT — look up an export by name.
 *   Request:  kbuf = NUL-terminated path; 1 <= buf_len <= VFS_EP_PATH_MAX
 *             (buf_len includes the NUL).
 *   Reply OK: words[1] = export size in bytes.
 *   Reply ERR: IRIS_ERR_NOT_FOUND / IRIS_ERR_INVALID_ARG.
 */
#define VFS_EP_OP_STAT     UINT64_C(0x0102)

/*
 * VFS_EP_OP_READ_AT — stateless positional read.
 *   Request:  kbuf     = NUL-terminated path (as STAT)
 *             words[0] = byte offset
 *             words[1] = requested length (server clamps to VFS_EP_DATA_MAX)
 *             word_count >= 2.
 *   Reply OK: words[1] = bytes read (0 = EOF; offset >= size is EOF, not error)
 *             words[2] = total export size in bytes
 *             kbuf     = data (buf_len = bytes read)
 *   Reply ERR: IRIS_ERR_NOT_FOUND / IRIS_ERR_INVALID_ARG.
 *
 *   Note (EP_CALL buffer reuse): msg.buf_uptr is both the request payload
 *   (path) and the reply bulk destination (data) — the client must re-stage
 *   the path before every call.
 */
#define VFS_EP_OP_READ_AT  UINT64_C(0x0103)

/*
 * VFS_EP_OP_STATUS — service health/diagnostics summary (Fase 7.5).
 *   Request:  no words, no bulk payload required (extra words are ignored;
 *             a bulk payload is rejected as IRIS_ERR_INVALID_ARG).
 *   Reply OK: words[1] = number of ready exports
 *             words[2] = total exported bytes across ready exports
 *   The stateless protocol has no open-file table, so the legacy
 *   opens/capacity counters do not exist on this path.
 */
#define VFS_EP_OP_STATUS   UINT64_C(0x0104)

/* IRIS_EP_OP_PING (0xFF01, endpoint_proto.h) is also served: reply OK. */

/* Maximum path length including NUL (matches legacy VFS_MAX_NAME). */
#define VFS_EP_PATH_MAX    64u

/* Boot contract: number of exports seeded before "[VFS] ep ready" (the
 * static boot exports; initrd exports come on top). Checked by init's diag
 * invariant. Moved here from iris/vfs_proto.h (removed, Fase 7.5). */
#define VFS_BOOT_EXPORT_COUNT 4u

/* Maximum data bytes per READ_AT reply (one IPC bulk buffer). */
#define VFS_EP_DATA_MAX    IRIS_IPC_BUF_SIZE

/* Service name under which svcmgr publishes the VFS endpoint. */
#define VFS_EP_SVC_NAME    "vfs.ep"

#endif /* IRIS_VFS_EP_PROTO_H */
