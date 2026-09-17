#ifndef IRIS_IP_EP_PROTO_H
#define IRIS_IP_EP_PROTO_H

#include <stdint.h>

/*
 * ip_ep_proto.h — ARP, IPv4 and UDP, as a service (Stage 10).
 *
 * ── Why this is not in the driver ──────────────────────────────────────────
 *
 * `net` moves Ethernet frames and parses nothing.  That was a deliberate line
 * and this service is the other side of it: everything here is POLICY about
 * what the bytes in a frame mean — which addresses are ours, what to do with a
 * broadcast, how long an ARP entry is good for, which datagram belongs to
 * which port.  None of it needs to touch hardware, and a driver that knew any
 * of it would be a driver nobody could replace without reimplementing a
 * protocol stack.
 *
 * The split is worth more than tidiness.  This service holds an endpoint to
 * `net` and nothing else — no ports, no device Untyped, no DMA authority, no
 * controller.  A compromised IP stack can send and receive frames on one
 * interface.  It cannot reprogram the card, cannot reach another device, and
 * cannot make the NIC write anywhere: `net` already decided what the hardware
 * may touch, and this service is downstream of that decision.
 *
 * ── What it implements, and what it does not ───────────────────────────────
 *
 * ARP with a small cache, and it ANSWERS requests for its own address as well
 * as making them — a host that only asks is a host nobody can reach.
 * IPv4 with a header checksum, no options, no fragmentation, no routing table
 * beyond "this subnet or the gateway".  UDP with a checksum.
 *
 * There is no TCP, no ICMP, no DHCP, no DNS, and no sockets: a port is a
 * number in a request rather than an object, so there is no bind, no listen
 * and no accept.  Each of those is a real thing to want and each is its own
 * body of work; naming them is more useful than a stack that half-has them.
 *
 * Fragmentation is not "unsupported" so much as refused: a datagram longer
 * than one Ethernet frame is rejected at the call rather than sent in pieces
 * that nothing here could reassemble on the way back.
 */

/* ── slot map, as the ip service receives it ────────────────────────────── */
#define IP_SLOT_CTRL_EP   5u
#define IP_SLOT_REPLY     6u
#define IP_SLOT_NET_EP    7u
/* Its budget is at IRIS_CPTR_OWN_UNTYPED, its address space at OWN_VSPACE. */

/* Its own CSpace. */
#define IP_SLOT_PAYLOAD  32u   /* the datagram buffer clients see          */
#define IP_SLOT_NETBUF   33u   /* whichever of the driver's buffers it holds */
#define IP_SLOT_PT       34u

/* ── addresses ─────────────────────────────────────────────────────────── */

/*
 * The addresses this system uses, and where they come from.
 *
 * QEMU's userspace network hands out 10.0.2.15, answers as the gateway on
 * 10.0.2.2 and runs a TFTP server there.  They are written here rather than
 * discovered because there is no DHCP client: a stack that cannot ask has to
 * be told, and being told by a constant is honest about which it is.
 */
#define IP_ADDR(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                          ((uint32_t)(c) << 8)  |  (uint32_t)(d))
#define IP_DEFAULT_ADDR    IP_ADDR(10, 0, 2, 15)
#define IP_DEFAULT_MASK    IP_ADDR(255, 255, 255, 0)
#define IP_DEFAULT_GATEWAY IP_ADDR(10, 0, 2, 2)

/* ── operations ────────────────────────────────────────────────────────── */

/*
 * What this interface is.  Reply:
 *   words[0] = 1 if the stack has an interface under it
 *   words[1] = our IPv4 address
 *   words[2] = our MAC, low six bytes
 *   words[3] = how many datagrams have been received for us
 */
#define IP_OP_INFO     0x7401u

/*
 * Resolve an IPv4 address to a MAC.  words[0] = the address.
 * Reply: words[0] = the MAC, or refused if nobody answered.
 *
 * An address outside our subnet resolves to the GATEWAY's MAC, which is what
 * a routing table of one entry means.
 */
#define IP_OP_ARP      0x7402u

/* The datagram buffer, as a READ-WRITE frame capability.  words[0] = its size.
 * A client writes a UDP payload into it and then calls IP_OP_UDP_SEND. */
#define IP_OP_BUF      0x7403u

/*
 * Send that payload as a UDP datagram.
 *   words[0] = destination address
 *   words[1] = destination port | source port << 16
 *   words[2] = payload length
 * Reply: words[0] = bytes sent.
 */
#define IP_OP_UDP_SEND 0x7404u

/*
 * Wait for a UDP datagram addressed to a local port.
 *   words[0] = the local port
 *   words[1] = how long to wait, in milliseconds
 * Replies with a READ-ONLY frame capability over the payload and
 *   words[0] = payload length, or 0 if nothing arrived in time
 *   words[1] = the sender's address
 *   words[2] = the sender's port
 *
 * The sender's PORT matters and is why this is not keyed on it: a TFTP server
 * answers a request to port 69 from an ephemeral port of its own, so a receive
 * that demanded the port it sent to would wait for ever.
 */
#define IP_OP_UDP_RECV 0x7405u

#define IP_REP_OK      0x7480u
#define IP_REP_ERR     0x7481u

/* One datagram per call, and it fits in one Ethernet frame.  A payload longer
 * than this is refused rather than fragmented. */
#define IP_MAX_PAYLOAD 512u

#endif /* IRIS_IP_EP_PROTO_H */
