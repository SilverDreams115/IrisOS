/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <unistd.h>

/*
 * What this system can say about a file, and nothing it cannot.
 *
 * `struct stat` on a POSIX system has a dozen fields, most of which answer
 * questions IRIS's filesystem does not have: there are no owners, no groups,
 * no link counts, no device numbers, and no timestamps, because nothing in
 * this system records any of them.  A structure with those fields present and
 * always zero would be a structure that INVITES a program to branch on them,
 * and every such branch would be wrong in the same silent way.
 *
 * So the fields here are the ones the VFS actually answers — the size, and
 * enough of a mode to say "a regular file you may read" — and a program that
 * needs more gets a compile error naming the field, which is the right place
 * to find out.
 */

#define S_IFMT   0170000u
#define S_IFREG  0100000u
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

/* The permission bits this system can produce.  A file is readable by whoever
 * holds a capability to it; there is no write path, so no write bit exists. */
#define S_IRUSR  0400u
#define S_IRGRP  0040u
#define S_IROTH  0004u

struct stat {
    off_t        st_size;    /* what the export says it is                 */
    unsigned int st_mode;    /* S_IFREG plus the read bits                 */
    off_t        st_offset;  /* NOT POSIX: where this descriptor is now.
                              * It is here because a descriptor IS an open
                              * file here and the service answers it in the
                              * same call — hiding it would mean a second
                              * round trip to `lseek` for a fact already in
                              * hand. */
};

int fstat(int fd, struct stat *st);

#endif /* _SYS_STAT_H */
