/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _ERRNO_H
#define _ERRNO_H

/*
 * errno — why the last call failed, when a return value can only say THAT it
 * did.
 *
 * Before this existed every function in this library answered -1 and nothing
 * else, so a program could tell an open that found no file from one that was
 * refused the authority, or from one that ran out of descriptors, only by
 * guessing.  Three different mistakes with one answer is three mistakes nobody
 * can act on.
 *
 * ── It is a function, not a variable ───────────────────────────────────────
 *
 * `errno` expands to a dereference of `__errno_location()`, which is how every
 * modern C library spells it and is not a formality here: this system has
 * threads, and the day a program has two of them the per-thread storage goes
 * behind that call and nothing that reads `errno` has to change.  Today it
 * returns the address of one word, and `libc_lock.h` records that as one of
 * the places where "one thread" is still assumed.
 *
 * ── The numbers ────────────────────────────────────────────────────────────
 *
 * The values are the Linux/POSIX ones, because a program that prints `errno`
 * or compares it against `ENOENT` should not have to learn a second table, and
 * because nothing is gained by inventing one.  The SET is small: only the
 * conditions this system can actually produce appear, and `libc_errno.c` maps
 * each `IRIS_ERR_*` to exactly one of them in a single place, so a service
 * that starts returning a new refusal changes one switch rather than every
 * caller.
 */

int *__errno_location(void);
#define errno (*__errno_location())

#define EPERM            1   /* the capability did not carry the right      */
#define ENOENT           2   /* the service answered NOT_FOUND              */
#define EIO              5   /* the call itself failed to be delivered      */
#define EBADF            9   /* not a descriptor of ours                    */
#define EAGAIN          11   /* WOULD_BLOCK                                 */
#define ENOMEM          12   /* the budget is spent — see malloc.c          */
#define EACCES          13   /* ACCESS_DENIED                               */
#define EBUSY           16   /* BUSY                                        */
#define EEXIST          17   /* ALREADY_EXISTS                              */
#define EINVAL          22   /* INVALID_ARG, and every argument this refuses */
#define ENFILE          23   /* no free descriptor slot in the CSpace       */
#define EMFILE          24   /* ...same limit, reported the POSIX way       */
#define ESPIPE          29   /* seeking something that is not a file        */
#define EROFS           30   /* there is no write path to a file            */
#define ERANGE          34   /* OVERFLOW                                    */
#define ENAMETOOLONG    36   /* the path is longer than the protocol carries */
#define ENOSYS          38   /* NOT_SUPPORTED                               */
#define ETIMEDOUT      110   /* TIMED_OUT                                   */
#define ECONNRESET     104   /* CLOSED: the other end is gone               */

#endif /* _ERRNO_H */
