#ifndef IRIS_PCI_EP_PROTO_H
#define IRIS_PCI_EP_PROTO_H

#include <stdint.h>

/*
 * pci_ep_proto.h — the bus, as a service (Stage 10).
 *
 * ── Why this is a service and not a library ────────────────────────────────
 *
 * PCI configuration space is reached through ONE pair of I/O ports at
 * 0xCF8/0xCFC, shared by every device on the machine.  A driver that holds a
 * capability for those ports can reprogram any device's BARs, turn any
 * device's bus mastering on, and read any device's registers — so handing that
 * capability to each driver would make "a driver reaches only what its
 * capabilities name" false again, one port range at a time.  It is exactly the
 * shape Stage 10-dma closed for DMA, and it would be silly to close that and
 * then reopen it through the config space that programs the DMA.
 *
 * So one task holds the ports and nobody else does.  A driver asks it for its
 * own device and gets back a FRAME over that device's register window and the
 * source-id the device puts on the bus — the two things a driver needs and the
 * only two.  It cannot see another device's registers, cannot move a BAR, and
 * cannot enable bus mastering on hardware it did not claim.
 *
 * ── What the service owns, and what it therefore is ────────────────────────
 *
 * The PCI-hole device Untyped (`IRIS_CPTR_MMIO_UNTYPED`) belongs to this
 * service and to nothing else.  That is what makes the claim above enforceable
 * rather than conventional: a driver has no device Untyped, so there is no
 * frame it could retype over a window it was not given.
 *
 * It also means the service must carve its frames in ADDRESS ORDER, because an
 * Untyped is a watermark and does not go backwards.  It does that once, at
 * startup, over every window it found — which is the right time anyway: a bus
 * driver that discovers the machine and then answers questions about it is a
 * simpler thing than one that discovers lazily and can fail later.
 *
 * ── The scan ───────────────────────────────────────────────────────────────
 *
 * Bus 0 only, every device, every function of a multifunction device.  A
 * PCI-to-PCI bridge's secondary bus is NOT walked: this machine has none, and
 * walking one would be code no test covers — which is worse than absent,
 * because it reads as support.  The limit is stated in `PCI_MAX_FUNCTIONS` and
 * in the service's reply to PCI_OP_COUNT, so a caller can tell "no such
 * device" from "this service never looked there".
 */

/* ── slot map, as the pci service receives it ───────────────────────────── */
#define PCI_SLOT_CTRL_EP   5u   /* the endpoint it serves on (RIGHT_READ)     */
#define PCI_SLOT_IOPORT    6u   /* 0xCF8..0xCFF, and nothing else             */
#define PCI_SLOT_REPLY     7u   /* the reply object its receive stages        */
#define PCI_SLOT_MMIO_UT   8u   /* the PCI hole, as a device Untyped          */
/* Its own budget arrives at IRIS_CPTR_OWN_UNTYPED (12) like every service's. */

/*
 * Where the service keeps the frames it carved, in its own CSpace: one leaf
 * per claimable window.  They are handed out by COPY, so claiming twice is not
 * an error and two drivers for one device is a question for whoever granted
 * them the endpoint — this service reports the machine, it does not arbitrate.
 */
#define PCI_SLOT_BAR_BASE  32u
/*
 * 16 was not a near miss on the first real machine -- it was 14 of 16 used.
 * One more device with a memory BAR and the carve would have stopped with
 * PCI_CARVE_FULL, and the drivers above would have found nothing to map for
 * reasons that have nothing to do with them.
 *
 * A number chosen against one machine and then met by the next one is not a
 * limit, it is a coincidence with a deadline.  64 is chosen against the shape
 * of the problem instead: a desktop with bridged storage, graphics, audio,
 * USB controllers and a NIC lands in the tens.
 */
#define PCI_MAX_WINDOWS    64u

/* ── limits, which are part of the contract ─────────────────────────────── */
/*
 * How many functions the service records, and how deep it looks.
 *
 * 32 was enough for a machine with one bus.  A real desktop enumerated 24
 * functions on bus 0 alone and had NO mass-storage controller among them --
 * its SATA and NVMe controllers sit behind PCI-to-PCI bridges, on buses this
 * service never visited.  That is the limitation the roadmap had recorded
 * since the service was written, met on the first real machine.
 *
 * `PCI_SCAN_BUS` is gone with it: the walk starts at bus 0 and follows every
 * bridge it finds, which is what "enumerate the machine" means on anything
 * newer than the one it was developed against.
 */
#define PCI_MAX_FUNCTIONS  96u  /* how many functions the service will record */
#define PCI_MAX_BUSES     256u  /* a bus number is eight bits                 */

/* ── operations ────────────────────────────────────────────────────────── */

/* How many functions were found.  words[0] = count, words[1] = PCI_MAX_FUNCTIONS
 * so a caller can tell a full table from an empty bus. */
#define PCI_OP_COUNT       0x7001u

/*
 * What a function IS.  words[0] = index.
 * Reply: words[0] = vendor | device << 16   (the config dword at 0x00)
 *        words[1] = class code dword        (the config dword at 0x08)
 *        words[2] = source id (bus << 8 | devfn) — what it puts on the bus,
 *                   and therefore what an IOSpace is bound to
 *        words[3] = bitmask of BARs that decode memory
 */
#define PCI_OP_INFO        0x7002u

/*
 * Where a BAR decodes.  words[0] = index, words[1] = BAR number (0..5).
 * Reply: words[0] = physical base, words[1] = size in bytes,
 *        words[2] = PCI_BAR_* flags.
 */
#define PCI_OP_BAR         0x7003u

/*
 * The window itself, as a capability.  words[0] = index, words[1] = BAR.
 * Replies with a FRAME capability covering exactly that BAR's window, and
 * words[0] = its physical base, words[1] = its size.  The caller declares a
 * receive slot; a caller that declares none gets the numbers and no frame,
 * which is a legitimate way to ask "how big is it".
 */
#define PCI_OP_CLAIM       0x7004u

/*
 * Turn the device on.  words[0] = index, words[1] = PCI_CMD_* bits to SET.
 * Only the three decode/master bits can be set and none can be cleared through
 * this operation: a caller that could clear them could stop another driver's
 * device, and a caller that could set arbitrary command bits could enable
 * interrupt lines and parity error responses nobody asked for.
 */
#define PCI_OP_ENABLE      0x7005u

/*
 * Why the carve stopped, in numbers.
 *   words[0] = the gap it could not step over
 *   words[1] = the error the retype gave
 *   words[2] = the watermark   words[3] = the end of the region
 *
 * Separate from PCI_OP_COUNT because that message already carries four words,
 * which is all a message has -- and because the state code it carries names
 * the STEP that failed rather than the reason.
 */
#define PCI_OP_CARVE       0x7006u

/* Reply labels. */
#define PCI_REP_OK         0x7080u
#define PCI_REP_ERR        0x7081u

/* BAR flags, as PCI_OP_BAR reports them. */
#define PCI_BAR_MMIO       (1u << 0)  /* memory space (else I/O space)        */
#define PCI_BAR_64         (1u << 1)  /* 64-bit BAR; consumes the next one too */
#define PCI_BAR_PREFETCH   (1u << 2)
#define PCI_BAR_CLAIMABLE  (1u << 3)  /* the service carved a frame over it    */

/* Command-register bits PCI_OP_ENABLE will set. */
#define PCI_CMD_IO         (1u << 0)
#define PCI_CMD_MEMORY     (1u << 1)
#define PCI_CMD_BUS_MASTER (1u << 2)
#define PCI_CMD_SETTABLE   (PCI_CMD_IO | PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER)

#endif /* IRIS_PCI_EP_PROTO_H */
