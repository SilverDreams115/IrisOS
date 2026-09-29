/* SPDX-License-Identifier: Apache-2.0 */
/*
 * errno.c — one place where a refusal becomes a number a program can read.
 *
 * Every function in this library that can fail goes through `__libc_fail`.
 * That is the whole discipline, and it is worth stating why it is a discipline
 * rather than a convenience: a library that sets `errno` at each call site sets
 * it in some of them, and the ones it misses report whatever the LAST failure
 * was — which is worse than reporting nothing, because it looks like an answer.
 *
 * The mapping is a single switch.  A service that begins returning a refusal
 * this table does not know falls to `EIO`, which is honest: something went
 * wrong in the call and this library cannot say what.
 */
#include <errno.h>
#include "libc_internal.h"
#include <iris/nc/error.h>

/*
 * One word, and the function that hands out its address.
 *
 * The indirection is the seam for per-thread storage: when a program here has
 * two threads, this returns a slot in the thread's own block and nothing that
 * reads `errno` changes.  `libc_lock.h` lists it with the rest of what still
 * assumes one thread.
 */
static int g_errno;

int *__errno_location(void) { return &g_errno; }

int __libc_errno_from(long e) {
    switch (e) {
        case IRIS_ERR_INVALID_ARG:    return EINVAL;
        case IRIS_ERR_NO_MEMORY:      return ENOMEM;
        case IRIS_ERR_NOT_FOUND:      return ENOENT;
        case IRIS_ERR_BAD_HANDLE:     return EBADF;
        case IRIS_ERR_ACCESS_DENIED:  return EACCES;
        case IRIS_ERR_TABLE_FULL:     return ENFILE;
        case IRIS_ERR_ALREADY_EXISTS: return EEXIST;
        case IRIS_ERR_BUSY:           return EBUSY;
        case IRIS_ERR_WRONG_TYPE:     return EBADF;
        case IRIS_ERR_OVERFLOW:       return ERANGE;
        case IRIS_ERR_NOT_SUPPORTED:  return ENOSYS;
        case IRIS_ERR_CLOSED:         return ECONNRESET;
        case IRIS_ERR_WOULD_BLOCK:    return EAGAIN;
        case IRIS_ERR_TIMED_OUT:      return ETIMEDOUT;
        /* INTERNAL, MISSING_TABLE and anything added later: the call failed
         * and this library cannot say more than that. */
        default:                      return EIO;
    }
}

long __libc_fail(int e) {
    g_errno = e;
    return -1;
}

long __libc_fail_iris(long iris_err) {
    g_errno = __libc_errno_from(iris_err);
    return -1;
}

/*
 * The text.
 *
 * Short, and describing what happened in THIS system rather than reciting the
 * POSIX wording: "permission denied" is what Linux says for EACCES, and here
 * the thing that was denied is a capability, which is a more useful sentence
 * for somebody reading a program's output on this machine.
 */
const char *strerror(int e) {
    switch (e) {
        case 0:           return "no error";
        case EPERM:       return "the capability does not carry that right";
        case ENOENT:      return "no such file";
        case EIO:         return "the call failed";
        case EBADF:       return "not a descriptor";
        case EAGAIN:      return "would block";
        case ENOMEM:      return "the budget is spent";
        case EACCES:      return "access denied";
        case EBUSY:       return "busy";
        case EEXIST:      return "already exists";
        case EINVAL:      return "invalid argument";
        case ENFILE:      /* fall through: one limit, two POSIX spellings */
        case EMFILE:      return "no free descriptor";
        case ESPIPE:      return "not seekable";
        case EROFS:       return "there is no write path to a file";
        case ERANGE:      return "out of range";
        case ENOSYS:      return "not supported";
        case ETIMEDOUT:   return "timed out";
        case ECONNRESET:  return "the other end is gone";
        default:          return "unknown error";
    }
}
