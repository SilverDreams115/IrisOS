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
#include "libc_internal.h"
#include "../common/iris_msg.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>

/*
 * Where descriptors live: slots 40..63, out of the 32..63 range
 * docs/contracts/program.md §2 gives the program.  Twenty-four open files,
 * which is a real limit and is stated rather than discovered.
 */
#define FD_LO  40
#define FD_HI  63

/* The three POSIX numbers, and nothing more.  -1 means "nothing there". */
#define FD_CONSOLE (-2)          /* an endpoint, not a file                 */
static int g_std[3] = { -1, FD_CONSOLE, FD_CONSOLE };

static int fd_is_slot(int fd) { return fd >= FD_LO && fd <= FD_HI; }

/* Resolve a descriptor to what it actually is. */
static int fd_resolve(int fd) {
    if (fd >= 0 && fd < 3) return g_std[fd];
    return fd_is_slot(fd) ? fd : -1;
}

static int fd_alloc(void) {
    for (int s = FD_LO; s <= FD_HI; s++) {
        /* An empty slot is one nothing identifies.  Asking the kernel is the
         * only account of what a CSpace holds that cannot be stale — a
         * bitmap here would be a second table, which is the thing this design
         * does not have. */
        if (iris_invoke0((long)s, INV_CAP_IDENTIFY) < 0) return s;
    }
    return -1;
}

/* One call on a file capability. */
static long fd_call(int slot, uint64_t label, uint64_t w0, uint64_t w1,
                    struct iris_msg *out) {
    iris_msg_zero(out);
    out->label      = label;
    out->words[0]   = w0;
    out->words[1]   = w1;
    out->word_count = 2u;
    if (iris_msg_call((long)slot, out) != 0) return (long)IRIS_ERR_NOT_SUPPORTED;
    if (out->label != IRIS_EP_REPLY_OK) return -1;
    return 0;
}

int open(const char *path, int flags, ...) {
    struct iris_msg m;
    uint32_t n;
    int slot;

    (void)flags;                     /* read-only: see <fcntl.h> */
    if (!path || !__libc_ipc_buf()) return -1;
    n = (uint32_t)strlen(path);
    if (n == 0u || n + 1u > VFS_EP_PATH_MAX) return -1;

    slot = fd_alloc();
    if (slot < 0) return -1;

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

    if (iris_msg_call((long)IRIS_CPTR_VFS_EP, &m) != 0) return -1;
    if (m.label != IRIS_EP_REPLY_OK) return -1;
    if (m.got_caps == 0u) return -1;  /* a reply with no capability is not one */
    return slot;
}

ssize_t read(int fd, void *buf, size_t n) {
    struct iris_msg m;
    int slot = fd_resolve(fd);

    if (slot == FD_CONSOLE) return 0;   /* the console does not read back */
    if (slot < 0 || !buf) return -1;
    /* stdin is nothing, and says so as EOF rather than as an error: a program
     * that reads until zero terminates, and one that treats -1 as fatal
     * would not. */
    if (fd == STDIN_FILENO) return 0;
    if (n == 0u) return 0;
    if (n > VFS_EP_DATA_MAX) n = VFS_EP_DATA_MAX;

    if (fd_call(slot, VFS_EP_OP_FILE_READ, n, 0, &m) != 0) return -1;
    {
        uint32_t got = (uint32_t)m.words[1];
        if (got > n) return -1;
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
    if (slot != FD_CONSOLE) return -1;
    if (!buf) return -1;
    __libc_console_write((const char *)buf, n);
    return (ssize_t)n;
}

int close(int fd) {
    int slot = fd_resolve(fd);
    struct iris_msg m;

    if (slot == FD_CONSOLE) return 0;   /* not ours to close */
    if (!fd_is_slot(slot)) return -1;

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

    if (!fd_is_slot(slot)) return -1;
    to = fd_alloc();
    if (to < 0) return -1;

    /*
     * A real derivation, in this program's own CSpace.
     *
     * `RIGHT_SAME_RIGHTS` is not asked for: a mint reduces, and asking for
     * exactly what the source has is how a duplicate stays a duplicate.  The
     * badge is inherited — the kernel's no-re-badge rule guarantees it — so
     * the copy names the same open file, with the same offset, which is what
     * `dup` means.
     */
    if (iris_invoke2((long)slot, INV_CSPACE_MINT,
                     (long)((uint64_t)to << 32),
                     (long)(RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER)) != 0)
        return -1;
    return to;
}

off_t lseek(int fd, off_t off, int whence) {
    struct iris_msg m;
    int slot = fd_resolve(fd);

    if (!fd_is_slot(slot)) return -1;
    if (whence < 0 || whence > SEEK_END) return -1;
    if (fd_call(slot, VFS_EP_OP_FILE_SEEK, (uint64_t)off, (uint64_t)whence, &m) != 0)
        return -1;
    return (off_t)m.words[1];
}

int __iris_revoke(int fd) {
    int slot = fd_resolve(fd);
    if (!fd_is_slot(slot)) return -1;
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
    return (n < 0) ? -1 : (int)n;
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
