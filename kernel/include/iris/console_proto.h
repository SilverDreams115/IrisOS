#ifndef IRIS_CONSOLE_PROTO_H
#define IRIS_CONSOLE_PROTO_H

/*
 * Ring-3 serial console protocol.
 *
 * Current contract:
 *   - clients send CONSOLE_MSG_WRITE over a KChannel handle with RIGHT_WRITE
 *   - CONSOLE_MSG_WRITE does not use attached handles
 *   - each message carries a little-endian payload length followed by bytes
 *   - writes are best-effort message deliveries; acknowledgement is not part
 *     of the current protocol
 *
 * Flush barrier (Fase 7.1 ABI extension, protocol version 2):
 *   - CONSOLE_MSG_SYNC carries an attached KChannel cap (RIGHT_WRITE).  The
 *     service channel is a FIFO ring, so by the time the console dequeues the
 *     SYNC every preceding CONSOLE_MSG_WRITE has already been emitted to the
 *     UART.  The console then sends one CONSOLE_MSG_SYNC_ACK on the attached
 *     cap and closes it.
 *   - Clients use this to serialize their queued console output against raw
 *     serial writers (e.g. init drains its log backlog before spawning
 *     iris_test, so gated markers cannot be interleaved mid-line).
 *   - A SYNC without an attached cap is ignored.  Old consoles drop unknown
 *     message types, so the barrier degrades to a no-op (callers must use a
 *     timeout when waiting for the ack).
 */

#define CONSOLE_PROTO_VERSION 2u
#define CONSOLE_MSG_WRITE     1u
#define CONSOLE_MSG_SYNC      2u
#define CONSOLE_MSG_SYNC_ACK  3u
/* KChanMsg.data is 64 bytes; 4 bytes for the little-endian length prefix
 * leaves 60 bytes for the character payload per message. */
#define CONSOLE_WRITE_MAX     60u

#endif
