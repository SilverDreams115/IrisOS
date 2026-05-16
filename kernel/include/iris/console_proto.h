#ifndef IRIS_CONSOLE_PROTO_H
#define IRIS_CONSOLE_PROTO_H

#define CONSOLE_PROTO_VERSION 1u
#define CONSOLE_MSG_WRITE     1u
/* KChanMsg.data is 64 bytes; 4 bytes for the little-endian length prefix
 * leaves 60 bytes for the character payload per message. */
#define CONSOLE_WRITE_MAX     60u

#endif
