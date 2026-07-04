#ifndef IRIS_CONSOLE_PROTO_H
#define IRIS_CONSOLE_PROTO_H

/*
 * Ring-3 serial console protocol — RETIRED (historical reference only).
 *
 * This was the KChannel-based console write path.  It is NON-FUNCTIONAL: the
 * KChannel object was removed and every SYS_CHAN_* syscall returns
 * IRIS_ERR_NOT_SUPPORTED (Fase 13/Track G).  The console is endpoint-only —
 * all clients (init, sh, vfs, iris_test, and svcmgr's klog drain) use
 * CONSOLE_EP_OP_WRITE/SYNC over "console.ep" (iris/console_ep_proto.h).
 *
 * The opcode/length constants below are retained only so historical wire
 * layouts stay documented; no live code sends or receives them.
 *
 * Historical contract (no longer served):
 *   - clients sent CONSOLE_MSG_WRITE over a KChannel handle with RIGHT_WRITE;
 *     each message carried a little-endian length prefix followed by bytes.
 *   - CONSOLE_MSG_SYNC carried an attached cap and produced a
 *     CONSOLE_MSG_SYNC_ACK flush barrier.
 */

#define CONSOLE_PROTO_VERSION 2u
#define CONSOLE_MSG_WRITE     1u
#define CONSOLE_MSG_SYNC      2u
#define CONSOLE_MSG_SYNC_ACK  3u
/* KChanMsg.data is 64 bytes; 4 bytes for the little-endian length prefix
 * leaves 60 bytes for the character payload per message. */
#define CONSOLE_WRITE_MAX     60u

#endif
