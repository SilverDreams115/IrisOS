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

#ifdef __KERNEL__
/*
 * Kernel-internal VFS structures and API.
 *
 * RETIRED compatibility API.
 *
 * The healthy-path VFS implementation now lives entirely in the ring-3 `vfs`
 * service. These declarations remain only so the retired kernel ramfs/VFS
 * implementation files can stay in-tree during the transition to full
 * deletion. They are no longer built into the default kernel image.
 */
#include <stddef.h>

struct inode {
    char     name[VFS_MAX_NAME];
    uint32_t type;        /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    uint32_t size;        /* bytes used */
    uint32_t capacity;    /* bytes allocated */
    uint8_t *data;        /* pointer to content buffer */
    uint32_t valid;       /* 1 = in use */
};

struct file {
    struct inode *inode;
    uint32_t      pos;    /* read/write cursor */
    uint32_t      flags;
    uint32_t      valid;
};

void     vfs_init(void);
int32_t  vfs_open(const char *path, uint32_t flags);
int32_t  vfs_read(int32_t fd, void *buf, uint32_t len);
int32_t  vfs_write(int32_t fd, const void *buf, uint32_t len);
int32_t  vfs_close(int32_t fd);
int32_t  vfs_stat(const char *path, uint32_t *out_size);
int32_t  vfs_seek(int32_t fd, int32_t offset, uint32_t whence);
int32_t  vfs_mkdir(const char *path);
#endif /* __KERNEL__ */

#endif /* IRIS_VFS_H */
