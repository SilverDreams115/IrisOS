#include <iris/ramfs.h>
#include <iris/vfs.h>

/* static storage: 32 inodes, each with 4KB buffer */
static uint8_t      ramfs_data[VFS_MAX_FILES][RAMFS_FILE_MAX_SIZE];
static struct inode ramfs_inodes[VFS_MAX_FILES];

static int ramfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void ramfs_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void ramfs_init(void) {
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        ramfs_inodes[i].valid    = 0;
        ramfs_inodes[i].data     = ramfs_data[i];
        ramfs_inodes[i].size     = 0;
        ramfs_inodes[i].capacity = RAMFS_FILE_MAX_SIZE;
    }
}

struct inode *ramfs_create(const char *name) {
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!ramfs_inodes[i].valid) {
            ramfs_strncpy(ramfs_inodes[i].name, name, VFS_MAX_NAME);
            ramfs_inodes[i].type  = VFS_TYPE_FILE;
            ramfs_inodes[i].size  = 0;
            ramfs_inodes[i].valid = 1;
            return &ramfs_inodes[i];
        }
    }
    return (void *)0;  /* full */
}

struct inode *ramfs_lookup(const char *name) {
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (ramfs_inodes[i].valid &&
            ramfs_strcmp(ramfs_inodes[i].name, name) == 0)
            return &ramfs_inodes[i];
    }
    return (void *)0;
}

struct inode *ramfs_mkdir(const char *name) {
    /* check if already exists */
    if (ramfs_lookup(name)) return (void *)0;
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!ramfs_inodes[i].valid) {
            ramfs_strncpy(ramfs_inodes[i].name, name, VFS_MAX_NAME);
            ramfs_inodes[i].type  = VFS_TYPE_DIR;
            ramfs_inodes[i].size  = 0;
            ramfs_inodes[i].valid = 1;
            return &ramfs_inodes[i];
        }
    }
    return (void *)0;
}

/* lookup file 'name' inside directory 'dir' using path 'dir/name' */
struct inode *ramfs_lookup_in(const char *dir, const char *name) {
    char path[VFS_MAX_NAME];
    int di = 0, ni = 0;
    while (dir[di] && di < VFS_MAX_NAME - 2) { path[di] = dir[di]; di++; }
    path[di++] = '/';
    while (name[ni] && di < VFS_MAX_NAME - 1) { path[di++] = name[ni++]; }
    path[di] = '\0';
    return ramfs_lookup(path);
}
