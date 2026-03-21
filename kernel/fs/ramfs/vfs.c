#include <iris/vfs.h>
#include <iris/ramfs.h>

static struct file fd_table[VFS_MAX_FDS];

static int vfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void vfs_init(void) {
    ramfs_init();
    for (int i = 0; i < VFS_MAX_FDS; i++)
        fd_table[i].valid = 0;
}

int32_t vfs_open(const char *path, uint32_t flags) {
    struct inode *node = ramfs_lookup(path);

    if (!node) {
        if (flags & VFS_O_CREATE)
            node = ramfs_create(path);
        if (!node) return -1;
    }

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].valid) {
            fd_table[i].inode = node;
            fd_table[i].pos   = 0;
            fd_table[i].flags = flags;
            fd_table[i].valid = 1;
            return i;
        }
    }
    return -1;  /* no free fds */
}

int32_t vfs_read(int32_t fd, void *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].valid) return -1;
    struct file  *f = &fd_table[fd];
    struct inode *n = f->inode;
    if (f->pos >= n->size) return 0;
    uint32_t avail = n->size - f->pos;
    uint32_t count = (len < avail) ? len : avail;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++)
        dst[i] = n->data[f->pos + i];
    f->pos += count;
    return (int32_t)count;
}

int32_t vfs_write(int32_t fd, const void *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].valid) return -1;
    struct file  *f = &fd_table[fd];
    struct inode *n = f->inode;
    if (!(f->flags & VFS_O_WRITE)) return -1;
    uint32_t space = n->capacity - f->pos;
    uint32_t count = (len < space) ? len : space;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++)
        n->data[f->pos + i] = src[i];
    f->pos += count;
    if (f->pos > n->size) n->size = f->pos;
    return (int32_t)count;
}

int32_t vfs_close(int32_t fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].valid) return -1;
    fd_table[fd].valid = 0;
    return 0;
}

int32_t vfs_stat(const char *path, uint32_t *out_size) {
    struct inode *node = ramfs_lookup(path);
    if (!node) return -1;
    if (out_size) *out_size = node->size;
    return 0;
}
