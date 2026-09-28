/* SPDX-License-Identifier: Apache-2.0 */
/*
 * net/r8169.c — the Realtek RTL8111/8168/8411 backend.
 *
 * ── Read this before trusting it ──────────────────────────
 *
 * NOT VERIFIED AGAINST SILICON.  Every other backend in this directory was
 * run before it was committed; this one cannot be, because nothing available
 * here presents the chip.  QEMU models an RTL8139, which is a different part
 * with a different descriptor model, and VirtualBox offers PCnet, Intel and
 * virtio and no Realtek at all.  It was written from the register layout the
 * family is documented with, and the machine it is FOR is the one that has
 * the card.
 *
 * That is the reason bring-up reports where it stopped rather than only
 * whether it worked: on a first run against real hardware, "it refused at the
 * reset" and "it refused because no BAR was claimable" are different problems
 * and only one of them is in this file.  The numbering is below.
 *
 * ── The C+ descriptor model ───────────────────────────────
 *
 * Unlike the 8139 before it, this family does not have a single receive
 * FIFO the driver reads out of.  It walks RINGS of sixteen-byte descriptors
 * whose physical addresses the driver writes into two register pairs, and it
 * finds the end of a ring by a BIT IN THE LAST DESCRIPTOR rather than by a
 * length register — which is why a ring of eight works exactly as well as the
 * ring of two hundred and fifty-six a general-purpose driver would build.
 *
 * Polled, like every other backend here.
 */
#include <stdint.h>
#include "netdev.h"
#include <iris/syscall.h>
#include <iris/pci_ep_proto.h>

#define REALTEK_VENDOR   0x10ECu

/* ── registers, at byte offsets from the mapped BAR ──────────────────────── */
#define R_IDR0           0x00u   /* the MAC address, six bytes              */
#define R_MAR0           0x08u   /* the multicast filter, eight bytes       */
#define R_TNPDS          0x20u   /* transmit ring, 64-bit, 256-byte aligned */
#define R_CR             0x37u   /* command: reset, receive, transmit       */
#define R_TPPOLL         0x38u   /* "look at the transmit ring now"         */
#define R_IMR            0x3Cu
#define R_ISR            0x3Eu
#define R_TCR            0x40u
#define R_RCR            0x44u
#define R_CFG9346        0x50u   /* the lock over the configuration bytes   */
#define R_PHYSTATUS      0x6Cu
#define R_RMS            0xDAu   /* the largest frame it may receive        */
#define R_CPCR           0xE0u   /* C+ command                              */
#define R_RDSAR          0xE4u   /* receive ring, 64-bit, 256-byte aligned  */
#define R_MTPS           0xECu   /* the largest frame it may send, /128     */

#define CR_RST           0x10u
#define CR_RE            0x08u
#define CR_TE            0x04u

#define TPPOLL_NPQ       0x40u   /* the normal-priority queue               */

#define CFG9346_UNLOCK   0xC0u
#define CFG9346_LOCK     0x00u

#define RCR_APM          (1u << 1)    /* frames addressed to our MAC        */
#define RCR_AB           (1u << 3)    /* broadcast, which is what ARP is    */
#define RCR_MXDMA_UNLIM  (7u << 8)
#define RCR_RXFTH_NONE   (7u << 13)

#define TCR_MXDMA_UNLIM  (7u << 8)
#define TCR_IFG_NORMAL   (3u << 24)

#define PHY_LINK_OK      0x02u

/* Descriptor opts1, which is both the command and the result. */
#define D_OWN            (1u << 31)   /* the CARD owns this descriptor      */
#define D_EOR            (1u << 30)   /* the ring wraps after this one      */
#define D_FS             (1u << 29)   /* first fragment of a frame          */
#define D_LS             (1u << 28)   /* last fragment                      */
#define D_RXERR          (1u << 21)
#define D_LEN_MASK       0x3FFFu

/*
 * The rings, in the page the service retyped.  Both starts must be aligned
 * to 256 bytes, which a page-aligned frame gives for free at 0x000 and 0x100.
 */
#define NET_RING_LEN     8u
#define RX_RING_OFF      0x000u   /* 8 * 16 = 128 bytes */
#define TX_RING_OFF      0x100u

/* The frame check sequence is left on the end of a received frame by this
 * family — there is no strip-CRC bit in the receive configuration — so the
 * length the card reports is four bytes longer than the frame. */
#define FCS_BYTES        4u

#define NET_TX_MS        1000u

/*
 * Where bring-up stopped, in `hw->step`, reported through NET_OP_INFO.
 *   1 no memory BAR large enough to be handed out as a frame
 *   2 the BAR the service mapped is too small for the register file
 *   3 the reset never completed
 *   4 the card did not come out of reset with transmit and receive on
 *  12 up, and the PHY reports a link
 *  13 up, and the PHY reports no link — a cable, not this file
 */

struct r8169_priv {
    uint32_t bar;
    uint32_t tx_cur;
};
_Static_assert(sizeof(struct r8169_priv) <= sizeof(((struct net_hw *)0)->priv),
               "r8169 state does not fit in the backend's private words");

/* ── register access ─────────────────────────────────────────────────────── */

static volatile uint8_t *reg(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(NET_VA_BAR + off);
}
static uint8_t  r8 (uint32_t o) { return *(volatile uint8_t  *)reg(o); }
static uint16_t r16(uint32_t o) { return *(volatile uint16_t *)reg(o); }
static void w8 (uint32_t o, uint8_t  v) { *(volatile uint8_t  *)reg(o) = v; }
static void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)reg(o) = v; }
static void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)reg(o) = v; }
static void w64(uint32_t o, uint64_t v) {
    /* Low half first: the card latches the address on the high write, so the
     * other order hands it half of one address and half of the last one. */
    w32(o,      (uint32_t)v);
    w32(o + 4u, (uint32_t)(v >> 32));
}

/* ── descriptors ─────────────────────────────────────────────────────────── */

static volatile uint32_t *desc(uint32_t ring, uint32_t i) {
    return (volatile uint32_t *)(uintptr_t)(NET_VA_RING + ring + i * 16u);
}
static void desc_set(uint32_t ring, uint32_t i, uint64_t addr, uint32_t opts1) {
    volatile uint32_t *d = desc(ring, i);
    /* The address before the ownership bit, always: the card may read the
     * descriptor the instant OWN appears, and a stale address is a transfer
     * into whatever used to be there. */
    d[2] = (uint32_t)addr;
    d[3] = (uint32_t)(addr >> 32);
    d[1] = 0u;
    __asm__ volatile ("" ::: "memory");
    d[0] = opts1;
}

/* ── which card, and which BAR ───────────────────────────────────────────── */

/*
 * The family, by device id.  All of these speak the C+ descriptor model
 * above; what differs between them is which BAR the registers landed in and
 * how fast the PHY is, and `probe` answers the first from the machine rather
 * than from a table.
 */
static int r8169_matches(uint32_t device) {
    return device == 0x8168u ||   /* RTL8111 / 8168 / 8411, the common one  */
           device == 0x8161u ||   /* the same part on an add-in card        */
           device == 0x8136u ||   /* RTL8101 / 8102 / 8106, 100 Mbit        */
           device == 0x8169u;     /* the original gigabit part              */
}

/*
 * WHICH BAR, asked of the machine.
 *
 * The register file is 256 bytes and every member of the family puts it in a
 * memory BAR, but not the same one: the 8168 uses BAR 2, the 8169 and the
 * 8101 use BAR 1, and a table of that is a table that goes wrong on the next
 * part.  So this takes the first memory BAR the bus service could carve a
 * frame over, which is the only kind it can hand out anyway.
 *
 * A part whose BAR is smaller than a page cannot be reached this way at all
 * — a frame is a whole number of pages — and that is reported rather than
 * worked around, because the fix is in the window mechanism and not here.
 */
static int r8169_probe(uint32_t index, struct net_hw *hw) {
    struct r8169_priv *p = (struct r8169_priv *)hw->priv;
    for (uint32_t i = 0; i < sizeof(hw->priv) / sizeof(hw->priv[0]); i++)
        hw->priv[i] = 0;

    hw->step = 1u;
    for (uint32_t b = 0; b < 6u; b++) {
        uint64_t base = 0, size = 0;
        uint32_t flags = 0;
        if (!net_bus_bar(index, b, &base, &size, &flags)) continue;
        if (!(flags & PCI_BAR_MMIO)) continue;
        /* CLAIMABLE is the bus service saying it carved a frame over this
         * window.  It is the actual question -- a BAR that is memory and big
         * enough but landed somewhere no device Untyped covers cannot be
         * handed out, and asking about the size instead would pick it and
         * then fail one step later with a worse error. */
        if (!(flags & PCI_BAR_CLAIMABLE)) continue;
        if (base == 0u || size < 4096u) continue;
        p->bar = b;
        hw->step = 2u;
        return (int)b;
    }
    return -1;
}

/* ── bring-up ────────────────────────────────────────────────────────────── */

static int r8169_bring_up(struct net_hw *hw) {
    struct r8169_priv *p = (struct r8169_priv *)hw->priv;
    if (hw->bar_len < 0x100u) { hw->step = 2u; return 0; }

    /*
     * Reset, and WAIT for it.  The bit clears itself when the card is done,
     * and everything below writes registers the reset would otherwise wipe.
     */
    hw->step = 3u;
    w8(R_CR, CR_RST);
    {
        long t0 = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
        for (;;) {
            if (!(r8(R_CR) & CR_RST)) break;
            long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
            if (t0 > 0 && now > 0 &&
                (uint64_t)(now - t0) > 100ull * 1000000ull) return 0;
        }
    }

    /* No interrupt is enabled, and whatever the reset left pending is
     * acknowledged: the status register clears by writing ones back. */
    w16(R_IMR, 0u);
    w16(R_ISR, 0xFFFFu);

    /* The MAC, before anything is written over it.  Six bytes, low first,
     * packed the way the endpoint reports it. */
    {
        uint64_t mac = 0;
        for (uint32_t i = 0; i < 6u; i++)
            mac |= (uint64_t)r8(R_IDR0 + i) << (8u * i);
        hw->mac = mac;
    }

    w8(R_CFG9346, CFG9346_UNLOCK);

    /* C+ mode is how this family works at all; read the register and put it
     * back rather than composing one, because the reset value carries bits
     * that differ per revision and this driver has no opinion about them. */
    w16(R_CPCR, r16(R_CPCR));

    /* The two size limits, in the two different units the card uses for
     * them: bytes for receive, and 128-byte units for transmit. */
    w16(R_RMS, (uint16_t)(NET_FRAME_BYTES + FCS_BYTES));
    w8(R_MTPS, 0x3Bu);

    /* Nothing is accepted by the multicast filter.  Broadcast is a separate
     * bit in the receive configuration and that is what ARP needs. */
    w32(R_MAR0, 0u);
    w32(R_MAR0 + 4u, 0u);

    /*
     * The receive ring: one descriptor per buffer, the card owns them all,
     * and the last one carries the bit that says where the ring ends.
     */
    for (uint32_t i = 0; i < NET_RING_LEN; i++) {
        uint64_t at = hw->rx_phys[i / NET_RX_PER_FRAME] +
                      (uint64_t)(i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;
        uint32_t o = D_OWN | (uint32_t)NET_FRAME_BYTES;
        if (i == NET_RING_LEN - 1u) o |= D_EOR;
        desc_set(RX_RING_OFF, i, at, o);
    }
    /* The transmit ring starts empty — the DRIVER owns every descriptor —
     * and the same end marker tells the card where it wraps. */
    for (uint32_t i = 0; i < NET_RING_LEN; i++)
        desc_set(TX_RING_OFF, i, 0u, (i == NET_RING_LEN - 1u) ? D_EOR : 0u);
    p->tx_cur = 0u;
    hw->rx_next = 0u;

    w64(R_RDSAR, hw->ring_phys + RX_RING_OFF);
    w64(R_TNPDS, hw->ring_phys + TX_RING_OFF);

    w8(R_CFG9346, CFG9346_LOCK);

    /* Rings first, then the engines, then what they may accept: a card told
     * to receive before it has a ring has nowhere to put the first frame. */
    hw->step = 4u;
    w8(R_CR, CR_RE | CR_TE);
    if ((r8(R_CR) & (CR_RE | CR_TE)) != (CR_RE | CR_TE)) return 0;

    w32(R_TCR, TCR_MXDMA_UNLIM | TCR_IFG_NORMAL);
    w32(R_RCR, RCR_APM | RCR_AB | RCR_MXDMA_UNLIM | RCR_RXFTH_NONE);

    /*
     * The link, reported and not waited for.  Autonegotiation on real copper
     * takes seconds, and a driver that refused to come up until it finished
     * would report a broken card for an unplugged cable — which is the one
     * failure that is not this file's.
     */
    hw->step = (r8(R_PHYSTATUS) & PHY_LINK_OK) ? 12u : 13u;
    return 1;
}

/* ── moving a frame ──────────────────────────────────────────────────────── */

static uint32_t r8169_send(struct net_hw *hw, uint32_t len) {
    struct r8169_priv *p = (struct r8169_priv *)hw->priv;
    if (len == 0u || len > NET_FRAME_BYTES) return 0;

    uint32_t i = p->tx_cur % NET_RING_LEN;
    uint32_t o = D_OWN | D_FS | D_LS | (len & D_LEN_MASK);
    if (i == NET_RING_LEN - 1u) o |= D_EOR;
    desc_set(TX_RING_OFF, i, hw->tx_phys, o);

    /* The card does not poll its own ring; it is told to look. */
    w8(R_TPPOLL, TPPOLL_NPQ);

    /*
     * Bounded by TIME and not by a count of reads, for the reason the e1000
     * backend gives at length: a count is a different amount of patience on
     * every machine.  A clock that will not answer leaves this unbounded
     * rather than instantaneous.
     */
    {
        long t0 = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
        for (;;) {
            if (!(desc(TX_RING_OFF, i)[0] & D_OWN)) {
                p->tx_cur = (i + 1u) % NET_RING_LEN;
                return len;
            }
            long now = iris_syscall4(SYS_CLOCK_GET, 0, 0, 0, 0);
            if (t0 > 0 && now > 0 &&
                (uint64_t)(now - t0) > (uint64_t)NET_TX_MS * 1000000ull) break;
        }
    }
    return 0;
}

static uint32_t r8169_recv(struct net_hw *hw, uint32_t *out_off) {
    uint32_t i = hw->rx_next;
    uint32_t o = desc(RX_RING_OFF, i)[0];
    if (o & D_OWN) return 0;                 /* still the card's */

    uint32_t len = o & D_LEN_MASK;

    /*
     * A frame longer than one buffer is SPLIT across descriptors, and only
     * the last of them carries the end marker.  Dropping the pieces without
     * it is not enough: the last piece looks like a whole short frame and
     * would be handed up as a tail with no Ethernet header — a worse failure
     * than losing it, because the layer above cannot recognise it.
     *
     * So a split frame is dropped WHOLE, exactly as the e1000 backend does:
     * once a piece arrives that is not both first and last, everything up to
     * and including the next end marker is discarded.
     */
    if (hw->rx_skipping) {
        if (o & D_LS) hw->rx_skipping = 0u;
        len = 0u;
    } else if ((o & (D_FS | D_LS)) != (D_FS | D_LS)) {
        hw->rx_skipping = 1u;
        len = 0u;
    } else if (o & D_RXERR) {
        len = 0u;
    } else if (len <= FCS_BYTES || len - FCS_BYTES > NET_FRAME_BYTES) {
        len = 0u;
    } else {
        /* The frame check sequence is still on the end of it. */
        len -= FCS_BYTES;
    }

    *out_off = (i / NET_RX_PER_FRAME) * 4096u +
               (i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;

    /* Hand the descriptor back: address and length first, then ownership,
     * which `desc_set` orders for exactly this reason. */
    {
        uint64_t at = hw->rx_phys[i / NET_RX_PER_FRAME] +
                      (uint64_t)(i % NET_RX_PER_FRAME) * NET_FRAME_BYTES;
        uint32_t back = D_OWN | (uint32_t)NET_FRAME_BYTES;
        if (i == NET_RING_LEN - 1u) back |= D_EOR;
        desc_set(RX_RING_OFF, i, at, back);
    }
    hw->rx_next = (i + 1u) % NET_RING_LEN;
    if (len) hw->rx_count++;
    return len;
}

const struct net_backend net_backend_r8169 = {
    .name     = "Realtek RTL8111/8168/8411",
    .vendor   = REALTEK_VENDOR,
    .matches  = r8169_matches,
    .probe    = r8169_probe,
    .bring_up = r8169_bring_up,
    .send     = r8169_send,
    .recv     = r8169_recv,
};
