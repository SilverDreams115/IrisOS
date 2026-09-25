#ifndef IRIS_BLK_EP_PROTO_H
#define IRIS_BLK_EP_PROTO_H

#include <stdint.h>

/*
 * blk_ep_proto.h — storage, as a service (Stage 10).
 *
 * ── What this is ───────────────────────────────────────────────────────────
 *
 * An AHCI driver in ring 3.  It asks `pci` for the SATA controller, gets a
 * frame over the controller's registers, builds the command structures in
 * memory it owns, and issues real ATA commands to a real disk.
 *
 * It is the second driver in the system and the first one that is USEFUL, and
 * it is here because "user-space drivers" is a claim that needs a driver doing
 * something a system actually needs.  Driving a DMA engine proves containment;
 * reading a disk proves the arrangement can carry a subsystem.
 *
 * ── Why the DMA is the interesting part ────────────────────────────────────
 *
 * AHCI is a bus master: the driver writes physical addresses into a command
 * table and the controller reads and writes those addresses itself.  That is
 * precisely the reach Stage 10-dma made containable, so this service does the
 * containable thing — when the machine has a remapping unit it binds an
 * IOSpace to the controller's source-id and maps ONLY the frames the
 * controller needs.  A compromised disk driver on such a machine can make the
 * controller write to its own buffers and to nothing else.
 *
 * On a machine with no unit it works anyway, and the controller can reach all
 * of memory.  That is not this service's failure and it does not pretend
 * otherwise: it reports which of the two it is, and the difference is visible.
 *
 * ── The buffer, and who owns it ────────────────────────────────────────────
 *
 * A read replies with a read-only FRAME capability over the data the transfer
 * landed in.  The service REUSES that buffer, so before each read it revokes
 * what it handed out: a client still holding the capability from the last read
 * loses it rather than watching its data change underneath.  Revoke-before-
 * reuse is the whole ownership story, and it is why the buffer can be one
 * frame instead of one per request.
 *
 * A client that wants to keep the bytes copies them.  A client that wants the
 * service to DMA into a frame the CLIENT owns is the right next shape and is
 * not this one — it needs the client's frame mapped into the service's
 * IOSpace per request, which is real work and is named here rather than
 * half-done.
 */

/* ── slot map, as the blk service receives it ───────────────────────────── */
#define BLK_SLOT_CTRL_EP    5u   /* the endpoint it serves on (RIGHT_READ)   */
#define BLK_SLOT_REPLY      6u   /* the reply object its receive stages      */
#define BLK_SLOT_PCI_EP     7u   /* how it finds its controller              */
#define BLK_SLOT_IOSPACE_C  8u   /* IOSpaceControl, to contain the controller */
/* Its own budget arrives at IRIS_CPTR_OWN_UNTYPED (12), its address space at
 * IRIS_CPTR_OWN_VSPACE (18), like every service's. */

/* Its own CSpace, for the objects it makes. */
#define BLK_SLOT_ABAR       32u  /* the frame over the controller's registers */
#define BLK_SLOT_CMD        33u  /* command list + received FIS + command table */
#define BLK_SLOT_DATA       34u  /* the data buffer a read lands in           */
#define BLK_SLOT_WR         42u  /* ...and the one a write comes from         */
#define BLK_SLOT_IOSPACE    35u  /* the controller's DMA address space        */
#define BLK_SLOT_IOPT(i)    (36u + (i))   /* three levels of its translation  */
#define BLK_SLOT_PT         40u  /* scratch for paging levels it installs     */

/* ── operations ────────────────────────────────────────────────────────── */

/*
 * What is attached.  Reply:
 *   words[0] = 1 if a disk was found and initialised, 0 otherwise
 *   words[1] = logical sector size in bytes (512 on every disk IRIS has seen)
 *   words[2] = the controller's source id, or 0 — what an IOSpace binds to
 *   words[3] = 1 if the controller's DMA is CONTAINED by a remapping unit
 */
#define BLK_OP_INFO   0x7101u

/*
 * Read sectors.  words[0] = LBA, words[1] = sector count (1..BLK_MAX_SECTORS).
 *
 * Replies with a read-only FRAME capability over the data, and
 *   words[0] = bytes transferred
 *   words[1] = the generation of this buffer, which goes up on every read
 * A caller that declares no receive slot gets the words and no frame, which is
 * a legitimate way to ask whether a sector reads at all.
 */
#define BLK_OP_READ   0x7102u

/*
 * The WRITE buffer, as a READ-WRITE frame capability.  words[0] = its size.
 *
 * A separate buffer from the one a read lands in, and separate on purpose: a
 * client that wrote into the read buffer would be writing the service's DMA
 * target while a read might be in flight, and a service that reused one frame
 * for both would have to say which of the two the capability it just handed
 * out was for.  Two frames, two rules, no ambiguity.
 */
#define BLK_OP_WRBUF  0x7103u

/*
 * Write sectors FROM that buffer.  words[0] = LBA, words[1] = sector count.
 * Reply: words[0] = bytes written.
 *
 * A reply means the data is on the MEDIUM, not merely accepted: the driver
 * issues FLUSH CACHE EXT after every write.  That is not belt and braces — an
 * emulated disk holds writes in a cache and reports success, so without the
 * flush the host image does not change and "persistent" would be a claim about
 * a cache.  It costs one non-data command per write on a driver that already
 * waits for each one; a driver that batched writes would want the choice back.
 */
#define BLK_OP_WRITE  0x7104u

/*
 * The window a disk may be addressed through.
 *   words[0] = disk index
 * Reply:
 *   words[0] = how many sectors, 0 if this disk carries no IRIS partition
 *   words[1] = the absolute LBA it starts at, for reporting only -- no
 *              operation here takes an absolute address
 *
 * It is its own operation rather than another field on INFO because an IPC
 * message carries exactly IRIS_MSG_WORDS words and INFO already uses all of
 * them.  Appending a fifth was tried and wrote past the array: the value came
 * back as 4 instead of 8159, which is the only reason it was noticed.
 */
#define BLK_OP_PART   0x7105u

/*
 * WHICH disk carries an IRIS partition.
 *   Reply: words[0] = the disk index, words[1] = 1 if one was found, else 0
 *
 * A client that wants IRIS's own storage has to ask this rather than assume an
 * index, and the difference is not cosmetic.  Under QEMU the answer is always
 * 1, because the runner builds the machine: disk 0 is the boot image and disk 1
 * is the one it made.  On real hardware the order is whatever the AHCI ports
 * enumerate in, so a hardcoded 1 is a coin flip between a machine's two SATA
 * drives -- and the one it lands on may be somebody's data, where the right
 * behaviour (refuse everything) is indistinguishable from a broken system.
 *
 * It is the same rule the disk driver already follows one layer down: `blk`
 * finds its controller by CLASS CODE rather than by vendor:device, because a
 * driver that matched an identifier would drive one machine.  Position is not
 * identity.
 */
#define BLK_OP_HOME   0x7106u

/*
 * What the partition scan saw on a disk, for the case where it found nothing.
 *   words[0] = disk index
 * Reply:
 *   words[0] = flags: 1 header read, 2 signature present, 4 entries read
 *   words[1] = how many entries the header claimed
 *   words[2] = the first eight bytes of entry 0's type
 *
 * It exists because the failure it describes DISABLES the other channel: a
 * disk with no IRIS partition is a disk the filesystem will not write a report
 * to, so the screen has to carry the diagnosis.  Those four numbers separate
 * "the read failed" from "not a GPT disk" from "the entries never arrived"
 * from "they arrived and nothing matched".
 */
#define BLK_OP_SCAN   0x7107u

#define BLK_REP_OK    0x7180u
#define BLK_REP_ERR   0x7181u

/* One page of data per request: the PRDT this driver builds has one entry and
 * the buffer is one frame.  Stated in the protocol because a caller has to
 * size its expectations, not discover them. */
/*
 * ── The partition, and why a client cannot address around it ───────────────
 *
 * Every LBA in this protocol is RELATIVE to the partition the service found,
 * and there is no way to express an absolute one.  That is the containment,
 * and it is by construction rather than by checking: a client that wants to
 * write outside its partition has no word to put the address in.
 *
 * It replaces a much worse arrangement.  `blk` took an absolute LBA and put it
 * straight into a `WRITE DMA EXT`, and `fs` wrote its superblock to LBA 0 --
 * which on a raw test image is the start of the image and on a real disk is
 * the PARTITION TABLE of the whole drive.  Pointing this system at a machine's
 * second SATA disk would have destroyed the addressing for every partition on
 * it, including the ones holding data, and no amount of care in the filesystem
 * above could have prevented it.
 *
 * The partition is found by its TYPE, in the GPT, and the type is the sixteen
 * bytes of ASCII "IRISFS-PARTITION".  A real type GUID is never printable
 * ASCII, so this cannot collide with one, and it is legible in a hex dump of a
 * disk -- which matters when the question is "did this thing touch my drive".
 * It is a deliberate choice rather than a generated identifier for exactly
 * that reason.
 *
 * A disk with no such partition gets NO window, and every read and write to it
 * is refused.  Not "mounted read-only", not "the whole disk by default":
 * refused.  The default for a disk nobody granted is nothing.
 */
#define BLK_PART_TYPE "IRISFS-PARTITION"   /* 16 bytes, on the wire as-is */

#define BLK_SECTOR_BYTES 512u
#define BLK_MAX_SECTORS  8u      /* 8 * 512 = one 4 KiB frame */

#endif /* IRIS_BLK_EP_PROTO_H */
