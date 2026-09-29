/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fdprog/main.c — descriptors, and the property a number cannot have.
 *
 * Stage 10-run step 7's evidence.  It opens a file, reads it, duplicates the
 * descriptor, and then REVOKES the original — which destroys the duplicate,
 * in this process and anywhere else it had been passed.
 *
 * That last step is the whole reason this design exists.  Every system has
 * `dup`.  What no system with descriptor NUMBERS can do is take one back: a
 * number handed to somebody is a number they keep, and closing yours changes
 * nothing about theirs.  Here a descriptor IS a capability, so `dup` is a
 * derivation and revocation reaches the whole subtree — the holder's next read
 * fails because it has no capability, not because a table said so.
 *
 * The shared OFFSET is the other half, and it is not a detail: two duplicates
 * name one open file, so reading through one moves the other.  That is what
 * POSIX's `dup` has always meant, and here it is what it IS rather than
 * something the library arranges.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define OK              42
#define BAD_OPEN         1
#define BAD_READ         2
#define BAD_CONTENT      3
#define BAD_DUP          4
#define BAD_NOT_SAME     5   /* dup did not name the same open file        */
#define BAD_NOT_SHARED   6   /* ...or did not share its offset             */
#define BAD_SEEK         7
#define BAD_REVOKE       8
#define BAD_SURVIVED     9   /* the duplicate outlived the revoke          */
#define BAD_ORIGINAL    10   /* ...or the revoke took the original with it */
#define BAD_CLOSE       11

int main(void) {
    char buf[64];
    int  fd, fd2, n;
    long off;

    /* ── a file, by name, once ── */
    fd = open("iris.txt", O_RDONLY);
    if (fd < 0) return BAD_OPEN;

    /* The descriptor is a CAPABILITY, and it is badged with the open file it
     * names — stamped by the kernel, unforgeable, and the reason none of the
     * calls below carries a path or an index. */
    {
        long badge = __iris_fd_badge(fd);
        if (badge < 0) return BAD_OPEN;
        printf("[FDPROG] fd %d is a capability badged 0x%lx\n", fd,
               (unsigned long)badge);
    }
    n = (int)read(fd, buf, 16);
    if (n <= 0) { return BAD_READ; }
    buf[n] = '\0';
    if (strncmp(buf, "Hello", 5) != 0) return BAD_CONTENT;

    /* ── dup: a derivation, not a number ── */
    fd2 = dup(fd);
    if (fd2 < 0) return BAD_DUP;
    if (!__iris_same_file(fd, fd2)) return BAD_NOT_SAME;

    /*
     * One open file, one offset.  Reading through the duplicate continues
     * where the original left off — which is only true because they are two
     * capabilities to one object, and not two entries in a table.
     */
    off = lseek(fd2, 0, SEEK_CUR);
    if (off != (long)n) return BAD_NOT_SHARED;
    {
        int m = (int)read(fd2, buf, 8);
        if (m <= 0) return BAD_READ;
        if (lseek(fd, 0, SEEK_CUR) != (long)(n + m)) return BAD_NOT_SHARED;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) return BAD_SEEK;

    printf("[FDPROG] fd=%d dup=%d name one open file, offset shared\n", fd, fd2);

    /*
     * ── and now take it back ──
     *
     * Revoking the original destroys every capability derived from it.  The
     * duplicate is gone: not closed, not marked — GONE, from a CSpace this
     * program does not even have to name.  Its next read fails because there
     * is no capability in that slot, which is a different kind of failure from
     * "the file is closed" and is the one worth having.
     */
    {
        int destroyed = __iris_revoke(fd);
        if (destroyed < 1) return BAD_REVOKE;
        if (read(fd2, buf, 8) >= 0) return BAD_SURVIVED;
        /* ...and the original is untouched: revoke removes the subtree, not
         * the root.  A revoke that took its own root with it would be a
         * different operation with a much less useful meaning. */
        if (read(fd, buf, 8) <= 0) return BAD_ORIGINAL;
        printf("[FDPROG] revoke destroyed %d duplicate, the original still reads\n",
               destroyed);
    }

    if (close(fd) != 0) return BAD_CLOSE;
    /* Closing a descriptor whose capability was already destroyed is not an
     * error to this program — it has nothing left to release. */
    (void)close(fd2);

    printf("[FDPROG] every claim held\n");
    return OK;
}
