#ifndef IRIS_RAMFS_H
#define IRIS_RAMFS_H

#include <iris/vfs.h>

#define RAMFS_FILE_MAX_SIZE  4096   /* 4KB per file */

void          ramfs_init(void);
struct inode *ramfs_create(const char *name);
struct inode *ramfs_lookup(const char *name);

#endif
