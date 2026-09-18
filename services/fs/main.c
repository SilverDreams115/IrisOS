/*
 * fs/main.c — a filesystem that survives the power going off (Stage 10).
 *
 * The format and the reason it is this small are in `iris/fs_ep_proto.h`.
 * This file is the implementation: mount the disk the block service numbers
 * as port 1, read the superblock, and serve files by name.
 *
 * ── Everything it does goes through a capability it was handed ─────────────
 *
 * It holds no ports, no device Untyped, no interrupt and no controller.  It
 * cannot find a disk; it was given an endpoint to a service that has one, and
 * that service hands it a frame per transfer.  So a compromised filesystem can
 * corrupt files on the disk it was given and can do nothing else at all — not
 * reach another disk, not reprogram the controller, not touch memory the block
 * driver did not put in front of it.
 *
 * ── Why the buffers are re-fetched every time ──────────────────────────────
 *
 * The block service revokes the buffer capability it handed out before it
 * reuses the frame, which is what stops a client watching its data change
 * underneath.  The consequence lands here: a capability from the last transfer
 * is gone, so every transfer asks again and maps again.  That is the cost of
 * the rule and it is worth paying — the alternative is a buffer per client,
 * which is a buffer per client the driver has to keep alive for ever.
 */
#include <stdint.h>
#include "../common/iris_msg.h"
#include "../common/iris_map.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/blk_ep_proto.h>
#include <iris/fs_ep_proto.h>

static void fs_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

#define FS_VA_DATA  0x80C0000000ULL   /* the file buffer clients see      */
#define FS_VA_BLK   0x80C1000000ULL   /* whichever driver buffer we hold  */

/* ── on-disk shapes ──────────────────────────────────────────────────────── */
struct fs_super {
    uint64_t magic;
    uint32_t version;
    uint32_t generation;
    /*
     * No file count here.  The first version had one and never maintained it:
     * the format path wrote zero and every write left it at zero, so the
     * service reported "0 files" over a directory with a file in it.  A number
     * stored in two places is a number that disagrees with itself, and the
     * directory is the one that has to be right — so the count is COUNTED.
     */
    uint32_t reserved[2];
};
struct fs_dirent {
    uint8_t  name[FS_NAME_BYTES];
    uint32_t size;          /* 0 = free */
    uint32_t reserved[3];
};

static uint32_t g_mounted, g_generation, g_files, g_formatted;
static uint32_t g_foreign;   /* a disk that exists and is not ours */

static uint32_t fs_count_files(void);

/* ── talking to the block service ────────────────────────────────────────── */

static long fs_blk(uint64_t op, uint64_t a0, uint64_t a1, uint64_t a2,
                   long recv, struct iris_msg *out) {
    struct iris_msg m;
    fs_msg_zero(&m);
    m.label = op;
    m.words[0] = a0; m.words[1] = a1; m.words[2] = a2;
    m.word_count = 3u;
    m.recv_slot  = recv;
    long r = iris_msg_call((long)FS_SLOT_BLK_EP, &m);
    if (out) *out = m;
    if (r != 0) return r;
    return (m.label == BLK_REP_OK) ? 0 : -1;
}

/*
 * Release the buffer we are holding, and only then let go of the capability.
 *
 * That order is the whole of this function and it was wrong the first time.
 * The unmap names a CAPABILITY and an address; if the slot has already been
 * replaced, the unmap asks the kernel to remove a mapping that new capability
 * does not have, which fails — leaving the OLD mapping in place, so the next
 * map of the same address answers BUSY and every filesystem operation after
 * the first one fails.  Unmap first, delete second, receive third.
 */
static void fs_release_blkbuf(void) {
    (void)iris_invoke2((long)FS_SLOT_BLKBUF, INV_FRAME_UNMAP,
                       (long)IRIS_CPTR_OWN_VSPACE, (long)FS_VA_BLK);
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)FS_SLOT_BLKBUF);
}

/* Map whichever buffer the driver just handed over where this service expects
 * it.  The slot is empty until the reply lands, so this runs after the call. */
static long fs_map_blkbuf(uint64_t flags) {
    return iris_map_frame(FS_SLOT_BLKBUF, IRIS_CPTR_OWN_VSPACE,
                          IRIS_CPTR_OWN_UNTYPED, FS_SLOT_PT,
                          FS_VA_BLK, 4096u, flags);
}

/* One sector off the disk, into FS_VA_BLK. */
static int fs_read_sector(uint64_t lba) {
    struct iris_msg r;
    fs_release_blkbuf();
    if (fs_blk(BLK_OP_READ, lba, 1u, FS_DISK_PORT, (long)FS_SLOT_BLKBUF, &r) != 0)
        return 0;
    if (r.got_caps == 0u) return 0;
    return fs_map_blkbuf(0ull) == 0;    /* read-only: we only copy out of it */
}

/* ...and one sector onto it, from whatever the caller put at FS_VA_BLK after
 * fs_write_begin handed the buffer over. */
static int fs_write_begin(void) {
    struct iris_msg r;
    fs_release_blkbuf();
    if (fs_blk(BLK_OP_WRBUF, 0, 0, 0, (long)FS_SLOT_BLKBUF, &r) != 0) return 0;
    if (r.got_caps == 0u) return 0;
    return fs_map_blkbuf(1ull) == 0;
}
static int fs_write_end(uint64_t lba) {
    struct iris_msg r;
    return fs_blk(BLK_OP_WRITE, lba, 1u, FS_DISK_PORT, 0, &r) == 0;
}

static void fs_zero_blk(void) {
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)FS_VA_BLK;
    for (uint32_t i = 0; i < FS_SECTOR; i++) b[i] = 0;
}

/* ── mounting ────────────────────────────────────────────────────────────── */

/*
 * Read the superblock, decide whether this is our filesystem, and record that
 * one more boot has mounted it.
 *
 * The generation is written back immediately rather than at shutdown, because
 * there is no shutdown: a machine that loses power has still mounted the disk,
 * and a count that only advances on a clean exit would say the opposite.
 */
static void fs_mount(void) {
    if (!fs_read_sector(0)) return;

    struct fs_super sb;
    {
        const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)FS_VA_BLK;
        uint8_t *dst = (uint8_t *)&sb;
        for (uint32_t i = 0; i < (uint32_t)sizeof(sb); i++) dst[i] = src[i];
    }

    if (sb.magic != FS_MAGIC0 || sb.version != FS_VERSION) {
        /*
         * No IRIS filesystem here.  Whether that means "format it" depends on
         * something this service cannot work out for itself, so it does not
         * guess: it looks for the token that says the disk is disposable.
         *
         * The old reasoning was that disk 1 is IRIS's own because the block
         * service numbers the boot disk 0.  That holds under QEMU, where disk
         * 1 is an image the runner made, and fails on real hardware, where
         * disk 1 is whatever SATA device enumerates second — somebody's data,
         * about to be overwritten on first boot by a service that was never
         * asked.  See `iris/fs_ep_proto.h`.
         */
        uint64_t token = 0;
        {
            const volatile uint8_t *src =
                (const volatile uint8_t *)(uintptr_t)(FS_VA_BLK + FS_SCRATCH_OFF);
            for (uint32_t i = 0; i < 8u; i++)
                token |= (uint64_t)src[i] << (8u * i);
        }
        if (token != FS_SCRATCH_TOKEN) {
            /* Foreign. Left exactly as found: not formatted, not mounted. */
            g_mounted = 0u; g_generation = 0u; g_files = 0u; g_formatted = 0u;
            g_foreign = 1u;
            return;
        }
        if (!fs_write_begin()) return;
        fs_zero_blk();
        {
            volatile struct fs_super *d = (volatile struct fs_super *)(uintptr_t)FS_VA_BLK;
            d->magic = FS_MAGIC0; d->version = FS_VERSION;
            d->generation = 1u;   d->reserved[0] = 0u; d->reserved[1] = 0u;
        }
        /* Carry the token across the format.  `fs_zero_blk` cleared the
         * sector, and a disk that lost its permission to be reformatted is a
         * disk that cannot be re-prepared without the host writing it again. */
        {
            volatile uint8_t *dst =
                (volatile uint8_t *)(uintptr_t)(FS_VA_BLK + FS_SCRATCH_OFF);
            for (uint32_t i = 0; i < 8u; i++)
                dst[i] = (uint8_t)(FS_SCRATCH_TOKEN >> (8u * i));
        }
        if (!fs_write_end(0)) return;

        /* An empty directory, written rather than assumed: a sector nobody
         * wrote holds whatever the image held, and "whatever" is not empty. */
        if (!fs_write_begin()) return;
        fs_zero_blk();
        if (!fs_write_end(FS_DIR_LBA)) return;

        g_mounted = 1u; g_generation = 1u; g_files = 0u; g_formatted = 1u;
        return;
    }

    sb.generation++;
    if (!fs_write_begin()) return;
    fs_zero_blk();
    {
        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)FS_VA_BLK;
        const uint8_t *src = (const uint8_t *)&sb;
        for (uint32_t i = 0; i < (uint32_t)sizeof(sb); i++) dst[i] = src[i];
    }
    /* And here too: this path also rewrites sector 0 from a zeroed buffer, so
     * leaving the token out would quietly revoke it on the second boot. */
    {
        volatile uint8_t *dst =
            (volatile uint8_t *)(uintptr_t)(FS_VA_BLK + FS_SCRATCH_OFF);
        for (uint32_t i = 0; i < 8u; i++)
            dst[i] = (uint8_t)(FS_SCRATCH_TOKEN >> (8u * i));
    }
    if (!fs_write_end(0)) return;

    g_mounted = 1u; g_generation = sb.generation; g_formatted = 0u;
    g_files = fs_count_files();
}

/* How many directory entries are in use.  Counted rather than stored: see the
 * note on `struct fs_super`. */
static uint32_t fs_count_files(void) {
    if (!fs_read_sector(FS_DIR_LBA)) return 0;
    const volatile struct fs_dirent *d =
        (const volatile struct fs_dirent *)(uintptr_t)FS_VA_BLK;
    uint32_t n = 0;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) if (d[i].size != 0u) n++;
    return n;
}

/* ── the directory ───────────────────────────────────────────────────────── */

static void name_pack(uint64_t w0, uint64_t w1, uint8_t out[FS_NAME_BYTES]) {
    for (uint32_t i = 0; i < 8u; i++) out[i]      = (uint8_t)(w0 >> (8 * i));
    for (uint32_t i = 0; i < 8u; i++) out[8u + i] = (uint8_t)(w1 >> (8 * i));
}

/* The directory slot holding `name`, or the first free one, or -1. */
static int dir_find(const uint8_t name[FS_NAME_BYTES], int want_free,
                    uint32_t *out_size) {
    if (!fs_read_sector(FS_DIR_LBA)) return -1;
    const volatile struct fs_dirent *d =
        (const volatile struct fs_dirent *)(uintptr_t)FS_VA_BLK;
    int first_free = -1;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        if (d[i].size == 0u) { if (first_free < 0) first_free = (int)i; continue; }
        int same = 1;
        for (uint32_t k = 0; k < FS_NAME_BYTES; k++)
            if (d[i].name[k] != name[k]) { same = 0; break; }
        if (same) { if (out_size) *out_size = d[i].size; return (int)i; }
    }
    return want_free ? first_free : -1;
}

/* ── the service loop ────────────────────────────────────────────────────── */

void fs_main(iris_cptr_t bootstrap_ch_h);
void fs_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    /* The buffer clients see.  One frame, reused, revoked before reuse — the
     * same ownership rule the block and network services follow. */
    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)FS_SLOT_DATA << 32), 4096) == 0)
        (void)iris_map_frame(FS_SLOT_DATA, IRIS_CPTR_OWN_VSPACE,
                             IRIS_CPTR_OWN_UNTYPED, FS_SLOT_PT,
                             FS_VA_DATA, 4096u, 1ull);

    fs_mount();

    for (;;) {
        struct iris_msg m;
        fs_msg_zero(&m);
        m.reply = (long)FS_SLOT_REPLY;
        if (iris_msg_recv((long)FS_SLOT_CTRL_EP, &m) != 0) continue;

        struct iris_msg rep;
        fs_msg_zero(&rep);
        rep.label = FS_REP_ERR;

        if (m.label == FS_OP_STAT) {
            rep.label      = FS_REP_OK;
            rep.words[0]   = g_mounted;
            rep.words[1]   = g_generation;
            rep.words[2]   = g_files;
            rep.words[3]   = g_formatted;
            /* A disk that is there and is not ours is a DIFFERENT answer from
             * no disk at all, and the caller has to be able to tell them
             * apart: one means "prepare a disk", the other means "this machine
             * has no disk this service can drive". */
            rep.words[4]   = g_foreign;
            rep.word_count = 5u;
        } else if (m.label == FS_OP_BUF && g_mounted) {
            rep.label      = FS_REP_OK;
            rep.words[0]   = FS_SECTOR;
            rep.word_count = 1u;
            (void)iris_invoke0((long)FS_SLOT_DATA, INV_CSPACE_REVOKE);
            rep.cap        = (long)FS_SLOT_DATA;
            rep.cap_rights = RIGHT_READ | RIGHT_WRITE;
        } else if (m.label == FS_OP_WRITE && g_mounted) {
            uint8_t name[FS_NAME_BYTES];
            name_pack(m.words[0], m.words[1], name);
            uint32_t len = (uint32_t)m.words[2];
            int slot = (len > 0u && len <= FS_SECTOR) ? dir_find(name, 1, 0) : -1;
            if (slot >= 0) {
                /* The contents first, the directory second.  A crash between
                 * them leaves a file that is not named, which is lost space;
                 * the other order leaves a NAME pointing at whatever the
                 * sector held, which is a file that reads as somebody else's
                 * data.  Neither order is a transaction — this filesystem has
                 * no journal and does not pretend to — but one of the two
                 * failure modes is much worse than the other. */
                int ok = fs_write_begin();
                if (ok) {
                    fs_zero_blk();
                    const volatile uint8_t *src =
                        (const volatile uint8_t *)(uintptr_t)FS_VA_DATA;
                    volatile uint8_t *dst =
                        (volatile uint8_t *)(uintptr_t)FS_VA_BLK;
                    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
                    ok = fs_write_end(FS_DATA_LBA + (uint64_t)slot);
                }
                if (ok && fs_read_sector(FS_DIR_LBA)) {
                    struct fs_dirent dir[FS_MAX_FILES];
                    {
                        const volatile uint8_t *s =
                            (const volatile uint8_t *)(uintptr_t)FS_VA_BLK;
                        uint8_t *d2 = (uint8_t *)dir;
                        for (uint32_t i = 0; i < (uint32_t)sizeof(dir); i++) d2[i] = s[i];
                    }
                    for (uint32_t k = 0; k < FS_NAME_BYTES; k++)
                        dir[slot].name[k] = name[k];
                    dir[slot].size = len;
                    if (fs_write_begin()) {
                        fs_zero_blk();
                        volatile uint8_t *d3 = (volatile uint8_t *)(uintptr_t)FS_VA_BLK;
                        const uint8_t *s2 = (const uint8_t *)dir;
                        for (uint32_t i = 0; i < (uint32_t)sizeof(dir); i++) d3[i] = s2[i];
                        if (fs_write_end(FS_DIR_LBA)) {
                            g_files = fs_count_files();
                            rep.label      = FS_REP_OK;
                            rep.words[0]   = len;
                            rep.word_count = 1u;
                        }
                    }
                }
            }
        } else if (m.label == FS_OP_READ && g_mounted) {
            uint8_t name[FS_NAME_BYTES];
            name_pack(m.words[0], m.words[1], name);
            uint32_t size = 0;
            int slot = dir_find(name, 0, &size);
            if (slot >= 0 && size > 0u && size <= FS_SECTOR &&
                fs_read_sector(FS_DATA_LBA + (uint64_t)slot)) {
                (void)iris_invoke0((long)FS_SLOT_DATA, INV_CSPACE_REVOKE);
                const volatile uint8_t *src =
                    (const volatile uint8_t *)(uintptr_t)FS_VA_BLK;
                volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)FS_VA_DATA;
                for (uint32_t i = 0; i < FS_SECTOR; i++) dst[i] = src[i];
                rep.label      = FS_REP_OK;
                rep.words[0]   = size;
                rep.word_count = 1u;
                rep.cap        = (long)FS_SLOT_DATA;
                rep.cap_rights = RIGHT_READ;
            }
        }

        (void)iris_msg_reply((long)FS_SLOT_REPLY, &rep);
    }
}
