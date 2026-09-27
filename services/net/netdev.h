/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_NET_NETDEV_H
#define IRIS_NET_NETDEV_H

#include <stdint.h>
#include <iris/net_ep_proto.h>   /* NET_RX_FRAMES, NET_FRAME_BYTES */

/*
 * The line between the network SERVICE and a network DRIVER.
 *
 * ── Why there is a line at all ────────────────────────────
 *
 * A disk driver can match by CLASS, because AHCI is a programming interface:
 * a controller that reports class 01:06:01 has the register layout the driver
 * knows, whoever built it.  Ethernet class 02:00 promises nothing of the kind.
 * An Intel e1000 and a Realtek RTL8168 are both class 02:00 and share not one
 * register, so "universal" cannot mean one driver that talks to any card — it
 * means a service that talks to any card FOR WHICH SOMEBODY WROTE A BACKEND,
 * and that says so plainly when nobody has.
 *
 * Everything above this line is the same for every card: finding it through
 * `pci`, retyping the frames it will reach, binding an IOSpace so its DMA is
 * contained before any address is handed to it, mapping the BAR, and answering
 * the endpoint.  Everything below it — the registers, the descriptor rings,
 * the reset sequence, how a MAC address is discovered — is per family.
 *
 * So a new card is a new `net_backend` and one more row in the table.  It is
 * not an edit to the service, and it cannot break the card that already works.
 */

/* Buffers, shared by the service and every backend: the service retypes and
 * maps them, the backend puts their PHYSICAL addresses into its ring. */
#define NET_VA_BAR   0x80A0000000ULL
#define NET_VA_RING  0x80A1000000ULL
#define NET_VA_RX    0x80A2000000ULL
#define NET_VA_TX    0x80A3000000ULL

/*
 * What the service has already arranged when a backend is called.
 *
 * The backend never retypes, never maps and never touches a capability: it is
 * handed memory that is already contained, and its whole job is the register
 * protocol of one family.  That is what keeps a second backend from being able
 * to break the first one's containment.
 */
struct net_hw {
    uint64_t bar_len;                    /* how much of the BAR was mapped */
    uint64_t ring_phys;                  /* the descriptor page */
    uint64_t rx_phys[NET_RX_FRAMES];     /* receive buffers */
    uint64_t tx_phys;                    /* the single transmit buffer */
    uint64_t mac;                        /* the backend fills this in */
    uint32_t rx_next;                    /* backend-owned ring cursor */
    uint32_t rx_skipping;                /* mid-way through an oversized frame */
    uint64_t rx_count;                   /* frames accepted, for the report */
    /* Where a backend's own bring-up stopped, in whatever numbering that
     * family finds useful.  Reported verbatim: the service does not know
     * what the numbers mean and does not need to. */
    uint32_t step;
    /*
     * The backend's own, carried from probe() through bring_up() and every
     * call after.  A family whose registers sit at FIXED offsets from its BAR
     * never touches it; a device that describes its own layout has to keep
     * what it read somewhere, and the service has no business knowing what.
     */
    uint64_t priv[8];
};

/*
 * What a backend may ask the bus BEFORE anything is claimed or mapped.
 *
 * Both are the service's, and both are READ ONLY.  A backend discovers; it
 * does not configure, and it still never holds a capability -- claiming the
 * BAR, enabling the device and containing its DMA stay above the line, where
 * one implementation serves every card.
 */
uint32_t net_bus_cfg(uint32_t index, uint32_t off);
int      net_bus_bar(uint32_t index, uint32_t bar,
                     uint64_t *base, uint64_t *size, uint32_t *flags);

struct net_backend {
    const char *name;
    uint32_t    vendor;                  /* PCI vendor this backend speaks */
    /* Does this backend drive that device id?  Asked only after the vendor
     * matches and the class says Ethernet. */
    int (*matches)(uint32_t device);
    /*
     * Which BAR holds the registers, and any discovery that has to happen
     * before it is claimed.  Returns the BAR index, or -1 to refuse the
     * device after a closer look than the id allowed.
     *
     * It exists because "the registers are in BAR 0" is an e1000 fact that
     * the service had quietly adopted as a rule.  A backend with a fixed
     * layout leaves this null and gets BAR 0; one that has to read the device
     * to find out fills in `hw->priv` here and uses it later.
     */
    int (*probe)(uint32_t index, struct net_hw *hw);
    /* Bring the card up: reset, rings, filters, link, MAC.  Returns 1 when
     * the card is usable.  A backend that cannot be sure returns 0 rather
     * than leaving a half-configured controller doing DMA. */
    int (*bring_up)(struct net_hw *hw);
    /* Send the frame already sitting in the transmit buffer.  Returns the
     * number of bytes the card reported having taken, or 0. */
    uint32_t (*send)(struct net_hw *hw, uint32_t len);
    /* The oldest received frame, as a byte offset into the receive window,
     * or 0 bytes when there is nothing. */
    uint32_t (*recv)(struct net_hw *hw, uint32_t *out_off);
};

/* The table.  One row per family somebody has actually run this against. */
extern const struct net_backend net_backend_e1000;
extern const struct net_backend net_backend_virtio;

#endif /* IRIS_NET_NETDEV_H */
