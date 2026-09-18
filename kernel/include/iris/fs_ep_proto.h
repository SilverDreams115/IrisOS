#ifndef IRIS_FS_EP_PROTO_H
#define IRIS_FS_EP_PROTO_H

#include <stdint.h>

/*
 * fs_ep_proto.h — a filesystem that survives the machine being turned off.
 *
 * ── Why this exists and `vfs` does not answer it ───────────────────────────
 *
 * `vfs` serves the initrd: files that live inside the kernel image, read-only,
 * identical on every boot.  That is a filesystem in the sense that it answers
 * lookups, and it is not one in the sense that matters here — nothing a task
 * does can change what it will say next time.
 *
 * This one writes to a DISK.  What it stores is on the medium after the
 * machine is off, and is there when it comes back.  The gate proves it the
 * only way that claim can honestly be proved: by booting twice over one image
 * and checking the second boot sees what the first wrote, and then by reading
 * the image from the HOST — which does not depend on IRIS being self-consistent
 * about anything.
 *
 * ── The format, and why it is this small ───────────────────────────────────
 *
 * A superblock, a fixed directory, and one sector per file.  No allocation, no
 * chains, no fragmentation, no free list.
 *
 * That is not the filesystem a system should ship; it is the smallest thing
 * that makes "persistent" a testable claim rather than a plan.  Everything
 * underneath it — a driver that writes, a disk the system owns, a client that
 * addresses files by name — is the part that took the work, and a richer
 * format is an afternoon on top of it rather than a different design.
 * Interoperating with FAT would have been more useful and is a different
 * project; this one is honest about being IRIS's own.
 *
 *   sector 0        superblock
 *   sector 1        directory: 16 entries of 32 bytes
 *   sector 16..     one sector per file, in directory order
 */

#define FS_MAGIC0     0x53464953495253ULL   /* "SRISFS\0" little-endian-ish */
#define FS_VERSION    1u
#define FS_MAX_FILES  16u
#define FS_SECTOR     512u
#define FS_DIR_LBA    1u

/*
 * ── The permission to destroy a disk ───────────────────────────────────────
 *
 * Mounting a disk that carries no IRIS superblock used to mean FORMATTING it,
 * and the reasoning written beside that code was: this disk is IRIS's own,
 * because the block service numbers the boot disk 0 and this one 1.
 *
 * That is true of the machine this was developed on and false of every other
 * one.  Under QEMU disk 1 is an image the test runner made.  On real hardware
 * disk 1 is whatever SATA device enumerates second — which is somebody's data,
 * and this service would have overwritten it on first boot without asking.
 *
 * So formatting is an AUTHORITY now, and like every other authority in this
 * system it has to be granted rather than assumed.  The grant is this token,
 * at a fixed offset in sector 0, and only whoever prepares a disk can write it
 * — which is the host, deliberately, for a disk it is willing to lose.  A disk
 * with neither a superblock nor the token is REFUSED: not formatted, not
 * mounted, reported as foreign and left exactly as it was found.
 *
 * It sits at the end of the sector so that a blank image, a partition table or
 * another filesystem's superblock cannot collide with it by accident, and it
 * is checked as a whole value rather than a prefix.
 */
#define FS_SCRATCH_OFF   496u
#define FS_SCRATCH_TOKEN 0x4B4F2D544D462D53ULL   /* "S-FMT-OK" */
#define FS_DATA_LBA   16u
#define FS_NAME_BYTES 16u

/* The disk this filesystem lives on, as the block service numbers them.
 * Port 0 is the disk the machine booted from and is not ours to write. */
/*
 * The disk this filesystem lives on is ASKED FOR, not assumed.
 *
 * This was `1`, which is right on the machine the tests build -- disk 0 is the
 * boot image and disk 1 is the one the runner made -- and a coin flip on a real
 * one, where the index is whatever order the AHCI ports enumerate in.  `fs`
 * asks `blk` which disk carries an IRIS partition (BLK_OP_HOME) and uses that.
 *
 * The constant stays as the FALLBACK for the case where nothing answers, so a
 * machine with no IRIS partition behaves as it did: every read refused by the
 * layer below, and `fs` reporting that it is not mounted.
 */
#define FS_DISK_PORT  1u

/* ── slot map, as the fs service receives it ────────────────────────────── */
#define FS_SLOT_CTRL_EP   5u
#define FS_SLOT_REPLY     6u
#define FS_SLOT_BLK_EP    7u
/* Its budget is at IRIS_CPTR_OWN_UNTYPED, its address space at OWN_VSPACE. */

/* Its own CSpace. */
#define FS_SLOT_DATA     32u   /* the one file buffer it hands clients      */
#define FS_SLOT_BLKBUF   33u   /* whichever of the driver's buffers it holds */
#define FS_SLOT_PT       34u   /* scratch for paging levels                  */

/* ── operations ────────────────────────────────────────────────────────── */

/*
 * Is there a filesystem, and how many times has it been mounted?  Reply:
 *   words[0] = 1 if mounted (formatted, readable, and ours)
 *   words[1] = the generation: how many times a boot has mounted it
 *   words[2] = how many files it holds
 *   words[3] = 1 if THIS boot formatted it — an image that was blank
 *
 * The generation is what makes persistence visible.  It is read from the disk,
 * incremented, and written back at mount, so a second boot over the same image
 * reports a higher number than the first.  A machine that lost the disk starts
 * again at 1 and says it formatted, which is a different answer and not a
 * quieter version of the same one.
 */
#define FS_OP_STAT    0x7301u

/* The file buffer, as a READ-WRITE frame capability.  words[0] = its size.
 * A client writes the contents of a file into it and then calls FS_OP_WRITE. */
#define FS_OP_BUF     0x7302u

/*
 * Create or replace a file.  words[0..1] = the name, sixteen bytes packed into
 * two words; words[2] = length in bytes, at most one sector.
 */
#define FS_OP_WRITE   0x7303u

/*
 * Read a file by name.  words[0..1] = the name.
 * Replies with a READ-ONLY frame capability over the contents and
 * words[0] = its length, or refuses if there is no such file.
 */
#define FS_OP_READ    0x7304u

#define FS_REP_OK     0x7380u
#define FS_REP_ERR    0x7381u

#endif /* IRIS_FS_EP_PROTO_H */
