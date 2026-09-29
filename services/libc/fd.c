/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fd.c — descriptors, which are capabilities.
 *
 * ── A file descriptor IS a CSpace slot ─────────────────────────────────────
 *
 * `open` asks the VFS for a file and receives a CAPABILITY, badged by the VFS
 * with the open file it names.  The slot that capability lands in is the
 * descriptor — the number a C program passes around is a CSpace index, and
 * there is no table beside it that could get out of step, because the CSpace
 * IS the table.
 *
 * Everything follows from that rather than from code here:
 *
 *   - `dup(fd)` is `CSpace_Mint`.  The copy is an MDB child of the original
 *     and carries the same badge, so it names the same OPEN FILE — the same
 *     offset, the same rights.  That is what `dup` has always meant; here it
 *     is what it is.
 *   - `close(fd)` is `CNode_Delete`.
 *   - `__iris_revoke(fd)` is `CSpace_Revoke`, and it **destroys every
 *     duplicate**, wherever it was passed — in this process or in a child it
 *     was handed to.  POSIX has no word for that because a number cannot be
 *     taken back.
 *
 * ── The three magic numbers ────────────────────────────────────────────────
 *
 * POSIX fixes 0, 1 and 2, and those cannot be CSpace slots: slot 0 is the root
 * CNode and 1..3 are the capabilities the program contract puts there.  So
 * exactly three entries redirect, and nothing else does — a table of three, to
 * honour a convention older than this system, and every other descriptor is
 * its own slot.
 *
 * `stdout` and `stderr` are the CONSOLE, which is an endpoint and not a file.
 * `stdin` is nothing: this system has no path from a keyboard to a program
 * that did not ask the keyboard service itself, and returning EOF is the
 * honest version of that.
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "libc_internal.h"
#include "libc_slots.h"
#include "libc_lock.h"
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>

/* Where descriptors live: the runtime's half of the program's slot range.
 * `libc_slots.h` states the whole of it, and the contract states the split. */
#define FD_LO  ((int)LIBC_SLOT_FD_LO)
#define FD_HI  ((int)LIBC_SLOT_FD_HI)

/*
 * The three POSIX numbers, and nothing more.
 *
 * Each names something that is NOT a CSpace slot, so each gets its own
 * sentinel rather than sharing "-1 = nothing there".  That distinction is
 * load-bearing: `stdin` and an unopened descriptor both used to resolve to -1,
 * and `read` cannot tell "there is nothing to read" from "that is not a
 * descriptor" once they are the same number.  It answered the first as an
 * ERROR, contradicting this file's own account of it.
 */
#define FD_CONSOLE (-2)          /* an endpoint, not a file                 */
#define FD_EOF     (-3)          /* nothing at all, and says so as EOF      */
static int g_std[3] = { FD_EOF, FD_CONSOLE, FD_CONSOLE };

static int fd_is_slot(int fd) { return fd >= FD_LO && fd <= FD_HI; }

/* Resolve a descriptor to what it actually is. */
static int fd_resolve(int fd) {
    if (fd >= 0 && fd < 3) return g_std[fd];
    return fd_is_slot(fd) ? fd : -1;
}

/*
 * Two threads scanning for a free slot find the SAME one: the scan and the
 * install are not one operation, and between them the slot is free to both.
 * The loser's retype or mint then fails with ALREADY_EXISTS, which reaches the
 * program as a mysterious refusal of a perfectly good `open`.
 *
 * The lock covers the scan; the caller holds it across the install too, which
 * is why this returns with it HELD and `fd_alloc_done` exists.
 */
static struct libc_lock g_fd_lock = LIBC_LOCK_INIT;

static int fd_alloc(void) {
    __libc_lock(&g_fd_lock);
    for (int s = FD_LO; s <= FD_HI; s++) {
        /* An empty slot is one nothing identifies.  Asking the kernel is the
         * only account of what a CSpace holds that cannot be stale — a
         * bitmap here would be a second table, which is the thing this design
         * does not have. */
        if (iris_invoke0((long)s, INV_CAP_IDENTIFY) < 0) return s;
    }
    __libc_unlock(&g_fd_lock);
    return -1;
}

/* Release what `fd_alloc` took.  Called once the slot is filled — or once the
 * attempt to fill it has failed, which is the case that must not be missed. */
static void fd_alloc_done(void) { __libc_unlock(&g_fd_lock); }

/*
 * One call on a file capability, answering WHAT went wrong.
 *
 * Two different failures used to come back as the same -1: the call not
 * reaching the service at all, and the service refusing it.  They mean
 * opposite things to a program — the first is a descriptor that is no longer
 * a descriptor, the second is an answer — so the error is carried through.
 * `iris/endpoint_proto.h` fixes the convention: an ERR reply puts the
 * `iris_error_t` in `words[0]`.
 */
static long fd_call(int slot, uint64_t label, uint64_t w0, uint64_t w1,
                    struct iris_msg *out) {
    long r;
    iris_msg_zero(out);
    out->label      = label;
    out->words[0]   = w0;
    out->words[1]   = w1;
    out->word_count = 2u;
    r = iris_msg_call((long)slot, out);
    if (r != 0) return r;                    /* it never got there */
    if (out->label != IRIS_EP_REPLY_OK)
        return (long)(int32_t)(uint32_t)out->words[0];   /* it was refused */
    return 0;
}

int open(const char *path, int flags, ...) {
    struct iris_msg m;
    uint32_t n;
    int slot;

    (void)flags;                     /* read-only: see <fcntl.h> */
    if (!path) return (int)__libc_fail(EINVAL);
    /* No IPC buffer means no way to name the path at all — the program was
     * started without a budget to build one out of. */
    if (!__libc_ipc_buf()) return (int)__libc_fail(EIO);
    n = (uint32_t)strlen(path);
    if (n == 0u) return (int)__libc_fail(EINVAL);
    if (n + 1u > VFS_EP_PATH_MAX) return (int)__libc_fail(ENAMETOOLONG);

    slot = fd_alloc();
    if (slot < 0) return (int)__libc_fail(EMFILE);

    iris_msg_zero(&m);
    m.label      = VFS_EP_OP_FILE_OPEN;
    m.words[0]   = VFS_FILE_RIGHT_STAT | VFS_FILE_RIGHT_READ |
                   VFS_FILE_RIGHT_DUPLICATE;
    m.word_count = 1u;
    memcpy(__libc_ipc_buf(), path, n);
    __libc_ipc_buf()[n] = 0u;
    m.buf_len    = n + 1u;
    m.recv_slot  = (long)slot;       /* the capability lands HERE, and that
                                      * slot number is the descriptor */

    /*
     * One exit, because `fd_alloc` returns holding the lock and every path
     * from here has to give it back.  The reservation is only really a
     * reservation while it is held: the slot is still empty until the reply
     * installs a capability in it, so another thread scanning would find it.
     */
    {
        long r = iris_msg_call((long)IRIS_CPTR_VFS_EP, &m);
        int err = 0;
        if (r != 0) err = __libc_errno_from(r);
        else if (m.label != IRIS_EP_REPLY_OK)
            err = __libc_errno_from((long)(int32_t)(uint32_t)m.words[0]);
        /* A reply with no capability is not a reply: there is nothing for the
         * descriptor to BE.  The service said yes and handed over nothing,
         * which is a fault in the service, not a refusal of this call. */
        else if (m.got_caps == 0u) err = EIO;
        fd_alloc_done();
        if (err) return (int)__libc_fail(err);
    }
    return slot;
}

ssize_t read(int fd, void *buf, size_t n) {
    struct iris_msg m;
    int slot = fd_resolve(fd);

    if (!buf) return (ssize_t)__libc_fail(EINVAL);
    /*
     * Neither of the two non-file descriptors reads back, and both say so as
     * END OF FILE rather than as an error: a program that reads until zero
     * terminates, and one that treats -1 as fatal would not.
     *
     * `stdin` is nothing because this system has no path from a keyboard to a
     * program that did not ask the keyboard service itself; the console is
     * write-only because it is an endpoint into a service, not a stream.
     */
    if (slot == FD_EOF || slot == FD_CONSOLE) return 0;
    if (!fd_is_slot(slot)) return (ssize_t)__libc_fail(EBADF);
    if (n == 0u) return 0;
    if (n > VFS_EP_DATA_MAX) n = VFS_EP_DATA_MAX;

    { long r = fd_call(slot, VFS_EP_OP_FILE_READ, n, 0, &m);
      if (r != 0) return (ssize_t)__libc_fail_iris(r); }
    {
        uint32_t got = (uint32_t)m.words[1];
        /* More than was asked for is a service that cannot be believed about
         * the rest of the reply either. */
        if (got > n) return (ssize_t)__libc_fail(EIO);
        if (got) memcpy(buf, __libc_ipc_buf(), got);
        return (ssize_t)got;
    }
}

ssize_t write(int fd, const void *buf, size_t n) {
    int slot = fd_resolve(fd);
    /*
     * There is no write path to a FILE in this system, and this returns -1
     * rather than pretending.  What a file's bytes ARE is a memory question
     * — a service's private state, or a frame a holder maps — and answering
     * it is Stage 11's business, not a descriptor's.
     */
    if (!buf) return (ssize_t)__libc_fail(EINVAL);
    if (slot == FD_EOF)     return (ssize_t)__libc_fail(EBADF);
    if (!fd_is_slot(slot) && slot != FD_CONSOLE)
        return (ssize_t)__libc_fail(EBADF);
    /* A FILE refuses, and says which refusal it is: there is no write path in
     * this system, not "you may not write to this one". */
    if (slot != FD_CONSOLE) return (ssize_t)__libc_fail(EROFS);
    __libc_console_write((const char *)buf, n);
    return (ssize_t)n;
}

int close(int fd) {
    int slot = fd_resolve(fd);
    struct iris_msg m;

    /* Closing one of the three POSIX numbers succeeds and does nothing: they
     * name no capability of ours, so there is none to delete. */
    if (slot == FD_CONSOLE || slot == FD_EOF) return 0;
    if (!fd_is_slot(slot)) return (int)__libc_fail(EBADF);

    /*
     * Tell the service, THEN delete the slot.
     *
     * Deleting the capability is what closes the descriptor; the service's own
     * object is freed by asking, because a server cannot see how many
     * capabilities to it exist.  seL4 has the same gap and the same answer:
     * the holder says when it is finished.  A program that only deleted its
     * slot would leave one bounded, visible table entry behind — not memory.
     */
    (void)fd_call(slot, VFS_EP_OP_FILE_CLOSE, 0, 0, &m);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
    return 0;
}

int dup(int fd) {
    int slot = fd_resolve(fd);
    int to;

    if (!fd_is_slot(slot)) return (int)__libc_fail(EBADF);
    to = fd_alloc();
    if (to < 0) return (int)__libc_fail(EMFILE);

    /*
     * A real derivation, in this program's own CSpace.
     *
     * `RIGHT_SAME_RIGHTS` is not asked for: a mint reduces, and asking for
     * exactly what the source has is how a duplicate stays a duplicate.  The
     * badge is inherited — the kernel's no-re-badge rule guarantees it — so
     * the copy names the same open file, with the same offset, which is what
     * `dup` means.
     */
    {
        long r = iris_invoke2((long)slot, INV_CSPACE_MINT,
                              (long)((uint64_t)to << 32),
                              (long)(RIGHT_WRITE | RIGHT_DUPLICATE |
                                     RIGHT_TRANSFER));
        fd_alloc_done();
        if (r != 0) return (int)__libc_fail_iris(r);
    }
    return to;
}

off_t lseek(int fd, off_t off, int whence) {
    struct iris_msg m;
    int slot = fd_resolve(fd);

    /* Seeking something that is not a file is ESPIPE, which is the answer a
     * program checks for before deciding it must read forwards instead. */
    if (slot == FD_CONSOLE || slot == FD_EOF) return (off_t)__libc_fail(ESPIPE);
    if (!fd_is_slot(slot)) return (off_t)__libc_fail(EBADF);
    if (whence < 0 || whence > SEEK_END) return (off_t)__libc_fail(EINVAL);
    { long r = fd_call(slot, VFS_EP_OP_FILE_SEEK, (uint64_t)off,
                       (uint64_t)whence, &m);
      if (r != 0) return (off_t)__libc_fail_iris(r); }
    return (off_t)m.words[1];
}

int __iris_revoke(int fd) {
    int slot = fd_resolve(fd);
    if (!fd_is_slot(slot)) return (int)__libc_fail(EBADF);
    /*
     * Every capability DERIVED from this one is destroyed — in this process
     * and in any other this descriptor was handed to.  The original survives;
     * revoke removes the subtree, not the root.
     *
     * The count comes back rather than a status, because `CSpace_Revoke`
     * answers how many it destroyed and zero is a perfectly good answer for a
     * descriptor nobody duplicated.
     */
    long n = iris_invoke0((long)slot, INV_CSPACE_REVOKE);
    return (n < 0) ? (int)__libc_fail_iris(n) : (int)n;
}

/*
 * What the SERVICE thinks this descriptor is.
 *
 * A PING on a file capability comes back carrying the badge the kernel stamped
 * on it — which is the service's own name for the open file.  A holder cannot
 * read its own badge any other way, and asking is how a program checks that
 * what it holds is a descriptor rather than something that happened to land in
 * that slot.
 */
long __iris_fd_badge(int fd) {
    struct iris_msg m;
    int slot = fd_resolve(fd);
    if (!fd_is_slot(slot)) return -1;
    iris_msg_zero(&m);
    m.label = IRIS_EP_OP_PING;
    if (iris_msg_call((long)slot, &m) != 0) return -2;
    if (m.label != IRIS_EP_REPLY_OK) return -3 - (long)m.words[0];
    return (long)m.words[1];
}

int __iris_same_file(int a, int b) {
    int sa = fd_resolve(a), sb = fd_resolve(b);
    if (!fd_is_slot(sa) || !fd_is_slot(sb)) return 0;
    /* Asked of the kernel, not computed from numbers: two descriptors name
     * the same open file when their capabilities name the same object. */
    return iris_invoke1((long)sa, INV_CAP_SAME_OBJECT, (long)sb) == 1;
}

/*
 * dup2 — the duplicate lands at a number the CALLER chose.
 *
 * POSIX exists for this function because a program redirecting a descriptor
 * needs the copy to be exactly 0, 1 or 2.  Here a descriptor is a CSpace slot,
 * so the destination must be one this library owns — and the three POSIX
 * numbers are not slots at all.  So this refuses to retarget them, with EBADF,
 * rather than pretending: redirecting `stdout` means handing a program a
 * different console capability at spawn, which is the spawner's decision and
 * is visible where it is made.
 */
int dup2(int fd, int to) {
    int slot = fd_resolve(fd);

    if (!fd_is_slot(slot)) return (int)__libc_fail(EBADF);
    if (!fd_is_slot(to))   return (int)__libc_fail(EBADF);
    if (slot == to) return to;               /* POSIX: a no-op, not an error */

    /* The destination is CLOSED first, exactly as POSIX says, and closing it
     * tells the service so — otherwise a dup2 over a live descriptor leaks one
     * open file per call, which is a table entry and not memory. */
    (void)close(to);
    if (iris_invoke2((long)slot, INV_CSPACE_MINT,
                     (long)((uint64_t)to << 32),
                     (long)(RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER)) != 0)
        return (int)__libc_fail(EACCES);
    return to;
}

/*
 * fstat — what the service says this file is.
 *
 * Only the fields this system can answer: see <sys/stat.h> for why the rest
 * are absent rather than zero.  The offset comes back in the same reply, so it
 * is passed on rather than thrown away and asked for again.
 */
int fstat(int fd, struct stat *st) {
    struct iris_msg m;
    int slot = fd_resolve(fd);
    long r;

    if (!st) return (int)__libc_fail(EINVAL);
    if (!fd_is_slot(slot)) return (int)__libc_fail(EBADF);
    r = fd_call(slot, VFS_EP_OP_FILE_STAT, 0, 0, &m);
    if (r != 0) return (int)__libc_fail_iris(r);

    st->st_size   = (off_t)m.words[1];
    st->st_offset = (off_t)m.words[2];
    /* A regular file, readable by whoever holds the capability.  There is no
     * write bit because there is no write path. */
    st->st_mode   = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    return 0;
}
