#ifndef IRIS_CONSOLE_EP_PROTO_H
#define IRIS_CONSOLE_EP_PROTO_H

#include <iris/endpoint_proto.h>

/*
 * console_ep_proto.h — KEndpoint protocol for the serial console (Fase 7.3).
 *
 * Wire format: struct IrisMsg (iris/ipc_msg.h), msg.label = opcode.
 * This is the ONLY console write path.  It fully replaced the legacy
 * CONSOLE_MSG_WRITE/SYNC KChannel protocol (iris/console_proto.h), which is
 * retired and non-functional (Fase 13/Track G).  All writers — init, sh, vfs,
 * iris_test, and svcmgr's klog drain — use this endpoint.
 *
 * CONSOLE_EP_OP_WRITE — synchronous write.
 *   Request:  bulk payload = raw bytes (buf_len = length, up to
 *             IRIS_IPC_BUF_SIZE). An empty write (buf_len == 0) is a no-op
 *             and replies OK.
 *   Reply OK: sent only after every byte has been emitted to the UART —
 *             the call itself is a per-write flush barrier.
 *
 * CONSOLE_EP_OP_SYNC — flush barrier.
 *   Request:  no payload; a bulk payload is rejected (IRIS_ERR_INVALID_ARG).
 *   Reply OK: sent once the console has flushed all output queued at that
 *             moment.  Retained as an explicit barrier primitive; with the
 *             legacy KChannel writers gone, EP writes are already synchronous
 *             by construction, so a plain WRITE is self-flushing.
 *
 * IRIS_EP_OP_PING is served: reply OK.
 * Unknown opcodes → IRIS_ERR_NOT_SUPPORTED. Exactly one reply per request.
 */

#define CONSOLE_EP_OP_WRITE UINT64_C(0x0301)
#define CONSOLE_EP_OP_SYNC  UINT64_C(0x0302)

/* Service name published by svcmgr (send side delivered by init at svcmgr
 * bootstrap, kind SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP — endpoint_proto.h). */
#define CONSOLE_EP_SVC_NAME "console.ep"

#endif /* IRIS_CONSOLE_EP_PROTO_H */
