#ifndef IRIS_NET_EP_PROTO_H
#define IRIS_NET_EP_PROTO_H

#include <stdint.h>

/*
 * net_ep_proto.h — a network interface, as a service (Stage 10).
 *
 * ── What this is, and what it deliberately is not ──────────────────────────
 *
 * An Intel 82540EM (e1000) driver in ring 3.  It moves ETHERNET FRAMES and
 * knows nothing about what is in them: no ARP, no IP, no checksums, no
 * addresses beyond its own MAC.  That is not an unfinished protocol stack, it
 * is the line between a driver and a stack — a driver that parsed ARP would be
 * policy in a driver, which is the thing this whole system is arranged to
 * avoid.  Whoever wants a stack builds one on top of this endpoint.
 *
 * It is also the third bus master in the tree, and the third one contained:
 * the NIC reads its descriptor rings and its buffers by physical address, so
 * on a machine with a remapping unit it reaches those frames and nothing else.
 *
 * ── The buffers, and who owns them ─────────────────────────────────────────
 *
 * Two, and they are asymmetric because sending and receiving are.
 *
 * TRANSMIT: the client asks for the TX buffer, gets a READ-WRITE frame
 * capability over it, writes a frame into it and says how long.  Read-write
 * because the bytes are the client's — the driver supplies the memory, not the
 * content.
 *
 * RECEIVE: the driver hands back a READ-ONLY frame over the buffer the NIC
 * wrote into, with the length.  Read-only for the same reason the disk
 * service's is: a client of a driver has no business writing the driver's DMA
 * target.
 *
 * Both are REUSED, so both are revoked before reuse.  A client holding a
 * capability from the previous call loses it rather than watching its data
 * change underneath — the same rule as `blk`, for the same reason.
 */

/* ── slot map, as the net service receives it ───────────────────────────── */
#define NET_SLOT_CTRL_EP    5u
#define NET_SLOT_REPLY      6u
#define NET_SLOT_PCI_EP     7u
#define NET_SLOT_IOSPACE_C  8u
/* Its budget is at IRIS_CPTR_OWN_UNTYPED, its address space at OWN_VSPACE. */

/* Its own CSpace. */
#define NET_SLOT_BAR        32u
#define NET_SLOT_RING       33u   /* the RX and TX descriptor rings       */
#define NET_SLOT_RXBUF      34u   /* eight receive buffers                */
#define NET_SLOT_TXBUF      35u   /* the one the client writes into       */
#define NET_SLOT_IOSPACE    36u
#define NET_SLOT_IOPT(i)    (37u + (i))
#define NET_SLOT_PT         41u

/* ── the interface ─────────────────────────────────────────────────────── */

/*
 * What is attached.  Reply:
 *   words[0] = 1 if a NIC was found and brought up
 *   words[1] = the MAC address, low six bytes
 *   words[2] = the controller's source id
 *   words[3] = 1 if its DMA is contained by a remapping unit
 */
#define NET_OP_INFO   0x7201u

/*
 * The transmit buffer, as a READ-WRITE frame capability.  words[0] = its size.
 * The caller writes a complete Ethernet frame into it — destination, source,
 * ethertype and payload — and then calls NET_OP_SEND.
 */
#define NET_OP_TXBUF  0x7202u

/* Transmit words[0] bytes from that buffer.  Reply: words[0] = bytes sent. */
#define NET_OP_SEND   0x7203u

/*
 * The oldest unread received frame, as a READ-ONLY frame capability.
 *   words[0] = length in bytes, or 0 if nothing has arrived
 *   words[1] = how many frames this interface has received in total
 * Length 0 comes with no capability, which is how a caller polls.
 */
#define NET_OP_RECV   0x7204u

#define NET_REP_OK    0x7280u
#define NET_REP_ERR   0x7281u

/* One frame per buffer, and the buffer is 256 bytes because that is an e1000
 * receive-size the hardware offers and it fits eight of them in one page.
 * Stated here because a caller has to size its expectations. */
#define NET_FRAME_BYTES 256u

#endif /* IRIS_NET_EP_PROTO_H */
