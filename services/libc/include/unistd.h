/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
typedef long ssize_t;
typedef long off_t;

ssize_t write(int, const void *, size_t);
ssize_t read(int, void *, size_t);
int     close(int);
int     dup(int);
off_t   lseek(int, off_t, int);
_Noreturn void _exit(int);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/*
 * ── Not POSIX, and it is the whole point ────────────────────────────────────
 *
 * A descriptor here IS a capability, so `dup` is a derivation and the original
 * can be REVOKED — which destroys every duplicate, wherever it was passed.
 * POSIX has no word for that because POSIX descriptors are numbers, and a
 * number cannot be taken back.
 *
 * `__iris_revoke` is that operation.  It is spelled with the system's name on
 * it so nobody mistakes it for something portable.
 */
int __iris_revoke(int fd);
/* Do these two descriptors name the same OPEN FILE?  `dup` makes them; this
 * asks the kernel rather than comparing numbers. */
int __iris_same_file(int a, int b);
/* The badge the service stamped on this descriptor, or negative. */
long __iris_fd_badge(int fd);
#endif
