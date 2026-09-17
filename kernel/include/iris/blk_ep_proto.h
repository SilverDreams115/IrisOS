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

#define BLK_REP_OK    0x7180u
#define BLK_REP_ERR   0x7181u

/* One page of data per request: the PRDT this driver builds has one entry and
 * the buffer is one frame.  Stated in the protocol because a caller has to
 * size its expectations, not discover them. */
#define BLK_SECTOR_BYTES 512u
#define BLK_MAX_SECTORS  8u      /* 8 * 512 = one 4 KiB frame */

#endif /* IRIS_BLK_EP_PROTO_H */
