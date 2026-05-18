#ifndef IRIS_CONSOLE_PROTO_H
#define IRIS_CONSOLE_PROTO_H

/*
 * Ring-3 serial console protocol.
 *
 * Current contract:
 *   - clients send CONSOLE_MSG_WRITE over a KChannel handle with RIGHT_WRITE
 *   - the protocol does not use attached handles
 *   - each message carries a little-endian payload length followed by bytes
 *   - writes are best-effort message deliveries; acknowledgement is not part
 *     of the current protocol
 */

#define CONSOLE_PROTO_VERSION 1u
#define CONSOLE_MSG_WRITE     1u
/* KChanMsg.data is 64 bytes; 4 bytes for the little-endian length prefix
 * leaves 60 bytes for the character payload per message. */
#define CONSOLE_WRITE_MAX     60u

#endif
