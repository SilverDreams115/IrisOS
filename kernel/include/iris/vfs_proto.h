#ifndef IRIS_VFS_PROTO_H
#define IRIS_VFS_PROTO_H

/*
 * Userland VFS protocol.
 *
 * Phase 9/current:
 *   - Clients talk to the ring-3 vfs service over the public request channel.
 *   - The service owns the client-visible file_id namespace and per-client
 *     open-file state for the migrated read-only open/read/close path.
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

#define VFS_MSG_OPEN_REPLY  0x80010001u
#define VFS_MSG_READ_REPLY  0x80010002u
#define VFS_MSG_CLOSE_REPLY 0x80010003u
#define VFS_MSG_RECLAIM_PROBE_REPLY 0x80010004u

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

#define VFS_MSG_OPEN_MIN_LEN        5u
#define VFS_MSG_OPEN_REPLY_LEN      8u
#define VFS_MSG_READ_LEN            8u
#define VFS_MSG_READ_REPLY_BASE_LEN 12u
#define VFS_MSG_CLOSE_LEN           4u
#define VFS_MSG_CLOSE_REPLY_LEN     8u
#define VFS_MSG_RECLAIM_PROBE_LEN   0u
#define VFS_MSG_RECLAIM_PROBE_REPLY_LEN 4u

#define VFS_SERVICE_OPEN_FILES      16u

#endif
