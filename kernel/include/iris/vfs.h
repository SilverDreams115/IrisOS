#ifndef IRIS_VFS_H
#define IRIS_VFS_H

#include <stdint.h>

/*
 * VFS constants shared between kernel and userland services.
 * These constants define the namespace visible to both the kernel VFS
 * backend and the userland VFS service.
 */
#define VFS_MAX_NAME    64
#define VFS_MAX_FILES   32
#define VFS_MAX_FDS     16

#define VFS_TYPE_FILE   0
#define VFS_TYPE_DIR    1

#define VFS_O_READ      0x01
#define VFS_O_WRITE     0x02
#define VFS_O_CREATE    0x04

#define VFS_SEEK_SET 0  /* from start */
#define VFS_SEEK_CUR 1  /* from current pos */
#define VFS_SEEK_END 2  /* from end */

#endif /* IRIS_VFS_H */
